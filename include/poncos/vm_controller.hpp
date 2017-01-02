/**
 * Poor mans scheduler
 *
 * Copyright 2017 by Jens Breitbart
 * Jens Breitbart     <jbreitbart@gmail.com>
 *
 * Licensed under GNU General Public License 2.0 or later.
 * Some rights reserved. See LICENSE
 */

#ifndef poncos_vm_controller
#define poncos_vm_controller

#include <condition_variable>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "poncos/controller.hpp"
#include "poncos/job.hpp"
#include "poncos/poncos.hpp"

#include <fast-lib/mqtt_communicator.hpp>
#include <fast-lib/message/migfra/result.hpp>
#include <fast-lib/message/migfra/task.hpp>


class vm_controller : public controllerT {
  public:
	// entries in the vector are read as: (machine index in machinefiles, #slot)
	using execute_config = std::vector<std::pair<size_t, size_t>>;



  public:
	vm_controller(const std::shared_ptr<fast::MQTT_communicator> &_comm, const std::string &machine_filename,
				  const std::string &_slot_path);
	~vm_controller();

	void init();
	void dismantle();

	// freezes all VMs with supplied id
	void freeze(const size_t id);
	// thaws all VMs with the supplied id
	void thaw(const size_t id);

	// wait until ressources are free
	void wait_for_ressource();
	// waits until id has completed its run
	void wait_for_completion_of(const size_t id);
	// wait until all workers are finished
	void done();

	// executes a command
	size_t execute(const jobT &command, const execute_config &config, std::function<void(size_t)> callback);

  private:
	std::string generate_command(const jobT &command, const execute_config &config);
	std::shared_ptr<fast::msg::migfra::Start> generate_start_task(size_t slot, vm_pool_elemT &free_vm);
	void execute_command_internal(std::string command, std::string cg_name, size_t config_used,
								  std::function<void(size_t)> callback);
	void command_done(const size_t config);

	template <typename T> void suspend_resume_virt_cluster(size_t slot);

	void start_all_VMs();
	void stop_all_VMs();

  public:
	// getter
	const std::vector<std::string> &machines() { return _machines; }

  private:
	// a list of all machines
	std::vector<std::string> _machines;

	// lock/cond variable used to wait for a job to be completed
	std::mutex worker_counter_mutex;
	std::condition_variable worker_counter_cv;
	std::unique_lock<std::mutex> work_counter_lock;

	// numbers of active workser
	size_t workers_active;

	// a counter that is increased with every new cgroup created
	size_t cmd_counter;

	// path to the xml slot files
	std::string slot_path;

	// threads used to run the applications
	std::vector<std::thread> thread_pool;

	// maps ids to the thread_pool
	std::unordered_map<size_t, size_t> id_to_pool;

	// maps ids to the slots
	std::unordered_map<size_t, size_t> id_to_slot;

	// stores the VMs used in the two slots
	// entries in the vector are read as: (host-name, guest-name)
	std::vector<std::pair<std::string, std::string>> virt_cluster[SLOTS];

	// reference to a mqtt communictor
	std::shared_ptr<fast::MQTT_communicator> comm;
};

#endif /* end of include guard: poncos_vm_controller */
