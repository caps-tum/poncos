#include "poncos/scheduler_multi_app_consec.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <numeric>

#include "poncos/controller.hpp"
#include "poncos/job.hpp"
#include "poncos/poncos.hpp"

// inititalize fast-lib log
FASTLIB_LOG_INIT(scheduler_multi_app_consec_log, "multi-app consec scheduler")
FASTLIB_LOG_SET_LEVEL_GLOBAL(scheduler_multi_app_consec_log, info);

// called after a command was completed
void multi_app_sched_consec::command_done(const size_t /*id*/, controllerT & /*controller*/) {}

void multi_app_sched_consec::schedule(const job_queueT &job_queue, fast::MQTT_communicator & /*comm*/,
									  controllerT &controller, std::chrono::seconds wait_time) {

	// for all commands
	for (auto job : job_queue.jobs) {
		assert(job.req_cpus() <= controller.machines.size() * SLOT_SIZE * SLOTS);
		controller.wait_for_ressource(job.req_cpus(), SLOTS);

		// select ressources
		controllerT::execute_config config;
		for (size_t m = 0; m < controller.machine_usage.size(); ++m) {
			const auto &mu = controller.machine_usage[m];

			// the same job should be running on all slots of a node
			assert(mu[0] == mu[1]);

			// take all slots if empty
			if (mu[0] == std::numeric_limits<size_t>::max()) {
				for (size_t s = 0; s < SLOTS; ++s) {
					config.emplace_back(m, s);
				}
			}
			if (config.size() * SLOT_SIZE >= job.req_cpus()) break;
		}
		assert(config.size() * SLOT_SIZE >= job.req_cpus());

		// start job
		controller.execute(job, config, [&](const size_t config) { command_done(config, controller); });
		FASTLIB_LOG(scheduler_multi_app_consec_log, info) << ">> \t starting '" << job;

		// wait ... if we really want to
		std::this_thread::sleep_for(wait_time);
	}

	controller.done();
}
