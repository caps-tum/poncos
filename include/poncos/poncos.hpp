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

#include <array>
#include <cassert>
#include <condition_variable>
#include <iterator>
#include <list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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

void read_file(const std::string &filename, std::vector<std::string> &command_queue);

namespace std {
// The following operator<< are implemented in the std namespace to allow fastlib
// to use argument dependent lookup (ADL) to find these functions (i.e. use the namespace
// of the first argument passed to it to use the function FASTLIB_LOG() << std::vector looks into std.
// Alternative would be to ensure ordering on the includes and make sure theses functions are available
// before fast-lib includes spdlog, but this seems not to be feasible.
template <typename T1, typename T2> std::ostream &operator<<(std::ostream &os, const std::pair<T1, T2> &p) {
	os << "(" << p.first << "," << p.second << ") ";
	return os;
}

template <typename T, size_t N> std::ostream &operator<<(std::ostream &os, const std::array<T, N> &a) {
	os << "[";
	auto it = std::ostream_iterator<T>(os, ",");
	std::copy(std::begin(a), std::end(a), it);
	os << "]";
	return os;
}
template <template <typename, typename> class C, typename T, typename T2>
std::ostream &operator<<(std::ostream &os, const C<T, T2> &vec) {
	os << "[";
	std::stringstream vec_stream;
	for (const auto &vec_elem : vec) {
		vec_stream << vec_elem << ",";
	}

	std::string vec_str = vec_stream.str();
	if (!vec_str.empty()) vec_str.pop_back();
	os << vec_str;
	os << "]";

	return os;
}
}
#endif /* end of include guard: poncos_hpp */
