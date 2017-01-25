#include "poncos/controller_vm.hpp"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <thread>
#include <utility>

#include "poncos/poncos.hpp"

#include <uuid/uuid.h>

// inititalize fast-lib log
FASTLIB_LOG_INIT(vm_controller_log, "vm-controller")
FASTLIB_LOG_SET_LEVEL_GLOBAL(vm_controller_log, info);

vm_controller::vm_controller(const std::shared_ptr<fast::MQTT_communicator> &_comm, const std::string &machine_filename,
							 std::string _slot_path)
	: controllerT(_comm, machine_filename), slot_path(std::move(_slot_path)) {
	// subscribe to the various topics
	for (const std::string &mach : machines) {
		std::string topic = "fast/migfra/" + mach + "/result";
		comm->add_subscription(topic);
	}
}

vm_controller::~vm_controller() = default;

void vm_controller::init() { start_all_VMs(); }

void vm_controller::dismantle() { stop_all_VMs(); }

void vm_controller::freeze(const size_t id) {
	assert(id < id_to_config.size());

	const execute_config &config = id_to_config[id];
	suspend_resume_virt_cluster<fast::msg::migfra::Suspend>(config);
}

void vm_controller::thaw(const size_t id) {
	assert(id < id_to_config.size());

	const execute_config &config = id_to_config[id];
	suspend_resume_virt_cluster<fast::msg::migfra::Resume>(config);
}

void vm_controller::freeze_opposing(const size_t id) {
	const execute_config opposing_config = generate_opposing_config(id);

	suspend_resume_virt_cluster<fast::msg::migfra::Suspend>(opposing_config);
}

void vm_controller::thaw_opposing(const size_t id) {
	const execute_config &opposing_config = generate_opposing_config(id);

	suspend_resume_virt_cluster<fast::msg::migfra::Resume>(opposing_config);
}

void vm_controller::update_config(const size_t id, const execute_config &new_config) {
	timestamps.tick("update-config-job-#" + std::to_string(id));
	const execute_config &old_config = id_to_config[id];
	assert(old_config.size() == new_config.size());

	// request the swap of slots pair-wise
	for (size_t idx = 0; idx < new_config.size(); ++idx) {
		const size_t src_host_idx = old_config[idx].first;
		const size_t dest_host_idx = new_config[idx].first;
		const size_t src_slot = old_config[idx].second;
		const size_t dest_slot = new_config[idx].second;

		// source and destination host are the same
		if (src_host_idx == dest_host_idx) {
			assert(src_slot == dest_slot);
			continue;
		}

		const std::string &src_host = machines[src_host_idx];
		const std::string &dest_host = machines[dest_host_idx];
		const std::string &src_guest = vm_locations[src_host_idx][src_slot];
		const std::string &dest_guest = vm_locations[dest_host_idx][dest_slot];

		const jobT &src_job = id_to_job[id];
		const size_t dest_job_id = machine_usage[dest_host_idx][dest_slot];
		const jobT &dest_job = id_to_job[dest_job_id];

		// generate migrate task
		std::string topic = "fast/migfra/" + src_host + "/task";
		auto task = std::make_shared<fast::msg::migfra::Migrate>(src_guest, dest_host, "warm", false, true, 0, false);
		task->swap_with = fast::msg::migfra::Swap_with();
		task->swap_with.get().vm_name = dest_guest;
		if (src_slot != dest_slot) {
			task->vcpu_map.get() = generate_vcpu_map(dest_slot);
			task->swap_with.get().vcpu_map.get() = generate_vcpu_map(src_slot);
		}

		// set pscom-hook-procs if necessary
		// we assume evently distributed processes across the VMs
		// TODO: handle remainder of the division
		if (src_job.uses_sr_protocol) {
			const size_t src_proc_count = src_job.nprocs / old_config.size();
			task->pscom_hook_procs.get() = std::to_string(src_proc_count);
		}
		if (dest_job.uses_sr_protocol) {
			const size_t dest_proc_count = dest_job.nprocs / id_to_config[dest_job_id].size();
			task->swap_with.get().pscom_hook_procs = std::to_string(dest_proc_count);
		}

		// put into task container
		fast::msg::migfra::Task_container m;
		m.tasks.push_back(task);

		FASTLIB_LOG(vm_controller_log, debug) << "sending message \n topic: " << topic << "\n message:\n"
											  << m.to_string();

		timestamps.tick("swap-" + src_host + "-" + dest_host + "-job-#" + std::to_string(id));
		comm->send_message(m.to_string(), topic);
	}

	// wait for results
	fast::msg::migfra::Result_container response;
	for (size_t idx = 0; idx < new_config.size(); ++idx) {
		const size_t src_host_idx = old_config[idx].first;
		const size_t dest_host_idx = new_config[idx].first;
		const size_t src_slot = old_config[idx].second;
		const size_t dest_slot = new_config[idx].second;

		const std::string &src_host = machines[src_host_idx];
		const std::string &dest_host = machines[dest_host_idx];

		// source and destination host are the same
		if (src_host_idx == dest_host_idx) {
			continue;
		}

		// wait for VMs to be migrated
		std::string topic = "fast/migfra/" + machines[src_host_idx] + "/result";
		response.from_string(comm->get_message(topic));
		timestamps.tock("swap-" + src_host + "-" + dest_host + "-job-#" + std::to_string(id));
		assert(response.results.front().status == "success");

		// update slot allocations
		std::swap(vm_locations[src_host_idx][src_slot], vm_locations[dest_host_idx][dest_slot]);
	}

	// update id_to_config for all affected jobs and machine_usage.
	controllerT::update_config(id, new_config);

	timestamps.tock("update-config-job-#" + std::to_string(id));
}

