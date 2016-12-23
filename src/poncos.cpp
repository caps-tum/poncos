/**
 * Simple scheduler.
 *
 * Copyright 2016 by LRR-TUM
 * Jens Breitbart     <j.breitbart@tum.de>
 *
 * Licensed under GNU General Public License 2.0 or later.
 * Some rights reserved. See LICENSE
 */

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include <uuid/uuid.h>

#include "poncos/poncos.hpp"
#include "poncos/time_measure.hpp"

#include <fast-lib/message/agent/mmbwmon/ack.hpp>
#include <fast-lib/message/agent/mmbwmon/reply.hpp>
#include <fast-lib/message/agent/mmbwmon/request.hpp>
#include <fast-lib/message/agent/mmbwmon/restart.hpp>
#include <fast-lib/message/agent/mmbwmon/stop.hpp>
#include <fast-lib/message/migfra/pci_id.hpp>
#include <fast-lib/message/migfra/result.hpp>
#include <fast-lib/message/migfra/task.hpp>
#include <fast-lib/mqtt_communicator.hpp>

// COMMAND LINE PARAMETERS
static std::string server;
static size_t port = 1883;
static std::string queue_filename;
static std::string machine_filename;
static std::string slot_path;

// marker if a slot is in use
static bool co_config_in_use[SLOTS] = {false, false};

// cgroup name running in a specific slot
static std::string co_config_cgroup_name[SLOTS];

// distgen results of a slot
static double co_config_distgend[SLOTS];

// threads used to run the applications
static std::vector<std::thread> thread_pool;

// index in the thread pool of the thread executing the cgroup in SLOT
static size_t co_config_thread_index[SLOTS];

// virtual cluster per slot
static std::unordered_map<std::string, vm_pool_elemT> co_config_virt_cluster[SLOTS];

// numbers of active workser
static size_t workers_active = 0;

// lock/cond variable used to wait for a job to be completed
static std::mutex worker_counter_mutex;
static std::condition_variable worker_counter_cv;

// a list of all machines
static std::vector<std::string> machines;

[[noreturn]] static void print_help(const char *argv) {
	std::cout << argv << " supports the following flags:\n";
	std::cout << "\t --server \t\t URI of the MQTT broker. \t\t\t Required!\n";
	std::cout << "\t --port \t\t Port of the MQTT broker. \t\t\t Default: 1883\n";
	std::cout << "\t --queue \t\t Filename for the job queue. \t\t\t Required!\n";
	std::cout << "\t --machine \t\t Filename containing node names. \t\t\t Required!\n";
	std::cout << "\t --slot-path \t\t Path to XML slot specifications. \t\t\t Required!\n";
	exit(0);
}

static void parse_options(size_t argc, const char **argv) {
	if (argc == 1) {
		print_help(argv[0]);
	}

	for (size_t i = 1; i < argc; ++i) {
		std::string arg(argv[i]);

		if (arg == "--server") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			server = std::string(argv[i + 1]);
			++i;
			continue;
		}
		if (arg == "--port") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			port = std::stoul(std::string(argv[i + 1]));
			++i;
			continue;
		}

		if (arg == "--queue") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			queue_filename = std::string(argv[i + 1]);
			++i;
			continue;
		}
		if (arg == "--machine") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			machine_filename = std::string(argv[i + 1]);
			++i;
			continue;
		}
		if (arg == "--slot-path") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			slot_path = std::string(argv[i + 1]);
			++i;
			continue;
		}
	}

	if (queue_filename == "" || machine_filename == "" || slot_path == "") print_help(argv[0]);
}

// stop isolated environments on all machines in given slot
static void stop_virt_cluster(fast::MQTT_communicator &comm, const size_t slot) {

	// send stop request
	for (auto cluster_elem : co_config_virt_cluster[slot]) {
		auto task = std::make_shared<fast::msg::migfra::Stop>(cluster_elem.second.name, false, true, true);
		fast::msg::migfra::Task_container m;
		m.tasks.push_back(task);

		std::string topic = "fast/migfra/" + cluster_elem.first + "/task";
		std::cout << "sending message \n topic: " << topic << "\n message:\n" << m.to_string() << std::endl;
		comm.send_message(m.to_string(), topic);
	}

	// wait for completion
	fast::msg::migfra::Result_container response;
	for (auto cluster_elem : co_config_virt_cluster[slot]) {
		std::string topic = "fast/migfra/" + cluster_elem.first + "/result";
		response.from_string(comm.get_message(topic));
		assert(!response.results.front().status.compare("success"));

		// add VM to vm_pool
		vm_pool.push_back(cluster_elem.second);
	}

	// clear virtual cluster
	co_config_virt_cluster[slot].clear();
}

