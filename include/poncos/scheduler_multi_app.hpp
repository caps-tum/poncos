#ifndef scheduler_multi_hpp
#define scheduler_multi_hpp

#include <numeric>

#include "poncos/scheduler.hpp"

#define PER_MACHINE_TH 0.9

struct multi_app_sched : public schedulerT {
	virtual void schedule(const job_queueT &job_queue, fast::MQTT_communicator &comm, controllerT &controller,
						  std::chrono::seconds wait_time);
	virtual void command_done(const size_t id, controllerT &controller);

	std::vector<size_t> check_membw(const controllerT::execute_config &config) const;
	controllerT::execute_config find_new_config(std::vector<size_t> marked_machines);
	controllerT::execute_config generate_optimal_config(std::vector<size_t> marked_machines, std::vector<size_t> swap_candidates) const;
	std::vector<size_t> sort_machines_by_membw_util(void);

	double membw_util_of_node(const size_t &idx);
	std::vector<std::array<double, SLOTS>> membw_util;
};

#endif /* end of include guard: scheduler_multi_hpp */