std::vector<std::vector<unsigned int>> vm_controller::generate_vcpu_map(size_t slot_id) const {
	std::vector<std::vector<unsigned int>> vcpu_map;
	vcpu_map.reserve(SLOT_SIZE);

	for (auto cpu_id : co_configs[slot_id].cpus) {
		vcpu_map.emplace_back(std::vector<unsigned int>({static_cast<unsigned int>(cpu_id)}));
	}
	assert(vcpu_map.size() == SLOT_SIZE);

	return vcpu_map;
}

std::string vm_controller::generate_command(const jobT &job, size_t /*counter*/, const execute_config &config) const {
	std::string host_list;
	for (const auto &config_elem : config) {
		host_list += vm_locations[config_elem.first][config_elem.second] + ",";
	}
	// remove last ','
	host_list.pop_back();

	return "mpiexec -np " + std::to_string(job.nprocs) + " -genv OMP_NUM_THREADS " +
		   std::to_string(job.threads_per_proc) + " -hosts " + host_list + " " + job.command;
}

// generates start task for a single VM
std::shared_ptr<fast::msg::migfra::Start> vm_controller::generate_start_task(size_t slot, vm_pool_elemT &free_vm) {
	// load XML
	std::fstream slot_file;
	slot_file.open(slot_path + "/slot-" + std::to_string(slot) + ".xml");
	std::stringstream slot_stream;
	slot_stream << slot_file.rdbuf();
	std::string slot_xml = slot_stream.str();

	// modify XML
	// -- vm name
	std::regex name_regex("(<name>)(.+)(</name>)");
	slot_xml = std::regex_replace(slot_xml, name_regex, "$1" + free_vm.name + "$3");
	// -- disc
	std::regex disk_regex(R"((.*<source file=".*)(parastation-.*)([.]qcow2"/>))");
	slot_xml = std::regex_replace(slot_xml, disk_regex, "$1" + free_vm.name + "$3");

	// -- uuid
	uuid_t uuid;
	uuid_generate(uuid);
	char uuid_char_str[40];
	uuid_unparse(uuid, uuid_char_str);
	std::string uuid_str(static_cast<const char *>(uuid_char_str));
	std::regex uuid_regex("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}");
	slot_xml = std::regex_replace(slot_xml, uuid_regex, uuid_str);
	// -- mac
	std::regex mac_regex("([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})");
	slot_xml = std::regex_replace(slot_xml, mac_regex, free_vm.mac_addr);

	// generate start task and return
	std::vector<fast::msg::migfra::PCI_id> pci_ids;
	pci_ids.emplace_back(0x15b3, 0x1004);

	auto start_task = std::make_shared<fast::msg::migfra::Start>(slot_xml, pci_ids, true);
	start_task->transient = true;

	return start_task;
}