// start isolated environments on all machines in given slot
static std::unordered_map<std::string, vm_pool_elemT> start_virt_cluster(fast::MQTT_communicator &comm, size_t slot) {
	std::vector<fast::msg::migfra::PCI_id> pci_ids;
	pci_ids.push_back(fast::msg::migfra::PCI_id(0x15b3, 0x1004));

	std::unordered_map<std::string, vm_pool_elemT> virt_cluster;
	for (auto mach = machines.begin(); mach != machines.end(); ++mach) {
		std::string topic = "fast/migfra/" + *mach + "/task";

		// load XML
		size_t env_idx = std::distance(machines.begin(), mach);
		std::fstream slot_file;
		slot_file.open(slot_path + "/slot-" + std::to_string(slot) + ".xml");
		std::stringstream slot_stream;
		slot_stream << slot_file.rdbuf();
		std::string slot_xml = slot_stream.str();

		// get free vm
		vm_pool_elemT free_vm = vm_pool.front();
		vm_pool.pop_front();

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
		std::string uuid_str((const char*)uuid_char_str);
		std::regex uuid_regex("[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}");
		slot_xml = std::regex_replace(slot_xml, uuid_regex, uuid_str);
		// -- mac
		std::regex mac_regex("([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})");
		slot_xml = std::regex_replace(slot_xml, mac_regex, free_vm.mac_addr);

		// send start task to mach
		auto task = std::make_shared<fast::msg::migfra::Start>(slot_xml, pci_ids, true);
		fast::msg::migfra::Task_container m;
		m.tasks.push_back(task);

//		std::cout << "sending message \n topic: " << topic << "\n message:\n" << m.to_string() << std::endl;
		std::cout << "sending message \n topic: " << topic << "\n Start " << free_vm.name << std::endl;

		comm.send_message(m.to_string(), topic);

		// add VM to result
		virt_cluster.insert({*mach, free_vm});
	}

	fast::msg::migfra::Result_container response;
	for (auto mach = machines.begin(); mach != machines.end(); ++mach) {
		// wait for VMs to be started
		std::string topic = "fast/migfra/" + *mach + "/result";
		response.from_string(comm.get_message(topic));

		assert(!response.results.front().status.compare("success"));
	}

	return virt_cluster;
}

// freeze virtual cluster for mmbwmon measurement
template<typename T>
static void suspend_resume_virt_cluster(fast::MQTT_communicator &comm, size_t slot) {
	// request freeze
	for (auto cluster_elem : co_config_virt_cluster[slot]) {
		std::string topic = "fast/migfra/" + cluster_elem.first + "/task";

		auto task = std::make_shared<T>(cluster_elem.second.name, true);

		fast::msg::migfra::Task_container m;
		m.tasks.push_back(task);

		std::cout << "sending message \n topic: " << topic << "\n message:\n" << m.to_string() << std::endl;

		comm.send_message(m.to_string(), topic);
	}

	// wait for results
	fast::msg::migfra::Result_container response;
	std::unordered_map<std::string, std::string> virt_cluster;
	for (auto cluster_elem : co_config_virt_cluster[slot]) {
		// wait for VMs to be started
		std::string topic = "fast/migfra/" + cluster_elem.first + "/result";
		response.from_string(comm.get_message(topic));

		int status = response.results.front().status.compare("success");
		if (status) {
			assert(!response.results.front().details.compare("Error suspending domain: Requested operation is not valid: domain is not running"));
		}

	}
}

// called after a command was completed
static void command_done(const size_t config) {
	std::lock_guard<std::mutex> work_counter_lock(worker_counter_mutex);
	--workers_active;
	co_config_in_use[config] = false;
	co_config_distgend[config] = 0;
	worker_counter_cv.notify_one();
}

