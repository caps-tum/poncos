#include "poncos/cgroup_controller.hpp"

#include <cassert>
#include <condition_variable>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>

#include "poncos/poncos.hpp"

#include <fast-lib/message/agent/mmbwmon/restart.hpp>
#include <fast-lib/message/agent/mmbwmon/stop.hpp>

cgroup_controller::cgroup_controller(const std::shared_ptr<fast::MQTT_communicator> &_comm,
									 const std::string &machine_filename)
	: controllerT(_comm, machine_filename) {
	// subscribe to the various topics
	for (const std::string &mach : machines) {
		std::string topic = "fast/agent/" + mach + "/mmbwmon/restart/ack";
		comm->add_subscription(topic);
		topic = "fast/agent/" + mach + "/mmbwmon/stop/ack";
		comm->add_subscription(topic);
	}
}

void cgroup_controller::init() {}
void cgroup_controller::dismantle() {}

cgroup_controller::~cgroup_controller() = default;

void cgroup_controller::freeze(const size_t id) {
	assert(id < id_to_config.size());

	const execute_config &config = id_to_config[id];

	send_message<fast::msg::agent::mmbwmon::stop>(config, "/mmbwmon/stop");
}

void cgroup_controller::thaw(const size_t id) {
	assert(id < id_to_config.size());

	const execute_config &config = id_to_config[id];

	send_message<fast::msg::agent::mmbwmon::restart>(config, "/mmbwmon/restart");
}

void cgroup_controller::freeze_opposing(const size_t id) {
	// TODO probably broken if there is no opposing job!
	const execute_config opposing_config = generate_opposing_config(id);

	send_message<fast::msg::agent::mmbwmon::stop>(opposing_config, "/mmbwmon/stop");
}

void cgroup_controller::thaw_opposing(const size_t id) {
	// TODO probably broken if there is no opposing job!
	const execute_config opposing_config = generate_opposing_config(id);

	send_message<fast::msg::agent::mmbwmon::restart>(opposing_config, "/mmbwmon/restart");
}

template <typename messageT>
void cgroup_controller::send_message(const controllerT::execute_config &config, const std::string &topic_adn) const {
	for (auto config_elem : config) {
		const size_t id = machine_usage[config_elem.first][config_elem.second];
		const messageT m(cmd_name_from_id(id));
		const std::string topic = "fast/agent/" + machines[config_elem.first] + topic_adn;

		comm->send_message(m.to_string(), topic);
	}

	for (auto config_elem : config) {
		const std::string topic = "fast/agent/" + machines[config_elem.first] + topic_adn + "/ack";

		comm->get_message(topic);
	}
}

void cgroup_controller::update_config(const size_t /*id*/, const execute_config & /*new_config*/) { assert(false); }

std::string cgroup_controller::generate_command(const jobT &job, size_t counter, const execute_config &config) const {
	std::string host_lists[SLOTS];
	size_t hosts_per_slot[SLOTS];
	for (unsigned long &s : hosts_per_slot) s = 0;

	for (std::pair<size_t, size_t> p : config) {
		host_lists[p.second] += machines[p.first] + ",";
		++hosts_per_slot[p.second];
	}

	assert(job.nprocs == std::accumulate(std::begin(hosts_per_slot), std::end(hosts_per_slot), size_t(0)));

	std::string commands[SLOTS];
	for (size_t slot = 0; slot < SLOTS; ++slot) {
		std::string &list = host_lists[slot];
		// skip if host list is empty
		if (hosts_per_slot[slot] == 0) continue;

		// remove last ','
		list.pop_back();
		std::string command = " ./cgroup_wrapper.sh ";
		command += cmd_name_from_id(counter) + " ";

		// cgroup CPUs and memory is set by the bash script
		for (int i : co_configs[slot].cpus) {
			command += std::to_string(i) + ",";
		}
		// remove last ','
		command.pop_back();

		command += " ";

		for (int i : co_configs[slot].mems) {
			command += std::to_string(i) + ",";
		}
		// remove last ','
		command.pop_back();

		command += job.command;
		commands[slot] = command;
	}

	// a colon must be added between slots
	bool add_colon = false;

	// TODO: add thread_per_procs to command string for the support of multi-threaded applications
	std::string ret = "mpiexec ";
	for (size_t slot = 0; slot < SLOTS; ++slot) {
		if (hosts_per_slot[slot] == 0) continue;

		if (add_colon) ret += " : ";

		ret += "-np " + std::to_string(hosts_per_slot[slot]) + " -hosts " + host_lists[slot] + commands[slot];
		add_colon = true;
	}

	return ret;
}
