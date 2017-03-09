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
									  controllerT &controller, std::chrono::seconds /*wait_time*/) {

	// for all commands
	for (const auto &job : job_queue.jobs) {
		assert(job.req_cpus() <= controller.machines.size() * SLOT_SIZE * SLOTS);
		controller.wait_for_resource(job.req_cpus(), SLOTS);

		controllerT::execute_config config = controller.generate_config(job.req_cpus(), controllerT::exclusive);
		assert(config.size() * SLOT_SIZE >= job.req_cpus());

		// start job
		controller.execute(job, config, [&](const size_t config) { command_done(config, controller); });
		FASTLIB_LOG(scheduler_multi_app_consec_log, info) << ">> \t starting '" << job;
	}

	controller.done();
}
