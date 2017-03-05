#include "poncos/controller_cgroup.hpp"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <thread>

#include "poncos/poncos.hpp"

#include <fast-lib/message/agent/mmbwmon/restart.hpp>
#include <fast-lib/message/agent/mmbwmon/stop.hpp>

cgroup_controller::cgroup_controller(const std::shared_ptr<fast::MQTT_communicator> &_comm,
									 const std::string &machine_filename)
	: controllerT(_comm, machine_filename) {}

void cgroup_controller::init() {}
void cgroup_controller::dismantle() {}

void cgroup_controller::create_domain(const size_t id) {
	// get the config for the given id
	const execute_config &config = id_to_config[id];

	const std::string cgroup_name = cmd_name_from_id(id);

	// store the tasks in a map with machine id as a key to merge slots on the same machine
	std::unordered_map<size_t, fast::msg::migfra::Task_container> task_container_map;

	// generate start tasks
	for (const auto &config_elem : config) {
		const size_t slot = config_elem.second;

		// determine info to create cgroup
		std::vector<std::vector<unsigned int>> memnode_map;
		std::vector<unsigned int> mem_nodes(co_configs[slot].mems.begin(), co_configs[slot].mems.end());
		memnode_map.emplace_back(mem_nodes);

		std::vector<std::vector<unsigned int>> cpu_map;
		std::vector<unsigned int> cpu_list(co_configs[slot].cpus.begin(), co_configs[slot].cpus.end());
		cpu_map.emplace_back(cpu_list);

		auto iter = task_container_map.find(config_elem.first);
		if (iter != task_container_map.end()) {
			// already an entry in the map -> append
			auto task = std::dynamic_pointer_cast<fast::msg::migfra::Start>(iter->second.tasks[0]);
			task->vcpu_map.get()[0].insert(task->vcpu_map.get()[0].end(), cpu_map[0].begin(), cpu_map[0].end());
			task->memnode_map.get()[0].insert(task->memnode_map.get()[0].end(), memnode_map[0].begin(), memnode_map[0].end());
		} else {
			auto task = std::make_shared<fast::msg::migfra::Start>();
			task->vm_name = cgroup_name;
			task->vcpu_map = cpu_map;
			task->memnode_map = memnode_map;

			fast::msg::migfra::Task_container m;
			m.tasks.push_back(task);

			task_container_map.insert({config_elem.first, m});
		}
	}
	for (const auto &tc : task_container_map) {
		const std::string topic = "fast/migfra/" + machines[tc.first] + "/task";
		comm->send_message(tc.second.to_string(), topic);
	}

	// wait for responses
	fast::msg::migfra::Result_container response;
	for (const auto &tc : task_container_map) {
		// wait for VMs to be started
		const std::string topic = "fast/migfra/" + machines[tc.first] + "/result";
		response.from_string(comm->get_message(topic));

		// check success for each result
		for (auto result : response.results) {
			assert(result.status == "success");
		}
	}
}

void cgroup_controller::delete_domain(const size_t id) {
	const execute_config &config = id_to_config[id];

	// determine info to create cgroup
	const std::string cgroup_name = cmd_name_from_id(id);

	// generate start tasks
	for (const auto &config_elem : config) {
		std::string topic = "fast/migfra/" + machines[config_elem.first] + "/task";
		fast::msg::migfra::Task_container m;
		auto task = std::make_shared<fast::msg::migfra::Stop>();
		task->vm_name = cgroup_name;
		m.tasks.push_back(task);
		comm->send_message(m.to_string(), topic);
	}

	// wait for responses
	fast::msg::migfra::Result_container response;
	for (const auto &config_elem : config) {
		// wait for VMs to be started
		std::string topic = "fast/migfra/" + machines[config_elem.first] + "/result";
		response.from_string(comm->get_message(topic));

		// check success for each result
		for (auto result : response.results) {
			assert(result.status == "success");
		}
	}
}

