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

multi_app_sched_consec::multi_app_sched_consec(const system_configT &system_config) : schedulerT(system_config) {}

// called after a command was completed
void multi_app_sched_consec::command_done(const size_t /*id*/, controllerT & /*controller*/) {}

void multi_app_sched_consec::schedule(const job_queueT &job_queue, fast::MQTT_communicator & /*comm*/,
									  controllerT &controller, std::chrono::seconds /*wait_time*/) {

	const size_t slots = system_config.slots.size();

	// for all commands
	for (const auto &job : job_queue.jobs) {
		assert(job.req_cpus() <= controller.machines.size() * controller.system_config.slot_size() * slots);
		controller.wait_for_ressource(job.req_cpus(), slots);

		// select ressources
		controllerT::execute_config config;
		for (size_t m = 0; m < controller.machine_usage.size(); ++m) {
			const auto &mu = controller.machine_usage[m];

			// the same job should be running on all slots of a node
			assert(mu[0] == mu[1]);

			// take all slots if empty
			if (mu[0] == std::numeric_limits<size_t>::max()) {
				for (size_t s = 0; s < slots; ++s) {
					config.emplace_back(m, s);
				}
			}
			if (config.size() * controller.system_config.slot_size() >= job.req_cpus()) break;
		}
		assert(config.size() * controller.system_config.slot_size() >= job.req_cpus());

		// start job
		controller.execute(job, config, [&](const size_t config) { command_done(config, controller); });
		FASTLIB_LOG(scheduler_multi_app_consec_log, info) << ">> \t starting '" << job;
	}

	controller.done();
}