// executed by a new thread, calls system to start the application
void execute_command_internal(std::string command, std::string cg_name, size_t config_used) {
	command += " 2>&1 ";
	// command += "| tee ";
	command += "> ";
	command += cg_name + ".log";

	auto temp = system(command.c_str());
	assert(temp != -1);

	// we are done
	std::cout << ">> \t '" << command << "' completed at configuration " << config_used << std::endl;
	command_done(config_used);
}

// command input: mpiexec -np X PONCOS command p0 p1
// run instead  : mpiexec -np X -H <virt_cluster> command p0 p1
static std::string parse_command(std::string comm, std::unordered_map<std::string, vm_pool_elemT> virt_cluster) {
	std::string replace = "-H ";
	for (auto cluster_elem : virt_cluster) {
		replace += cluster_elem.second.name + ",";
	}
	comm.replace(comm.find("PONCOS"), std::string("PONCOS").size(), replace);

	return comm;
}

static size_t execute_command(fast::MQTT_communicator &comm, std::string command,
							  const std::unique_lock<std::mutex> &work_counter_lock) {
	static size_t cgroups_counter = 0;

	assert(work_counter_lock.owns_lock());
	++workers_active;

	std::string cg_name = cgroup_name_from_id(cgroups_counter);
	// cgroup is created by the bash script
	++cgroups_counter;

	// search for a free slot and assign it to a new job
	for (size_t i = 0; i < SLOTS; ++i) {
		if (!co_config_in_use[i]) {
			co_config_in_use[i] = true;
			co_config_cgroup_name[i] = cg_name;
			co_config_thread_index[i] = thread_pool.size();

			// start isolated environments (e.g, VMs)
			co_config_virt_cluster[i] = start_virt_cluster(comm, i);

			command = parse_command(command, co_config_virt_cluster[i]);

			std::cout << ">> \t starting '" << command << "' at configuration " << i << std::endl;

			// wait for MPI layer to come up
			std::this_thread::sleep_for(std::chrono::seconds(2));

			thread_pool.emplace_back(execute_command_internal, command, cg_name, i);

			return i;
		}
	}

	assert(false);

	return 0;
}

static double run_distgen(fast::MQTT_communicator &comm, sched_configT conf) {

	// ask for measurements
	{
		fast::msg::agent::mmbwmon::request m;

		// TODO check if we can use the same type
		m.cores.resize(conf.cpus.size());
		for (unsigned int i = 0; i < conf.cpus.size(); ++i) {
			m.cores[i] = static_cast<size_t>(conf.cpus[i]);
		}

		for (std::string mach : machines) {
			std::string topic = "fast/agent/" + mach + "/mmbwmon/request";
			// std::cout << "sending message \n topic: " << topic << "\n message:\n" << m.to_string() << std::endl;
			comm.send_message(m.to_string(), topic);
		}
	}

	double ret = 0.0;

	// wait for results
	{
		for (std::string mach : machines) {
			fast::msg::agent::mmbwmon::reply m;
			std::string topic = "fast/agent/" + mach + "/mmbwmon/response";
			// std::cout << "waiting on topic: " << topic << " ... " << std::flush;
			m.from_string(comm.get_message(topic));
			// std::cout << "done\n";

			if (m.result > ret) ret = m.result;
		}
	}

	return ret;
}

