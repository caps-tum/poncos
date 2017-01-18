/**
 * Poor mans scheduler
 *
 * Copyright 2016 by LRR-TUM
 * Jens Breitbart     <j.breitbart@tum.de>
 *
 * Licensed under GNU General Public License 2.0 or later.
 * Some rights reserved. See LICENSE
 */

#ifndef poncos_hpp
#define poncos_hpp

#include <cassert>
#include <condition_variable>
#include <string>
#include <vector>
#include <list>

#include <fast-lib/log.hpp>

struct vm_pool_elemT {
	std::string name;
	std::string mac_addr;
};
extern std::list<vm_pool_elemT> glob_vm_pool;



struct sched_configT {
	std::vector<unsigned char> cpus;
	std::vector<unsigned char> mems;
};

constexpr size_t SLOT_SIZE = 8;
constexpr size_t SLOTS = 2;
extern const sched_configT co_configs[SLOTS];

void read_file(const std::string& filename, std::vector<std::string> &command_queue);

std::ostream &operator<<(std::ostream &os, const std::vector<size_t> &vec);
#endif /* end of include guard: poncos_hpp */
