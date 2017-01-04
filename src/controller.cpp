#include "poncos/controller.hpp"

#include <iostream>
#include <limits>

#include "poncos/poncos.hpp"

controllerT::controllerT(const std::shared_ptr<fast::MQTT_communicator> &_comm, const std::string &machine_filename)
	: machines(_machines), available_slots(_available_slots), cmd_counter(0), work_counter_lock(worker_counter_mutex),
	  comm(_comm) {

	// fill the machine file
	std::cout << "Reading machine file " << machine_filename << " ...";
	std::cout.flush();
	read_file(machine_filename, _machines);
	std::cout << " done!" << std::endl;

	std::cout << "Machine file:\n";
	std::cout << "==============\n";
	for (std::string c : machines) {
		std::cout << c << "\n";
	}
	std::cout << "==============\n";

	machine_usage.assign(machines.size(), std::array<size_t, 2>{{std::numeric_limits<size_t>::max(),
																 std::numeric_limits<size_t>::max()}});

	_available_slots = machines.size();
}

controllerT::~controllerT() {
	done();
	for (auto &t : thread_pool) {
		if (t.joinable()) t.join();
	}
	thread_pool.resize(0);
}

void controllerT::done() {
	// wait until all workers are finished
	if (!work_counter_lock.owns_lock()) work_counter_lock.lock();
	worker_counter_cv.wait(work_counter_lock, [&] {
		for (size_t i = 0; i < machine_usage.size(); ++i) {
			for (size_t s = 0; s < SLOTS; ++s) {
				if (machine_usage[i][s] != std::numeric_limits<size_t>::max()) return false;
			}
		}

		return true;
	});
}

void controllerT::wait_for_ressource(const size_t requested) {
	if (!work_counter_lock.owns_lock()) work_counter_lock.lock();

	worker_counter_cv.wait(work_counter_lock, [&] {
		// TODO maybe we should not do this lazy but keep it updated all the time
		size_t counter = 0;

		for (size_t i = 0; i < machine_usage.size(); ++i) {
			for (size_t s = 0; s < SLOTS; ++s) {
				if (machine_usage[i][s] == std::numeric_limits<size_t>::max()) {
					counter += SLOT_SIZE;
					break;
				}
			}
		}

		return counter >= requested;
	});
}

void controllerT::wait_for_completion_of(const size_t id) {
	assert(id < id_to_tpool.size());
	work_counter_lock.unlock();

	thread_pool[id_to_tpool[id]].join();
}

controllerT::execute_config controllerT::generate_opposing_config(const size_t id) const {
	assert(id < id_to_config.size());
	assert(SLOTS == 2);

	execute_config opposing_config;
	const execute_config &config = id_to_config[id];

	for (auto const &config_elem : config) {
		// TODO: what abour more than two slots per host?
		opposing_config.push_back({config_elem.first, (config_elem.second + 1) % SLOTS});
	}

	return opposing_config;
}

size_t controllerT::execute(const jobT &job, const execute_config &config, std::function<void(size_t)> callback) {
	assert(work_counter_lock.owns_lock());
	assert(config.size() > 0);

	id_to_tpool.push_back(thread_pool.size());
	id_to_config.push_back(config);
	for (size_t i = 0; i < config.size(); ++i) {
		assert(machine_usage[config[i].first][config[i].second] == std::numeric_limits<size_t>::max());
		machine_usage[config[i].first][config[i].second] = cmd_counter;
	}

	const std::string command = generate_command(job, cmd_counter, config);
	thread_pool.emplace_back(&controllerT::execute_command_internal, this, command, cmd_counter, config, callback);

	return cmd_counter++;
}

void controllerT::execute_command_internal(std::string command, size_t counter, const execute_config config,
										   std::function<void(size_t)> callback) {
	const std::string cmd_name = cmd_name_from_id(counter);

	command += " 2>&1 ";
	// command += "| tee ";
	command += "> ";
	command += cmd_name + ".log";

	auto temp = system(command.c_str());
	assert(temp != -1);

	// we are done
	std::cout << ">> \t '" << command << "' completed at configuration " << config[0].second << std::endl;

	std::lock_guard<std::mutex> work_counter_lock(worker_counter_mutex);

	for (size_t i = 0; i < config.size(); ++i) {
		assert(machine_usage[config[i].first][config[i].second] != std::numeric_limits<size_t>::max());
		machine_usage[config[i].first][config[i].second] = std::numeric_limits<size_t>::max();
	}

	callback(config[0].second);
	worker_counter_cv.notify_one();
}

std::string controllerT::cmd_name_from_id(size_t id) const { return std::string("poncos_") + std::to_string(id); }
