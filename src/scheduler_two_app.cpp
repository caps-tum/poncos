#include "poncos/scheduler_two_app.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>

#include "poncos/controller.hpp"
#include "poncos/job.hpp"
#include "poncos/poncos.hpp"

// inititalize fast-lib log
FASTLIB_LOG_INIT(scheduler_two_app_log, "two-app scheduler")
FASTLIB_LOG_SET_LEVEL_GLOBAL(scheduler_two_app_log, trace);

// called after a command was completed
void two_app_sched::command_done(const size_t config, controllerT & /*controller*/) {
	co_config_in_use[config] = false;
	co_config_distgend[config] = 0;
}

void two_app_sched::schedule(const job_queueT &job_queue, fast::MQTT_communicator &comm, controllerT &controller,
							 std::chrono::seconds wait_time) {
	// for all commands
	for (auto job : job_queue.jobs) {
		assert(job.nprocs == controller.machines.size() * SLOT_SIZE);

		controller.wait_for_ressource(job.nprocs);

		// search for a free slot and assign it to a new job
		size_t new_slot = 0;
		size_t job_id = 0;
		for (; new_slot < SLOTS; ++new_slot) {
			if (!co_config_in_use[new_slot]) {
				co_config_in_use[new_slot] = true;

				controllerT::execute_config config;

				for (size_t j = 0; j < controller.machines.size(); ++j) {
					config.emplace_back(j, new_slot);
				}

				job_id =
					controller.execute(job, config, [&](const size_t config) { command_done(config, controller); });

				FASTLIB_LOG(scheduler_two_app_log, trace) << ">> \t starting '" << job << "' at configuration " << new_slot;

				break;
			}
		}
		assert(new_slot < SLOTS);

		// for the initialization phase of the application to be completed
		std::this_thread::sleep_for(wait_time);

		// check if two are running
		if (co_config_in_use[0] && co_config_in_use[1]) {
			FASTLIB_LOG(scheduler_two_app_log, debug) << "0: freezing old";
			controller.freeze_opposing(job_id);
		}

		// measure distgen result
		FASTLIB_LOG(scheduler_two_app_log, trace) << ">> \t Running distgend";
		auto temp = run_distgen(comm, controller.machines, controller.generate_opposing_config(job_id));
		co_config_distgend[new_slot] = *std::max_element(temp.begin(), temp.end());

		FASTLIB_LOG(scheduler_two_app_log, trace) << ">> \t Result for command '" << job << "' is: " << 1 - co_config_distgend[new_slot];

		if (co_config_in_use[0] && co_config_in_use[1]) {
			FASTLIB_LOG(scheduler_two_app_log, debug) << "0: thaw old";
			controller.thaw_opposing(job_id);

			FASTLIB_LOG(scheduler_two_app_log, trace) << ">> \t Estimating total usage of "
					  << (1 - co_config_distgend[0]) + (1 - co_config_distgend[1]);

			if ((1 - co_config_distgend[0]) + (1 - co_config_distgend[1]) > 0.9) {
				FASTLIB_LOG(scheduler_two_app_log, trace) << " -> we will run one";
				FASTLIB_LOG(scheduler_two_app_log, debug) << "0: freezing new";
				controller.freeze(job_id);

				controller.wait_for_ressource(job.nprocs);

				FASTLIB_LOG(scheduler_two_app_log, debug) << "0: thaw new";
				controller.thaw(job_id);
			} else {
				FASTLIB_LOG(scheduler_two_app_log, trace) << " -> we will run both applications";
			}

		} else {
			FASTLIB_LOG(scheduler_two_app_log, trace) << ">> \t Just one config in use ATM";
		}
	}
	controller.done();
}
