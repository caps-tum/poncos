#include "poncos/scheduler.hpp"

#include <fast-lib/message/agent/mmbwmon/reply.hpp>
#include <fast-lib/message/agent/mmbwmon/request.hpp>

// inititalize fast-lib log
FASTLIB_LOG_INIT(scheduler_log, "scheduler")
FASTLIB_LOG_SET_LEVEL_GLOBAL(scheduler_log, info);

schedulerT::~schedulerT() = default;

std::vector<double> schedulerT::run_distgen(fast::MQTT_communicator &comm, const std::vector<std::string> &machines,
											const controllerT::execute_config &config) {

	assert(!config.empty());
	// ask for measurements
	{
		for (const auto &c : config) {
			fast::msg::agent::mmbwmon::request m;

			const auto &slot_conf = co_configs[c.second];

			// TODO check if we can use the same type
			m.cores.resize(slot_conf.cpus.size());
			for (size_t i = 0; i < slot_conf.cpus.size(); ++i) {
				m.cores[i] = static_cast<size_t>(slot_conf.cpus[i]);
			}

			const std::string topic = "fast/agent/" + machines[c.first] + "/mmbwmon/request";
			FASTLIB_LOG(scheduler_log, debug) << "sending message \n topic: " << topic << "\n message:\n" << m.to_string();
			comm.send_message(m.to_string(), topic);
		}
	}

	std::vector<double> ret;
	ret.reserve(config.size());

	// wait for results
	{
		for (const auto &c : config) {
			fast::msg::agent::mmbwmon::reply m;
			const std::string topic = "fast/agent/" + machines[c.first] + "/mmbwmon/response";
			FASTLIB_LOG(scheduler_log, debug) << "Waiting on topic: " << topic << " ... ";
			m.from_string(comm.get_message(topic));
			FASTLIB_LOG(scheduler_log, debug) << "Message received!";

			ret.push_back(m.result);
		}
	}

	return ret;
}
