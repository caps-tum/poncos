#include "poncos/scheduler_multi_app.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>

#include "poncos/cgroup_controller.hpp"
#include "poncos/job.hpp"
#include "poncos/poncos.hpp"

double multi_app_sched::membw_util_of_node(const size_t &idx) {
	double total_membw_util = 0;
	for (size_t slot = 0; slot < SLOTS; ++slot) {
		total_membw_util += membw_util[idx][slot];
	}

	return total_membw_util;
}

std::vector<size_t> multi_app_sched::sort_machines_by_membw_util(std::vector<size_t> machine_idxs, bool reverse) {

	std::vector<double> total_mem_bws;
	for (size_t idx = 0; idx < membw_util.size(); ++idx) {
		total_mem_bws.emplace_back(membw_util_of_node(idx));
	}

	std::sort(machine_idxs.begin(), machine_idxs.end(), [&total_mem_bws, &reverse](size_t i1, size_t i2) {
		if (reverse) {
			return total_mem_bws[i1] > total_mem_bws[i2];
		} else {
			return total_mem_bws[i1] < total_mem_bws[i2];
		}
	});

	return machine_idxs;
}

std::vector<size_t> multi_app_sched::check_membw(const controllerT::execute_config &config) const {
	std::vector<size_t> marked_machines;
	for (const auto &c : config) {
		// get membw
		double membw = 0.0;
		for (size_t s = 0; s < SLOTS; ++s) {
			membw += 1.0 - membw_util[c.first][s];
		}

		// 	membw ok?
		// 		no -> mark it
		if (membw > 0.9) {
			marked_machines.push_back(c.first);
		}
	}
	return marked_machines;
}

controllerT::execute_config multi_app_sched::generate_optimal_config(std::vector<size_t> marked_machines, std::vector<size_t> swap_candidates) const {
	controllerT::execute_config new_config;
	//TODO
	return new_config;
}

controllerT::execute_config multi_app_sched::find_new_config(std::vector<size_t> marked_machines) {
	controllerT::execute_config new_config;

	// determine swap candidates
	std::vector<size_t> swap_candidates;
	std::iota(swap_candidates.begin(), swap_candidates.end(), 0);
	swap_candidates = sort_machines_by_membw_util(swap_candidates, false);

	// check if new config is possible
	double total_membw_util = 0;
	for (size_t idx = 0; idx < marked_machines.size(); ++idx) {
		total_membw_util += membw_util_of_node(marked_machines[idx]);
		total_membw_util += membw_util_of_node(swap_candidates[idx]);
	}

	// are we able to find a new config?
	if (total_membw_util < PER_MACHINE_TH*marked_machines.size()*2) {
		new_config = generate_optimal_config(sort_machines_by_membw_util(marked_machines, true), swap_candidates);
	}

	return new_config;
}

// called after a command was completed
void multi_app_sched::command_done(const size_t id, controllerT &controller) {
	const auto &config = controller.id_to_config[id];

	for (const auto &c : config) {
		membw_util[c.first][c.second] = 0.0;
	}
}

void multi_app_sched::schedule(const job_queueT &job_queue, fast::MQTT_communicator &comm, controllerT &controller,
							   std::chrono::seconds wait_time) {

	membw_util.resize(controller.machines.size(), std::array<double, 2>{{0.0, 0.0}});

	// for all commands
	for (auto job : job_queue.jobs) {
		assert(job.nprocs <= controller.machines.size() * SLOT_SIZE);
		controller.wait_for_ressource(job.nprocs);

		// select ressources
		controllerT::execute_config config;
		for (size_t m = 0; m < controller.machine_usage.size(); ++m) {
			const auto &mu = controller.machine_usage[m];

			// TODO check distgen values here?
			// -> don't use the ones that are already saturated?
			// -> prioritize something else?

			// pick one slot per machine
			for (size_t s = 0; s < SLOTS; ++s) {
				if (mu[s] == std::numeric_limits<size_t>::max()) {
					config.emplace_back(m, s);
					break;
				}
			}
			if (config.size() * SLOT_SIZE == job.nprocs) break;
		}

		assert(config.size() * SLOT_SIZE == job.nprocs);

		// start job
		auto job_id = controller.execute(job, config, [&](const size_t config) { command_done(config, controller); });
		std::cout << ">> \t starting '" << job << std::endl;

		// for the initialization phase of the application to be completed
		std::this_thread::sleep_for(wait_time);

		// stop the opposing VM
		controller.freeze_opposing(job_id);

		// start distgen
		auto distgen_res =
			schedulerT::run_distgen(comm, controller.machines, controller.generate_opposing_config(job_id));
		assert(distgen_res.size() == config.size());

		for (size_t i = 0; i < distgen_res.size(); ++i) {
			const auto &c = config[i];
			assert(membw_util[c.first][c.second] == 0.0);
			membw_util[c.first][c.second] = distgen_res[i];
		}

		// restart opposing VM
		controller.thaw_opposing(job_id);

		bool frozen = false;

		while (true) {
			// for all host-id of new job
			auto marked_machines = check_membw(config);

			// everything fine?
			if (marked_machines.empty()) break;

			if (controller.update_supported()) {
				controllerT::execute_config new_config = find_new_config(marked_machines);

				if (!new_config.empty()) {
					controller.update_config(job_id, new_config);
				}
			}

			if (!frozen) {
				controller.freeze(job_id);
				frozen = true;
			}
			controller.wait_for_change();
		}
		if (frozen) controller.thaw(job_id);
	}

	controller.done();
}
