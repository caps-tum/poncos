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

#include <fast-lib/message/migfra/pci_id.hpp>
#include <fast-lib/message/migfra/result.hpp>
#include <fast-lib/message/migfra/task.hpp>

#include <uuid/uuid.h>

vm_controller::vm_controller(const std::shared_ptr<fast::MQTT_communicator> &_comm, const std::string &machine_filename,
							 const std::string &_slot_path)
	: work_counter_lock(worker_counter_mutex), workers_active(0), cmd_counter(0), slot_path(_slot_path), comm(_comm) {

	// fill the machine file
	std::cout << "Reading machine file " << machine_filename << " ...";
	std::cout.flush();
	read_file(machine_filename, _machines);
	std::cout << " done!" << std::endl;

	std::cout << "Machine file:\n";
	std::cout << "==============\n";
	for (std::string c : machines()) {
		std::cout << c << "\n";
	}
	std::cout << "==============\n";

	// subscribe to the various topics
	for (std::string mach : machines()) {
		std::string topic = "fast/migfra/" + mach + "/result";
		comm->add_subscription(topic);
	}
}

vm_controller::~vm_controller() {
	done();
	for (auto &t : thread_pool) {
		if (t.joinable()) t.join();
	}
	thread_pool.resize(0);
}

void vm_controller::init() { start_all_VMs(); }

void vm_controller::dismantle() { stop_all_VMs(); }

void vm_controller::done() {
	// wait until all workers are finished
	if (!work_counter_lock.owns_lock()) work_counter_lock.lock();
	worker_counter_cv.wait(work_counter_lock, [&] { return workers_active == 0; });
}

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

void vm_controller::wait_for_ressource() {
	if (!work_counter_lock.owns_lock()) work_counter_lock.lock();

	worker_counter_cv.wait(work_counter_lock, [&] { return workers_active < SLOTS; });
}

void vm_controller::wait_for_completion_of(const size_t id) {
	work_counter_lock.unlock();

	auto t = id_to_pool.find(id);

	assert(t != id_to_pool.end());
	auto i = t->second;

	// std::cout << "0: wait for old" << std::endl;
	thread_pool[i].join();
}

size_t vm_controller::execute(const jobT &job, const execute_config &config, std::function<void(size_t)> callback) {
	assert(config.size() > 0);
	// we currently only support the same slot for all configs
	{
		assert(config.size() > 0);
		assert(config.size() == machines().size());
		size_t compare = config[0].second;
		for (size_t i = 1; i < config.size(); ++i) {
			assert(compare == config[i].second);
		}
	}
	assert(work_counter_lock.owns_lock());

	++workers_active; // TODO should this be here?

	id_to_pool.emplace(cmd_counter, thread_pool.size());
	id_to_slot.emplace(cmd_counter, config[0].second);

	std::string cmd_name = "cmd_" + std::to_string(cmd_counter);

	const std::string command = generate_command(job, cmd_name, config);
	thread_pool.emplace_back(&vm_controller::execute_command_internal, this, command, cmd_name, config[0].second,
							 callback);

	return cmd_counter++;
}

// executed by a new thread, calls system to start the application
void vm_controller::execute_command_internal(std::string command, std::string cg_name, size_t config_used,
											 std::function<void(size_t)> callback) {
	command += " 2>&1 ";
	// command += "| tee ";
	command += "> ";
	command += cg_name + ".log";

	auto temp = system(command.c_str());
	assert(temp != -1);

	// we are done
	std::cout << ">> \t '" << command << "' completed at configuration " << config_used << std::endl;

	std::lock_guard<std::mutex> work_counter_lock(worker_counter_mutex);
	--workers_active; //  TODO should that be here?
	callback(config_used);
	worker_counter_cv.notify_one();
}

std::string vm_controller::generate_command(const jobT &job, std::string cg_name, const execute_config &config) {
	std::string host_list;
	for (auto virt_cluster_node : virt_cluster[config[0].second]) {
		host_list += virt_cluster_node.second.name + ",";
	}
	// remove last ','
	host_list.pop_back();

	return "mpiexec -np " + std::to_string(job.nprocs) + " -hosts " + host_list + " " + job.command;
}

