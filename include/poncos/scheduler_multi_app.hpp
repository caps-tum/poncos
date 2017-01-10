#ifndef scheduler_multi_hpp
#define scheduler_multi_hpp

#include "poncos/scheduler.hpp"

struct multi_app_sched : public schedulerT {

	virtual void schedule(const job_queueT &job_queue, fast::MQTT_communicator &comm, controllerT &controller,
						  std::chrono::seconds wait_time);
	virtual void command_done(const size_t id, controllerT &controller);

	std::vector<size_t> check_membw(const controllerT::execute_config &config) const;
	std::vector<size_t> find_swap_candidates(const std::vector<size_t> &marked_machines) const;
	controllerT::execute_config generate_new_config(const controllerT::execute_config &old_config, const std::vector<size_t> marked_machines, const std::vector<size_t> swap_candidates);
	std::vector<size_t> sort_machines_by_membw_util(const std::vector<size_t> &machine_idxs, const bool reverse) const;

	double membw_util_of_node(const size_t &idx) const;
	std::vector<std::array<double, SLOTS>> membw_util;
};

#endif /* end of include guard: scheduler_multi_hpp */
