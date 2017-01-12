#include "poncos/vm_controller.hpp"

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

		// generate migrate task and put into task container
		std::string topic = "fast/migfra/" + src_host + "/task";
		auto task =
			std::make_shared<fast::msg::migfra::Migrate>(src_guest, dest_host, "warm", true, true, "auto", false);
		task->swap_with = dest_guest;
		fast::msg::migfra::Task_container m;
		m.tasks.push_back(task);

		FASTLIB_LOG(vm_controller_log, debug) << "sending message \n topic: " << topic << "\n message:\n" << m.to_string();

		comm->send_message(m.to_string(), topic);
	}

	// wait for results
	fast::msg::migfra::Result_container response;
	for (size_t idx = 0; idx < new_config.size(); ++idx) {
		const size_t src_host_idx = old_config[idx].first;
		const size_t dest_host_idx = new_config[idx].first;
		const size_t src_slot = old_config[idx].second;
		const size_t dest_slot = new_config[idx].second;

		// source and destination host are the same
		if (src_host_idx == dest_host_idx) {
			continue;
		}

		// wait for VMs to be migrated
		std::string topic = "fast/migfra/" + machines[src_host_idx] + "/result";
		response.from_string(comm->get_message(topic));
		assert(!response.results.front().status.compare("success"));

		// update slot allocations
		std::swap(vm_locations[src_host_idx][src_slot], vm_locations[dest_host_idx][dest_slot]);
	}

	// update id_to_config for all affected jobs and machine_usage.
	controllerT::update_config(id, new_config);
}

std::string vm_controller::generate_command(const jobT &job, size_t /*counter*/, const execute_config &config) const {
	std::string host_list;
	for (const auto &config_elem : config) {
		host_list += vm_locations[config_elem.first][config_elem.second] + ",";
	}
	// remove last ','
	host_list.pop_back();

	return "mpiexec -np " + std::to_string(job.nprocs) + " -hosts " + host_list + " " + job.command;
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

	return std::make_shared<fast::msg::migfra::Start>(slot_xml, pci_ids, true);
}

template <typename T> void vm_controller::suspend_resume_virt_cluster(const execute_config &config) {
	// request OP
	for (auto config_elem : config) {
		std::string topic = "fast/migfra/" + machines[config_elem.first] + "/task";

		auto task = std::make_shared<T>(vm_locations[config_elem.first][config_elem.second], true);

		fast::msg::migfra::Task_container m;
		m.tasks.push_back(task);

		FASTLIB_LOG(vm_controller_log, debug) << "sending message \n topic: " << topic << "\n message:\n" << m.to_string();

		comm->send_message(m.to_string(), topic);
	}

	// wait for results
	fast::msg::migfra::Result_container response;
	for (auto config_elem : config) {
		// wait for VMs to be started
		std::string topic = "fast/migfra/" + machines[config_elem.first] + "/result";
		response.from_string(comm->get_message(topic));

		int status = response.results.front().status.compare("success");
		if (status) {
			assert(!response.results.front().details.compare(
				"Error suspending domain: Requested operation is not valid: domain is not running"));
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
			assert(!result.status.compare("success"));
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
			auto task = std::make_shared<fast::msg::migfra::Stop>(vm_locations[mach_id][slot], false, true, true);
			m.tasks.push_back(task);
		}

		// send stop request
		std::string topic = "fast/migfra/" + mach + "/task";
		FASTLIB_LOG(vm_controller_log, debug) << "sending message \n topic: " << topic << "\n message:\n" << m.to_string();
		comm->send_message(m.to_string(), topic);

		mach_id++;
	}

	// wait for completion
	fast::msg::migfra::Result_container response;
	for (const auto &mach : machines) {
		std::string topic = "fast/migfra/" + mach + "/result";
		response.from_string(comm->get_message(topic));
		for (auto result : response.results) {
			assert(!result.status.compare("success"));
		}
	}
}