// TODO should delete the mpi host files!
cgroup_controller::~cgroup_controller() = default;

std::string cgroup_controller::domain_name_from_config_elem(const execute_config_elemT &config_elem) const {
	return cmd_name_from_id(machine_usage[config_elem.first][config_elem.second]);
}

void cgroup_controller::update_config(const size_t /*id*/, const execute_config & /*new_config*/) { assert(false); }

controllerT::execute_config cgroup_controller::sort_config_by_hostname(const execute_config &config) const {
	execute_config sorted_config = config;
	std::sort(sorted_config.begin(), sorted_config.end(),
			  [](const auto &elem_a, const auto &elem_b) { return elem_a.first < elem_b.first; });
	return sorted_config;
}

std::string cgroup_controller::generate_command(const jobT &job, size_t counter, const execute_config &config) const {

	// TODO refactor, eg seperate file creation (shouln't that be part of controllerT)

	// index 0 == SLOT 0; index 1 == SLOT 1; index SLOTS == all slots on the same system are used
	std::vector<std::string> host_lists[SLOTS + 1];
	size_t hosts_per_slot[SLOTS + 1] = {0};
	std::string commands[SLOTS + 1];
	execute_config sorted_config = sort_config_by_hostname(config);

	for (size_t i = 0; i < config.size(); ++i) {
		// both slots of a system used?
		if (i + 1 < config.size() && config[i].first == config[i + 1].first) {
			host_lists[SLOTS].emplace_back(machines[config[i].first]);
			host_lists[SLOTS].emplace_back(machines[config[i].first]);
			hosts_per_slot[SLOTS] += 2;

			++i;
			continue;
		}
		host_lists[config[i].second].emplace_back(machines[config[i].first]);
		++hosts_per_slot[config[i].second];
	}
	assert(job.req_cpus() <=
		   std::accumulate(std::begin(hosts_per_slot), std::end(hosts_per_slot), size_t(0)) * SLOT_SIZE);

	for (size_t slot = 0; slot < SLOTS; ++slot) {
		if (hosts_per_slot[slot] == 0) continue;

		std::string &command = commands[slot];
		command = " ./cgroup_wrapper.sh ";
		command += cmd_name_from_id(counter) + " ";

		command += job.command;
	}

	// some dedicated nodes, requires special cgroup config
	if (hosts_per_slot[SLOTS] != 0) {
		std::string &command = commands[SLOTS];
		command = " ./cgroup_wrapper.sh ";
		command += cmd_name_from_id(counter) + " ";

		command += job.command;
	}

	// create hostfile
	// filename: cmd_name_from_id(counter).hosts
	// content: one line per allocated slot in accordance with the following scheme
	//          <hostname>:job.nprocs/hosts_per_slot
	std::string hosts_filename = cmd_name_from_id(counter) + ".hosts";
	std::ofstream hosts_file(hosts_filename);
	assert(hosts_file.is_open());

	size_t process_per_slot = SLOT_SIZE / job.threads_per_proc;
	assert(SLOT_SIZE % job.threads_per_proc == 0);

	for (size_t i = 0; i <= SLOTS; ++i) {
		if (hosts_per_slot[i] == 0) continue;

		for (const auto &host : host_lists[i]) {
			hosts_file << host + ":";
			hosts_file << process_per_slot;
			hosts_file << std::endl;
		}
	}
	hosts_file.close();

	std::string ret = "mpiexec ";
	ret += "-f " + hosts_filename + " -genv OMP_NUM_THREADS " + std::to_string(job.threads_per_proc);

	// a colon must be added between slots
	bool add_colon = false;
	for (size_t slot = 0; slot < SLOTS + 1; ++slot) {
		if (hosts_per_slot[slot] == 0) continue;

		if (add_colon) ret += " : ";

		ret += " -np " + std::to_string(process_per_slot * hosts_per_slot[slot]) + " " + commands[slot];
		add_colon = true;
	}
	return ret;
}