static void coschedule_queue(const std::vector<std::string> &command_queue, fast::MQTT_communicator &comm) {
	// for all commands
	for (auto command : command_queue) {
		// wait until workers_active < SLOTS
		std::unique_lock<std::mutex> work_counter_lock(worker_counter_mutex);
		worker_counter_cv.wait(work_counter_lock, [] { return workers_active < SLOTS; });

		// start the new job
		const size_t new_config = execute_command(comm, command, work_counter_lock);
		// old config is used to run distgen
		const size_t old_config = (new_config + 1) % SLOTS;

		// for the initialization phase of the application to be completed
		std::this_thread::sleep_for(std::chrono::seconds(5));

		// check if two are running
		if (co_config_in_use[0] && co_config_in_use[1]) {
			// std::cout << "0: freezing old" << std::endl;
			suspend_resume_virt_cluster<fast::msg::migfra::Suspend>(comm, old_config);
		}

		// measure distgen result
		std::cout << ">> \t Running distgend at " << old_config << std::endl;
		co_config_distgend[new_config] = run_distgen(comm, co_configs[old_config]);

		std::cout << ">> \t Result for command '" << command << "' is: " << 1 - co_config_distgend[new_config]
				  << std::endl;

		if (co_config_in_use[0] && co_config_in_use[1]) {
			// std::cout << "0: thaw old" << std::endl;
			suspend_resume_virt_cluster<fast::msg::migfra::Resume>(comm, old_config);

			std::cout << ">> \t Estimating total usage of "
					  << (1 - co_config_distgend[0]) + (1 - co_config_distgend[1]);

			if ((1 - co_config_distgend[0]) + (1 - co_config_distgend[1]) > 0.9) {
				std::cout << " -> we will run one" << std::endl;
				// std::cout << "0: freezing new" << std::endl;
				suspend_resume_virt_cluster<fast::msg::migfra::Suspend>(comm, new_config);
				work_counter_lock.unlock();

				// std::cout << "0: wait for old" << std::endl;
				thread_pool[co_config_thread_index[old_config]].join();
				std::cout << "Stop virtual cluster ... " << std::endl;
				stop_virt_cluster(comm, old_config);

				// std::cout << "0: thaw new" << std::endl;
				suspend_resume_virt_cluster<fast::msg::migfra::Resume>(comm, new_config);
			} else {
				std::cout << " -> we will run both applications" << std::endl;
			}

		} else {
			std::cout << ">> \t Just one config in use ATM" << std::endl;
		}
	}

	// wait until all workers are finished before deleting the cgroup
	std::unique_lock<std::mutex> work_counter_lock(worker_counter_mutex);
	worker_counter_cv.wait(work_counter_lock, [] { return workers_active == 0; });

	// stop all running virtual clusters
	for (unsigned int i = 0; i < SLOTS; ++i) {
		stop_virt_cluster(comm, i);
	}
}

static void cleanup() {
	for (auto &t : thread_pool) {
		if (t.joinable()) t.join();
	}
	thread_pool.resize(0);

	// No need to delete cgroups. Should be done automatically
	// by our bash script.
}

int main(int argc, char const *argv[]) {
	parse_options(static_cast<size_t>(argc), argv);

	// fill the command qeue
	std::cout << "Reading command queue " << queue_filename << " ...";
	std::cout.flush();
	std::vector<std::string> command_queue;
	read_file(queue_filename, command_queue);
	std::cout << " done!" << std::endl;

	// fill the machine file
	std::cout << "Reading machine file " << machine_filename << " ...";
	std::cout.flush();
	read_file(machine_filename, machines);
	std::cout << " done!" << std::endl;

	std::cout << "Command queue:\n";
	std::cout << "==============\n";
	for (std::string c : command_queue) {
		std::cout << c << "\n";
	}
	std::cout << "==============\n";

	std::cout << "Machine file:\n";
	std::cout << "==============\n";
	for (std::string c : machines) {
		std::cout << c << "\n";
	}
	std::cout << "==============\n";

	fast::MQTT_communicator comm("fast/poncos", "fast/poncos", "fast/poncos", server, static_cast<int>(port), 60);

	// subscribe to the various topics
	for (std::string mach : machines) {
		std::string topic = "fast/agent/" + mach + "/mmbwmon/response";
		comm.add_subscription(topic);
		topic = "fast/agent/" + mach + "/mmbwmon/restart/ack";
		comm.add_subscription(topic);
		topic = "fast/agent/" + mach + "/mmbwmon/stop/ack";
		comm.add_subscription(topic);
		topic = "fast/migfra/" + mach + "/result";
		comm.add_subscription(topic);
	}

	std::cout << "MQTT ready!\n\n";

	const auto runtime = time_measure<>::execute(coschedule_queue, command_queue, comm);
	std::cout << "total runtime: " << runtime << " ms" << std::endl;

	cleanup();
}
