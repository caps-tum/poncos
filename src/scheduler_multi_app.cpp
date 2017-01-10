#include "poncos/scheduler_multi_app.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>

#include "poncos/cgroup_controller.hpp"
#include "poncos/job.hpp"
#include "poncos/poncos.hpp"

std::vector<size_t> multi_app_sched::check_membw(const controllerT::execute_config &config) const {
	std::vector<size_t> marked_machines;
	for (const auto &c : config) {
		// get membw
		double membw = 0.0;
		for (size_t s = 0; s < SLOTS; ++s) {
			membw += membw_util[c.first][s];
		}

		// 	membw ok?
		// 		no -> mark it
		if (membw > 0.9) {
			marked_machines.push_back(c.first);
		}
	}
	return marked_machines;
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
			membw_util[c.first][c.second] = 1 - distgen_res[i];
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
				// TODO think about migration the marked machines
				// for all marked
				// 	check if there is another host available
				//		yes: save pair for swap
				//		no: job must be suspended
				// if yes for all: swap
				//                 assert(check_membw(config).size() == 0);
				//                 break;
				// if no:
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
