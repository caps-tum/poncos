#include "poncos/scheduler_multi_app.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <numeric>

#include "poncos/cgroup_controller.hpp"
#include "poncos/job.hpp"
#include "poncos/poncos.hpp"

// per machine threshold for the membw utilization
constexpr double PER_MACHINE_TH = 0.9;

double multi_app_sched::membw_util_of_node(const size_t &idx) const {
	assert(idx < membw_util.size());

	double total_membw_util = 0;
	for (size_t slot = 0; slot < SLOTS; ++slot) {
		total_membw_util += membw_util[idx][slot];
	}

	return total_membw_util;
}

std::vector<size_t> multi_app_sched::sort_machines_by_membw_util(const std::vector<size_t> &machine_idxs,
																 const bool reverse) const {
	assert(machine_idxs.size() <= membw_util.size());

	// determine membw_util per node
	std::vector<double> total_membw_util(membw_util.size());
	for (size_t idx = 0; idx < membw_util.size(); ++idx) {
		total_membw_util.emplace_back(membw_util_of_node(idx));
	}

	// sort machine indices in accordance with the nodes' total_membw_util
	std::vector<size_t> sorted_machine_idxs = machine_idxs;
	std::sort(sorted_machine_idxs.begin(), sorted_machine_idxs.end(),
			  [&total_membw_util, &reverse](size_t i1, size_t i2) {
				  if (reverse) {
					  return total_membw_util[i1] > total_membw_util[i2];
				  } else {
					  return total_membw_util[i1] < total_membw_util[i2];
				  }
			  });

	return sorted_machine_idxs;
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
		if (membw > PER_MACHINE_TH) {
			marked_machines.push_back(c.first);
		}
	}
	return marked_machines;
}

controllerT::execute_config multi_app_sched::generate_new_config(const controllerT::execute_config &old_config,
																 const std::vector<size_t> marked_machines,
																 const std::vector<size_t> swap_candidates) {
	// are there any swap candidates?
	if (swap_candidates.empty()) {
		return {};
	}
	assert(marked_machines.size() == swap_candidates.size());

	std::vector<size_t> marked_machines_sorted = sort_machines_by_membw_util(marked_machines, true);

	// determine sorted swap config
	//
	// 	marked_machines are iterated in descending order with respect
	// 	to their total_membw_util while the swap_candidates are iterated
	// 	in ascending order, i.e., we always match nodes with high
	// 	total_membw_util to those we a low value. For each pair choose
	// 	the swap slot such that the variance of the total_membw_util of
	// 	the respective nodes is minimized (see below).
	controllerT::execute_config new_config_sorted(old_config.size());
	for (size_t idx = 0; idx < marked_machines.size(); ++idx) {
		const size_t old_mach = marked_machines_sorted[idx];
		const size_t new_mach = swap_candidates[idx];

		// determine slot on old_mach
		const auto old_slot_it = std::find_if(old_config.begin(), old_config.end(),
											  [old_mach](auto &config_elem) { return config_elem.first == old_mach; });
		const size_t old_slot = old_slot_it->second;

		// determine 'optimal' slot on new_mach
		//
		// 	This is done by minimizing the total_membw_util
		// 	variances of old_mach and new_mach, i.e., if the old
		// 	slot exhibits the lower membw_util on old_mach, we
		// 	should swap with the slot having the higher value on the
		// 	new_mach and vice versa.
		std::array<double, SLOTS>::iterator new_slot_it;
		if (membw_util[old_mach][old_slot] <
			membw_util[old_mach][(old_slot + 1) % SLOTS]) { // TODO: what about more than 2 SLOTS?
			new_slot_it = std::max_element(membw_util[new_mach].begin(), membw_util[new_mach].end());
		} else {
			new_slot_it = std::min_element(membw_util[new_mach].begin(), membw_util[new_mach].end());
		}

		new_config_sorted.emplace_back(new_mach, std::distance(membw_util[new_mach].begin(), new_slot_it));
	}
	assert(new_config_sorted.size() == marked_machines.size());

	// sort swap-config in accordance with order of marked_machines
	controllerT::execute_config new_config;
	for (auto &config_elem : old_config) {
		auto machine_pos = std::find(marked_machines_sorted.begin(), marked_machines_sorted.end(), config_elem.first);
		if (machine_pos != marked_machines_sorted.end()) {
			new_config.emplace_back(new_config_sorted[machine_pos - marked_machines_sorted.begin()]);
		} else {
			new_config.emplace_back(config_elem);
		}
	}

	return new_config;
}

// find nodes that are eligible to resolve the overload of marked_machines
// 	assumptions: -- marked_machines are N overloaded nodes
// 	             -- good candidates have a preferably low total_membw_util
// 	condition  : we can only resolve the overload if the total_membw_util
// 	             utilization of marked_machines and the swap candidates does
// 	             not exceed N * PER_MACHINE_TH * 2
// 	goal       : find the N nodes with lowest total_membw_util such that the
// 	             condition is met
std::vector<size_t> multi_app_sched::find_swap_candidates(const std::vector<size_t> &marked_machines) const {
	// determine swap candidates
	// -> sorted list of machine indices in accordance with membw_util
	std::vector<size_t> swap_candidates(membw_util.size());
	std::iota(swap_candidates.begin(), swap_candidates.end(), 0);
	swap_candidates = sort_machines_by_membw_util(swap_candidates, false);

	// calculate current total membw_util for all marked machines and
	// swap candidates
	double total_membw_util = 0;
	for (size_t idx = 0; idx < marked_machines.size(); ++idx) {
		total_membw_util += membw_util_of_node(marked_machines[idx]);
		total_membw_util += membw_util_of_node(swap_candidates[idx]);
	}

	// are we able to find a new config?
	// -> if the total sum of all membw_utils exceeds the some of all
	//    thresholds, a new config won't be able to resolve the overload
	if (total_membw_util < PER_MACHINE_TH * marked_machines.size() * 2) {
		swap_candidates.resize(marked_machines.size());
		return swap_candidates;
	}

	return {};
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
			const auto marked_machines = check_membw(config);

			// everything fine?
			if (marked_machines.empty()) break;

			if (controller.update_supported()) {
				const std::vector<size_t> swap_candidates = find_swap_candidates(marked_machines);
				controllerT::execute_config old_config = controller.id_to_config[job_id];
				controllerT::execute_config new_config =
					generate_new_config(old_config, marked_machines, swap_candidates);
				assert(new_config.size() == old_config.size());

				if (!new_config.empty()) {
					controller.update_config(job_id, new_config);
					break;
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