template <typename T> void vm_controller::suspend_resume_virt_cluster(const execute_config &config) {
	// request OP
	for (auto config_elem : config) {
		std::string topic = "fast/migfra/" + machines[config_elem.first] + "/task";

		auto task = std::make_shared<T>(vm_locations[config_elem.first][config_elem.second], true);

		fast::msg::migfra::Task_container m;
		m.tasks.push_back(task);

		FASTLIB_LOG(vm_controller_log, debug) << "sending message \n topic: " << topic << "\n message:\n"
											  << m.to_string();

		comm->send_message(m.to_string(), topic);
	}

	// wait for results
	fast::msg::migfra::Result_container response;
	for (auto config_elem : config) {
		// wait for VMs to be started
		std::string topic = "fast/migfra/" + machines[config_elem.first] + "/result";
		response.from_string(comm->get_message(topic));

		if (response.results.front().status != "success") {
			assert(response.results.front().details !=
				   "Error suspending domain: Requested operation is not valid: domain is not running");
		}
	}
}

void vm_controller::start_all_VMs() {
	for (const auto &mach : machines) {
		std::string topic = "fast/migfra/" + mach + "/task";

		// create task container and add tasks per slot
		fast::msg::migfra::Task_container m;
		std::array<std::string, SLOTS> cur_slot_allocation;
		for (size_t slot = 0; slot < SLOTS; ++slot) {
			// get free vm
			vm_pool_elemT free_vm = glob_vm_pool.front();
			glob_vm_pool.pop_front();

			// generate task
			auto task = generate_start_task(slot, free_vm);

			// update vm_locations
			cur_slot_allocation[slot] = free_vm.name;
			m.tasks.push_back(task);
		}

		// send start request
		comm->send_message(m.to_string(), topic);

		// add slot allocation to vm_locations
		vm_locations.push_back(cur_slot_allocation);
	}

	fast::msg::migfra::Result_container response;
	for (const auto &mach : machines) {
		// wait for VMs to be started
		std::string topic = "fast/migfra/" + mach + "/result";
		response.from_string(comm->get_message(topic));

		// check success for each result
		for (auto result : response.results) {
			assert(result.status == "success");
		}
	}
}

void vm_controller::stop_all_VMs() {
	// request stop of all VMs per host
	size_t mach_id = 0;
	for (const auto &mach : machines) {
		// generate stop tasks
		fast::msg::migfra::Task_container m;
		for (size_t slot = 0; slot < SLOTS; ++slot) {
			auto task = std::make_shared<fast::msg::migfra::Stop>(vm_locations[mach_id][slot], false, false, true);
			m.tasks.push_back(task);
		}

		// send stop request
		std::string topic = "fast/migfra/" + mach + "/task";
		FASTLIB_LOG(vm_controller_log, debug) << "sending message \n topic: " << topic << "\n message:\n"
											  << m.to_string();
		comm->send_message(m.to_string(), topic);

		mach_id++;
	}

	// wait for completion
	fast::msg::migfra::Result_container response;
	for (const auto &mach : machines) {
		std::string topic = "fast/migfra/" + mach + "/result";
		response.from_string(comm->get_message(topic));
		for (auto result : response.results) {
			assert(result.status == "success");
		}
	}
}
