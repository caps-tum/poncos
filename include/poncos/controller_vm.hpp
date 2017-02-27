/**
 * Poor mans scheduler
 *
 * Copyright 2017 by Jens Breitbart
 * Jens Breitbart     <jbreitbart@gmail.com>
 *
 * Licensed under GNU General Public License 2.0 or later.
 * Some rights reserved. See LICENSE
 */

#ifndef poncos_controller_vm
#define poncos_controller_vm

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
				  std::string _slot_path);
	~vm_controller();

	void init();
	void dismantle();

	// freezes all VMs with supplied id
	void freeze(const size_t id);
	// thaws all VMs with the supplied id
	void thaw(const size_t id);
	// freeze VMs opposing to the supplied id
	void freeze_opposing(const size_t id);
	// thaws VMs opposing to the supplied id
	void thaw_opposing(const size_t id);
	// swaps all slots from the given job with those in the new config
	void update_config(const size_t id, const execute_config &new_config);
	bool update_supported() { return true; }

  private:
	std::string generate_command(const jobT &job, size_t counter, const execute_config &config) const;
	std::string domain_name_from_config_elem(const execute_config_elemT &config_elem) const;
	std::vector<std::vector<unsigned int>> generate_vcpu_map(size_t slot_id) const;
	std::shared_ptr<fast::msg::migfra::Start> generate_start_task(size_t slot, vm_pool_elemT &free_vm);

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

#endif /* end of include guard: poncos_controller_vm */
