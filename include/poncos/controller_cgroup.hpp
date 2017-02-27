/**
 * Poor mans scheduler
 *
 * Copyright 2016 by Jens Breitbart
 * Jens Breitbart     <jbreitbart@gmail.com>
 *
 * Licensed under GNU General Public License 2.0 or later.
 * Some rights reserved. See LICENSE
 */

#ifndef poncos_controller_cgroup
#define poncos_controller_cgroup

#include <memory>
#include <string>
#include <utility>

#include "poncos/controller.hpp"
#include "poncos/job.hpp"
#include "poncos/poncos.hpp"

#include <fast-lib/mqtt_communicator.hpp>

class cgroup_controller : public controllerT {
  public:
	cgroup_controller(const std::shared_ptr<fast::MQTT_communicator> &_comm, const std::string &machine_filename);
	~cgroup_controller();

	void init();
	void dismantle();

	// freezes all cgroups with supplied id
	void freeze(const size_t id);
	// thaws all cgroups with the supplied id
	void thaw(const size_t id);
	// freeze cgroups opposing to the supplied id
	void freeze_opposing(const size_t id);
	// thaws cgroups opposing to the supplied id
	void thaw_opposing(const size_t id);
	// not supported
	void update_config(const size_t id, const execute_config &new_config);
	bool update_supported() { return false; }

  private:
	controllerT::execute_config sort_config_by_hostname(const execute_config &config) const;
	std::string generate_command(const jobT &job, size_t counter, const execute_config &config) const;
	std::string domain_name_from_config_elem(const execute_config_elemT &config_elem) const;

	template <typename messageT> void send_message(const execute_config &config, const std::string &topic_adn) const;

  private:
};

#endif /* end of include guard: poncos_controller_cgroup */
