#include "poncos/cgroup_controller.hpp"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <iostream>
#include <string>
#include <thread>

#include "poncos/poncos.hpp"

#include <fast-lib/message/agent/mmbwmon/restart.hpp>
#include <fast-lib/message/agent/mmbwmon/stop.hpp>

cgroup_controller::cgroup_controller(const std::shared_ptr<fast::MQTT_communicator> &_comm,
									 const std::string &machine_filename)
	: controllerT(_comm, machine_filename) {
	// subscribe to the various topics
	for (std::string mach : machines) {
		std::string topic = "fast/agent/" + mach + "/mmbwmon/restart/ack";
		comm->add_subscription(topic);
		topic = "fast/agent/" + mach + "/mmbwmon/stop/ack";
		comm->add_subscription(topic);
	}
}

void cgroup_controller::init() {}
void cgroup_controller::dismantle() {}

cgroup_controller::~cgroup_controller() {}

void cgroup_controller::freeze(const size_t id) {
	const fast::msg::agent::mmbwmon::stop m(cgroup_name_from_id(id));
	for (std::string mach : machines) {
		std::string topic = "fast/agent/" + mach + "/mmbwmon/stop";
		// std::cout << "sending message \n topic: " << topic << "\n message:\n" << m.to_string() << std::endl;
		comm->send_message(m.to_string(), topic);
	}

	for (std::string mach : machines) {
		std::string topic = "fast/agent/" + mach + "/mmbwmon/stop/ack";
		// std::cout << "waiting on topic: " << topic << " ... " << std::flush;
		comm->get_message(topic);
		// std::cout << "done\n";
	}
}

void cgroup_controller::thaw(const size_t id) {
	const fast::msg::agent::mmbwmon::restart m(cgroup_name_from_id(id));
	for (std::string mach : machines) {
		std::string topic = "fast/agent/" + mach + "/mmbwmon/restart";
		// std::cout << "sending message \n topic: " << topic << "\n message:\n" << m.to_string() << std::endl;
		comm->send_message(m.to_string(), topic);
	}

	for (std::string mach : machines) {
		std::string topic = "fast/agent/" + mach + "/mmbwmon/restart/ack";
		// std::cout << "waiting on topic: " << topic << " ... " << std::flush;
		comm->get_message(topic);
		// std::cout << "done\n";
	}
}

size_t cgroup_controller::execute(const jobT &job, const execute_config &config, std::function<void(size_t)> callback) {
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

	std::string cg_name = cgroup_name_from_id(cmd_counter);
	// cgroup is created by the bash script

	id_to_pool.emplace(cmd_counter, thread_pool.size());

	const std::string command = generate_command(job, cg_name, config);
	thread_pool.emplace_back(&cgroup_controller::execute_command_internal, this, command, cg_name, config, callback);

	return cmd_counter++;
}

// executed by a new thread, calls system to start the application
void cgroup_controller::execute_command_internal(std::string command, std::string cg_name, const execute_config config,
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

std::string cgroup_controller::generate_command(const jobT &job, std::string cg_name, const execute_config &config) {
	std::string host_list;
	for (std::pair<size_t, size_t> p : config) {
		host_list += machines[p.first] + ",";
	}
	// remove last ','
	host_list.pop_back();

	std::string command = " ./cgroup_wrapper.sh ";

	command += cg_name + " ";

	// cgroup CPUs and memory is set by the bash script
	for (int i : co_configs[config[0].second].cpus) {
		command += std::to_string(i) + ",";
	}
	// remove last ','
	command.pop_back();

	command += " ";

	for (int i : co_configs[config[0].second].mems) {
		command += std::to_string(i) + ",";
	}
	// remove last ','
	command.pop_back();

	command += job.command;

	return "mpiexec -np " + std::to_string(job.nprocs) + " -hosts " + host_list + command;
}

std::string cgroup_controller::cgroup_name_from_id(size_t id) {
	std::string cg_name("pons_");
	cg_name += std::to_string(id);
	return cg_name;
}
