#include "poncos/controller.hpp"

#include <iostream>
#include <limits>
#include <utility>

#include "poncos/poncos.hpp"

// inititalize fast-lib log
FASTLIB_LOG_INIT(controller_log, "controller")
FASTLIB_LOG_SET_LEVEL_GLOBAL(controller_log, info);

controllerT::controllerT(std::shared_ptr<fast::MQTT_communicator> _comm, const std::string &machine_filename)
	: machines(_machines), available_slots(_available_slots), machine_usage(_machine_usage),
	  id_to_config(_id_to_config), id_to_job(_id_to_job), cmd_counter(0), work_counter_lock(worker_counter_mutex),
	  comm(std::move(_comm)), timestamps(true, "timestamps") {

	// fill the machine file
	FASTLIB_LOG(controller_log, info) << "Reading machine file " << machine_filename << " ...";
	read_file(machine_filename, _machines);

	FASTLIB_LOG(controller_log, info) << "Machine file:";
	FASTLIB_LOG(controller_log, info) << "==============";
	for (const std::string &c : _machines) {
		FASTLIB_LOG(controller_log, info) << c;
	}
	FASTLIB_LOG(controller_log, info) << "==============";

	_machine_usage.assign(_machines.size(), std::array<size_t, 2>{{std::numeric_limits<size_t>::max(),
																   std::numeric_limits<size_t>::max()}});

	_available_slots = _machines.size();
}

controllerT::~controllerT() {
	done();
	for (auto &t : thread_pool) {
		if (t.joinable()) t.join();
	}
	thread_pool.resize(0);

	FASTLIB_LOG(controller_log, info) << "Controller timestamps:";
	FASTLIB_LOG(controller_log, info) << "==========================";
	FASTLIB_LOG(controller_log, info) << "\n" << timestamps.emit();
}

void controllerT::done() {
	// wait until all workers are finished
	if (!work_counter_lock.owns_lock()) work_counter_lock.lock();
	worker_counter_cv.wait(work_counter_lock, [&] {
		for (auto i : machine_usage) {
			for (size_t s = 0; s < SLOTS; ++s) {
				if (i[s] != std::numeric_limits<size_t>::max()) return false;
			}
		}

		return true;
	});
}

void controllerT::freeze(const size_t id) {
	assert(id < id_to_config.size());

	timestamps.tick("freeze-job-#" + std::to_string(id));

	const execute_config &config = id_to_config[id];
	suspend_resume_config<fast::msg::migfra::Suspend>(config);
}

void controllerT::thaw(const size_t id) {
	assert(id < id_to_config.size());

	const execute_config &config = id_to_config[id];
	suspend_resume_config<fast::msg::migfra::Resume>(config);

	timestamps.tock("freeze-job-#" + std::to_string(id));
}

void controllerT::freeze_opposing(const size_t id) {
	const execute_config opposing_config = generate_opposing_config(id);

	suspend_resume_config<fast::msg::migfra::Suspend>(opposing_config);
}

void controllerT::thaw_opposing(const size_t id) {
	const execute_config &opposing_config = generate_opposing_config(id);

	suspend_resume_config<fast::msg::migfra::Resume>(opposing_config);
}

void controllerT::wait_for_ressource(const size_t requested, const size_t slots_per_host) {
	if (!work_counter_lock.owns_lock()) work_counter_lock.lock();

	worker_counter_cv.wait(work_counter_lock, [&] {
		// TODO maybe we should not do this lazy but keep it updated all the time
		size_t counter = 0;

		for (auto i : machine_usage) {
			size_t allocated_slots = 0;
			for (size_t s = 0; s < SLOTS; ++s) {
				if (i[s] == std::numeric_limits<size_t>::max()) {
					++allocated_slots;

					if (allocated_slots == slots_per_host) break;
				}
			}
			if (allocated_slots == slots_per_host) {
				counter += SLOT_SIZE * slots_per_host;
			}
		}

		return counter >= requested;
	});
}

void controllerT::wait_for_change() {
	if (!work_counter_lock.owns_lock()) work_counter_lock.lock();

	worker_counter_cv.wait(work_counter_lock);
}

void controllerT::wait_for_completion_of(const size_t id) {
	assert(id < id_to_tpool.size());
	work_counter_lock.unlock();

	thread_pool[id_to_tpool[id]].join();
}

