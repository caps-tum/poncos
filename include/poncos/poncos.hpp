/**
 * Poor mans scheduler
 *
 * Copyright 2016 by LRR-TUM
 * Jens Breitbart     <j.breitbart@tum.de>
 *
 * Licensed under GNU General Public License 2.0 or later.
 * Some rights reserved. See LICENSE
 */

#ifndef pons_hpp
#define pons_hpp

#include <cassert>
#include <condition_variable>
#include <string>
#include <vector>

struct sched_configT {
	std::vector<unsigned char> cpus;
	std::vector<unsigned char> mems;
};

constexpr size_t SLOTS = 2;
extern const sched_configT co_configs[SLOTS];

void read_file(std::string filename, std::vector<std::string> &command_queue);
std::string cgroup_name_from_id(size_t id);

#endif /* end of include guard: pons_hpp */
