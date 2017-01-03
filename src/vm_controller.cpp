#include "poncos/vm_controller.hpp"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <thread>

#include "poncos/poncos.hpp"

#include <uuid/uuid.h>

vm_controller::vm_controller(const std::shared_ptr<fast::MQTT_communicator> &_comm, const std::string &machine_filename,
							 const std::string &_slot_path)
	: controllerT(_comm, machine_filename), slot_path(_slot_path) {
	// subscribe to the various topics
	for (std::string mach : machines) {
		std::string topic = "fast/migfra/" + mach + "/result";
		comm->add_subscription(topic);
	}
}

vm_controller::~vm_controller() {}

void vm_controller::init() { start_all_VMs(); }

void vm_controller::dismantle() { stop_all_VMs(); }

void vm_controller::freeze(const size_t id) {
	auto t = id_to_slot.find(id);

	assert(t != id_to_slot.end());
	size_t slot = t->second;
	suspend_resume_virt_cluster<fast::msg::migfra::Suspend>(slot);
}

void vm_controller::thaw(const size_t id) {
	auto t = id_to_slot.find(id);

	assert(t != id_to_slot.end());
	size_t slot = t->second;
	suspend_resume_virt_cluster<fast::msg::migfra::Resume>(slot);
}

size_t vm_controller::execute(const jobT &job, const execute_config &config, std::function<void(size_t)> callback) {
	assert(config.size() > 0);
	// we currently only support the same slot for all configs
	{
		assert(config.size() == machines.size());
		size_t compare = config[0].second;
		for (size_t i = 1; i < config.size(); ++i) {
			assert(compare == config[i].second);
		}
	}
	assert(work_counter_lock.owns_lock());

	assert(config.size() >= free_slots);
	free_slots -= config.size();

	id_to_pool.emplace(cmd_counter, thread_pool.size());
	id_to_slot.emplace(cmd_counter, config[0].second);

	std::string cmd_name = "cmd_" + std::to_string(cmd_counter);

	const std::string command = generate_command(job, config[0].second);
	thread_pool.emplace_back(&vm_controller::execute_command_internal, this, command, cmd_name, config, callback);

	return cmd_counter++;
}

// executed by a new thread, calls system to start the application
void vm_controller::execute_command_internal(std::string command, std::string cg_name, const execute_config config,
											 std::function<void(size_t)> callback) {
	command += " 2>&1 ";
	// command += "| tee ";
	command += "> ";
	command += cg_name + ".log";

	auto temp = system(command.c_str());
	assert(temp != -1);

	// we are done
	std::cout << ">> \t '" << command << "' completed at configuration " << config[0].second << std::endl;

	std::lock_guard<std::mutex> work_counter_lock(worker_counter_mutex);
	free_slots += config.size();
	assert(free_slots <= total_available_slots);
	callback(config[0].second);
	worker_counter_cv.notify_one();
}

std::string vm_controller::generate_command(const jobT &job, const size_t slot) {
	std::string host_list;
	for (auto virt_cluster_node : virt_cluster[slot]) {
		host_list += virt_cluster_node.second + ",";
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
	std::regex disk_regex("(.*<source file=\".*)(parastation-.*)([.]qcow2\"/>)");
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
	pci_ids.push_back(fast::msg::migfra::PCI_id(0x15b3, 0x1004));

	return std::make_shared<fast::msg::migfra::Start>(slot_xml, pci_ids, true);
}

template <typename T> void vm_controller::suspend_resume_virt_cluster(size_t slot) {
	// request freeze
	for (auto cluster_elem : virt_cluster[slot]) {
		std::string topic = "fast/migfra/" + cluster_elem.first + "/task";

		auto task = std::make_shared<T>(cluster_elem.second, true);

		fast::msg::migfra::Task_container m;
		m.tasks.push_back(task);

		std::cout << "sending message \n topic: " << topic << "\n message:\n" << m.to_string() << std::endl;

		comm->send_message(m.to_string(), topic);
	}

	// wait for results
	fast::msg::migfra::Result_container response;
	for (auto cluster_elem : virt_cluster[slot]) {
		// wait for VMs to be started
		std::string topic = "fast/migfra/" + cluster_elem.first + "/result";
		response.from_string(comm->get_message(topic));

		int status = response.results.front().status.compare("success");
		if (status) {
			assert(!response.results.front().details.compare(
				"Error suspending domain: Requested operation is not valid: domain is not running"));
		}
	}
}

void vm_controller::start_all_VMs() {
	for (auto mach : machines) {
		std::string topic = "fast/migfra/" + mach + "/task";

		// create task container and add tasks per slot
		fast::msg::migfra::Task_container m;
		std::cout << "> Prepare task container for " << mach << std::endl;
		for (size_t slot = 0; slot < SLOTS; ++slot) {
			// get free vm
			vm_pool_elemT free_vm = glob_vm_pool.front();
			glob_vm_pool.pop_front();

			auto task = generate_start_task(slot, free_vm);
			virt_cluster[slot].push_back({mach, free_vm.name});
			std::cout << "> add task to container " << free_vm.name << std::endl;
			m.tasks.push_back(task);
		}

		comm->send_message(m.to_string(), topic);
	}

	fast::msg::migfra::Result_container response;
	for (auto mach : machines) {
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
	std::unordered_map<std::string, std::vector<std::string>> cluster_layout;

	// collect VMs per host
	for (size_t slot = 0; slot < SLOTS; ++slot) {
		// send stop request
		for (auto cluster_node : virt_cluster[slot]) {
			cluster_layout[cluster_node.first].push_back(cluster_node.second);
		}
	}

	for (auto host_vms : cluster_layout) {
		fast::msg::migfra::Task_container m;
		for (auto vm : host_vms.second) {
			auto task = std::make_shared<fast::msg::migfra::Stop>(vm, false, true, true);
			m.tasks.push_back(task);
		}

		std::string topic = "fast/migfra/" + host_vms.first + "/task";
		std::cout << "sending message \n topic: " << topic << "\n message:\n" << m.to_string() << std::endl;
		comm->send_message(m.to_string(), topic);
	}

	// wait for completion
	fast::msg::migfra::Result_container response;
	for (auto host_vms : cluster_layout) {
		std::string topic = "fast/migfra/" + host_vms.first + "/result";
		response.from_string(comm->get_message(topic));
		for (auto result : response.results) {
			assert(!result.status.compare("success"));
		}
	}

	// clear virt_cluster
	for (size_t slot = 0; slot < SLOTS; ++slot) {
		virt_cluster[slot].clear();
	}
}