template <typename T> void vm_controller::suspend_resume_virt_cluster(size_t slot) {
	// request freeze
	for (auto cluster_elem : virt_cluster[slot]) {
		std::string topic = "fast/migfra/" + cluster_elem.first + "/task";

		auto task = std::make_shared<T>(cluster_elem.second.name, true);

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
	for (size_t slot = 0; slot < SLOTS; ++slot) {
		std::vector<fast::msg::migfra::PCI_id> pci_ids;
		pci_ids.push_back(fast::msg::migfra::PCI_id(0x15b3, 0x1004));

		std::vector<std::pair<std::string, vm_pool_elemT>> temp_virt_cluster;
		for (auto mach = machines().begin(); mach != machines().end(); ++mach) {
			std::string topic = "fast/migfra/" + *mach + "/task";

			// load XML
			std::fstream slot_file;
			slot_file.open(slot_path + "/slot-" + std::to_string(slot) + ".xml");
			std::stringstream slot_stream;
			slot_stream << slot_file.rdbuf();
			std::string slot_xml = slot_stream.str();

			// get free vm
			vm_pool_elemT free_vm = glob_vm_pool.front();
			glob_vm_pool.pop_front();

			// modify XML
			// -- vm name
			std::regex name_regex("(<name>)(.+)(</name>)");
			slot_xml = std::regex_replace(slot_xml, name_regex, "$1" + free_vm.name + "$3");
			// -- disc
			std::regex disk_regex("(.*<source file=\".*)(parastation-.*)(\.qcow2\"/>)");
			slot_xml = std::regex_replace(slot_xml, disk_regex, "$1" + free_vm.name + "$3");

			// -- uuid
			uuid_t uuid;
			uuid_generate(uuid);
			char uuid_char_str[40];
			uuid_unparse(uuid, uuid_char_str);
			std::string uuid_str((const char *)uuid_char_str);
			std::regex uuid_regex("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}");
			slot_xml = std::regex_replace(slot_xml, uuid_regex, uuid_str);
			// -- mac
			std::regex mac_regex("([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})");
			slot_xml = std::regex_replace(slot_xml, mac_regex, free_vm.mac_addr);

			// send start task to mach
			auto task = std::make_shared<fast::msg::migfra::Start>(slot_xml, pci_ids, true);
			fast::msg::migfra::Task_container m;
			m.tasks.push_back(task);

			std::cout << "sending message \n topic: " << topic << "\n Start " << free_vm.name << std::endl;

			comm->send_message(m.to_string(), topic);

			// add VM to result
			temp_virt_cluster.emplace_back(*mach, free_vm);
		}

		fast::msg::migfra::Result_container response;
		for (auto mach = machines().begin(); mach != machines().end(); ++mach) {
			// wait for VMs to be started
			std::string topic = "fast/migfra/" + *mach + "/result";
			response.from_string(comm->get_message(topic));

			assert(!response.results.front().status.compare("success"));
		}

		virt_cluster[slot] = temp_virt_cluster;
	}
}

void vm_controller::stop_all_VMs() {
	for (size_t slot = 0; slot < SLOTS; ++slot) {
		// send stop request
		for (auto cluster_elem : virt_cluster[slot]) {
			auto task = std::make_shared<fast::msg::migfra::Stop>(cluster_elem.second.name, false, true, true);
			fast::msg::migfra::Task_container m;
			m.tasks.push_back(task);

			std::string topic = "fast/migfra/" + cluster_elem.first + "/task";
			std::cout << "sending message \n topic: " << topic << "\n message:\n" << m.to_string() << std::endl;
			comm->send_message(m.to_string(), topic);
		}

		// wait for completion
		fast::msg::migfra::Result_container response;
		for (auto cluster_elem : virt_cluster[slot]) {
			std::string topic = "fast/migfra/" + cluster_elem.first + "/result";
			response.from_string(comm->get_message(topic));
			assert(!response.results.front().status.compare("success"));

			// add VM to vm_pool
			glob_vm_pool.push_back(cluster_elem.second);
		}

		// clear virtual cluster
		virt_cluster[slot].clear();
	}
}