controllerT::execute_config controllerT::generate_opposing_config(const size_t id) const {
	assert(id < id_to_config.size());
	static_assert(SLOTS == 2, "");

	execute_config opposing_config;
	const execute_config &config = id_to_config[id];

	for (auto const &config_elem : config) {
		// TODO: what abour more than two slots per host?
		opposing_config.emplace_back(config_elem.first, (config_elem.second + 1) % SLOTS);
	}

	return opposing_config;
}

void controllerT::update_config(const size_t id, const execute_config &new_config) {
	execute_config &old_config = _id_to_config[id];
	assert(new_config.size() == old_config.size());

	// update id_to_config for all affected jobs and machine_usage.
	for (size_t i = 0; i < new_config.size(); ++i) {
		auto &oc = old_config[i];
		const auto &nc = new_config[i];

		// get the config of the opposing job
		const size_t op_job_id = machine_usage[nc.first][nc.second];
		std::swap(_machine_usage[oc.first][oc.second], _machine_usage[nc.first][nc.second]);

		// is there any "oposite job"?
		if (op_job_id == std::numeric_limits<size_t>::max()) {
			// no? We only need to update our config
			oc = nc;
			continue;
		}

		// yes? get the oposing config
		execute_config &op_config = _id_to_config[op_job_id];

		// find entry that matches our current entry in the new config
		bool success = false;
		for (auto &opc : op_config) {
			if (opc.first == nc.first && opc.second == nc.second) {
				// swap both configs
				std::swap(opc, oc);
				success = true;
				break;
			}
		}
		assert(success);
	}

	// "old" config should now be updated to the new config
	assert(old_config == new_config);
}

size_t controllerT::execute(const jobT &job, const execute_config &config, std::function<void(size_t)> callback) {
	assert(work_counter_lock.owns_lock());
	assert(!config.empty());

	id_to_tpool.push_back(thread_pool.size());
	_id_to_config.push_back(config);
	_id_to_job.push_back(job);
	for (const auto &i : config) {
		assert(machine_usage[i.first][i.second] == std::numeric_limits<size_t>::max());
		_machine_usage[i.first][i.second] = cmd_counter;
	}

	// create domain before job start
	create_domain(cmd_counter);

	const std::string command = generate_command(job, cmd_counter, config);
	thread_pool.emplace_back(&controllerT::execute_command_internal, this, command, cmd_counter, callback);

	return cmd_counter++;
}

void controllerT::execute_command_internal(std::string command, size_t counter,
										   const std::function<void(size_t)> &callback) {
	const std::string cmd_name = cmd_name_from_id(counter);

	// command += "| tee ";
	command += "> ";
	command += cmd_name + ".log";
	command += " 2>&1 ";

	FASTLIB_LOG(controller_log, info) << "Executing command: " << command;

	timestamps.tick("job-#" + std::to_string(counter));
	auto temp = system(command.c_str());
	timestamps.tock("job-#" + std::to_string(counter));
	assert(temp != -1);

	// we are done
	std::lock_guard<std::mutex> work_counter_lock(worker_counter_mutex);


	// cleanup
	delete_domain(counter);

	controllerT::execute_config cur_config = id_to_config[counter];
	FASTLIB_LOG(controller_log, info) << ">> \t '" << command << "' completed at configuration "
									  << cur_config[0].second;

	for (const auto &i : cur_config) {
		assert(machine_usage[i.first][i.second] != std::numeric_limits<size_t>::max());
		_machine_usage[i.first][i.second] = std::numeric_limits<size_t>::max();
	}

	callback(counter);
	worker_counter_cv.notify_all();
}

std::string controllerT::cmd_name_from_id(size_t id) const { return std::string("poncos_") + std::to_string(id); }

template <typename T> void controllerT::suspend_resume_config(const execute_config &config) {
	// request OP
	for (auto config_elem : config) {
		std::string topic = "fast/migfra/" + machines[config_elem.first] + "/task";

		auto task = std::make_shared<T>(domain_name_from_config_elem(config_elem), true);

		fast::msg::migfra::Task_container m;
		m.tasks.push_back(task);

		comm->send_message(m.to_string(), topic);
	}

	// wait for results
	fast::msg::migfra::Result_container response;
	for (auto config_elem : config) {
		std::string topic = "fast/migfra/" + machines[config_elem.first] + "/result";
		response.from_string(comm->get_message(topic));

		if (response.results.front().status != "success") {
			assert(response.results.front().details !=
				   "Error suspending domain: Requested operation is not valid: domain is not running");
		}
	}
}
