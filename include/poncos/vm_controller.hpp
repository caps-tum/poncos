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

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "poncos/controller.hpp"
#include "poncos/job.hpp"
#include "poncos/poncos.hpp"

#include <fast-lib/message/migfra/result.hpp>
#include <fast-lib/message/migfra/task.hpp>
#include <fast-lib/mqtt_communicator.hpp>

class vm_controller : public controllerT {
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
	// stop VMs opposing to the supplied id
	void stop_opposing(const size_t id);

  private:
	std::string generate_command(const jobT &job, size_t counter, const execute_config &config) const;
	std::shared_ptr<fast::msg::migfra::Start> generate_start_task(size_t slot, vm_pool_elemT &free_vm);

	template <typename T> void suspend_resume_virt_cluster(const execute_config &config);

	void start_all_VMs();
	void stop_all_VMs();

	std::string get_hostname_from_machinename(const std::pair<size_t, size_t> &config) const;

  private:
	// path to the xml slot files
	std::string slot_path;

	// stores the VMs used in the two slots per machine
	// vector index -> machine index (see: machines)
	// vector elem  -> array of VM names per slot
	std::vector<std::array<std::string, SLOTS>> vm_locations;
};

#endif /* end of include guard: poncos_vm_controller */
