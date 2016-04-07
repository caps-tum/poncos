/**
 * po     n  c       i
 * poor mans cgroups interface
 *
 * Copyright 2016 by LRR-TUM
 * Jens Breitbart     <j.breitbart@tum.de>
 *
 * Licensed under GNU Lesser General Public License 2.1 or later.
 * Some rights reserved. See LICENSE
 */

#include "ponci/ponci.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <cassert>
#include <cstdio>
#include <cstring>

#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

// default mount path
static std::string path_prefix("/sys/fs/cgroup/");

// TODO function to change prefix

/////////////////////////////////////////////////////////////////
// PROTOTYPES
/////////////////////////////////////////////////////////////////
static inline std::string cgroup_path(const char *name);
template <typename T> static inline void write_vector_to_file(std::string filename, const std::vector<T> &vec);
template <typename T> static inline void write_array_to_file(std::string filename, T *arr, size_t size);
template <typename T> static inline void write_value_to_file(std::string filename, T val);
template <> inline void write_value_to_file<const char *>(std::string filename, const char *val);
template <typename T> static inline void append_value_to_file(std::string filename, T val);

/////////////////////////////////////////////////////////////////
// EXPORTED FUNCTIONS
/////////////////////////////////////////////////////////////////
void cgroup_create(const char *name) {
	const int err = mkdir(cgroup_path(name).c_str(), S_IRWXU | S_IRWXG);

	if (err != 0 && errno != EEXIST) throw std::runtime_error(strerror(errno));
	errno = 0;
}

void cgroup_delete(const char *name) {
	const int err = rmdir(cgroup_path(name).c_str());

	if (err != 0) throw std::runtime_error(strerror(errno));
}
void cgroup_add_me(const char *name) {
	pid_t me = static_cast<pid_t>(syscall(SYS_gettid));
	cgroup_add_task(name, me);
}

void cgroup_add_task(const char *name, const pid_t tid) {
	std::string filename = cgroup_path(name) + std::string("tasks");

	append_value_to_file(filename, tid);
}

void cgroup_set_cpus(const char *name, const size_t *cpus, size_t size) {
	std::string filename = cgroup_path(name) + std::string("cpuset.cpus");

	write_array_to_file(filename, cpus, size);
}

void cgroup_set_cpus(const std::string &name, const std::vector<unsigned char> &cpus) {
	std::string filename = cgroup_path(name.c_str()) + std::string("cpuset.cpus");
	write_vector_to_file(filename, cpus);
}

void cgroup_set_mems(const char *name, const size_t *mems, size_t size) {
	std::string filename = cgroup_path(name) + std::string("cpuset.mems");

	write_array_to_file(filename, mems, size);
}

void cgroup_set_mems(const std::string &name, const std::vector<unsigned char> &mems) {
	std::string filename = cgroup_path(name.c_str()) + std::string("cpuset.mems");
	write_vector_to_file(filename, mems);
}

void cgroup_set_memory_migrate(const char *name, size_t flag) {
	assert(flag == 0 || flag == 1);
	std::string filename = cgroup_path(name) + std::string("cpuset.memory_migrate");

	write_value_to_file(filename, flag);
}

void cgroup_set_cpus_exclusive(const char *name, size_t flag) {
	assert(flag == 0 || flag == 1);
	std::string filename = cgroup_path(name) + std::string("cpuset.cpu_exclusive");

	write_value_to_file(filename, flag);
}

void cgroup_set_mem_hardwall(const char *name, size_t flag) {
	assert(flag == 0 || flag == 1);
	std::string filename = cgroup_path(name) + std::string("cpuset.mem_hardwall");

	write_value_to_file(filename, flag);
}

void cgroup_set_scheduling_domain(const char *name, int flag) {
	assert(flag >= -1 && flag <= 5);
	std::string filename = cgroup_path(name) + std::string("cpuset.sched_relax_domain_level");

	write_value_to_file(filename, flag);
}

void cgroup_freeze(const char *name) {
	assert(strcmp(name, "") != 0);
	std::string filename = cgroup_path(name) + std::string("freezer.state");

	write_value_to_file(filename, "FROZEN");
}

void cgroup_thaw(const char *name) {
	assert(strcmp(name, "") != 0);
	std::string filename = cgroup_path(name) + std::string("freezer.state");

	write_value_to_file(filename, "THAWED");
}

/////////////////////////////////////////////////////////////////
// INTERNAL FUNCTIONS
/////////////////////////////////////////////////////////////////

static inline std::string cgroup_path(const char *name) {
	std::string res(path_prefix);
	if (strcmp(name, "") != 0) {
		res.append(name);
		res.append("/");
	}
	return res;
}

template <typename T> static inline void write_vector_to_file(std::string filename, const std::vector<T> &vec) {
	write_array_to_file(filename, &vec[0], vec.size());
}

template <typename T> static inline void write_array_to_file(std::string filename, T *arr, size_t size) {
	assert(size > 0);
	assert(filename.compare("") != 0);

	std::string str;
	for (size_t i = 0; i < size; ++i) {
		str.append(std::to_string(arr[i]));
		str.append(",");
	}

	write_value_to_file(filename, str.c_str());
}

template <typename T> static inline void append_value_to_file(std::string filename, T val) {
	assert(filename.compare("") != 0);

	FILE *f = fopen(filename.c_str(), "a+");
	if (f == nullptr) {
		throw std::runtime_error(strerror(errno));
	}
	std::string str = std::to_string(val);

	if (fputs(str.c_str(), f) == EOF && ferror(f) != 0) {
		throw std::runtime_error(strerror(errno));
	}
	if (fclose(f) != 0) {
		throw std::runtime_error(strerror(errno));
	}
}

template <typename T> static inline void write_value_to_file(std::string filename, T val) {
	write_value_to_file(filename, std::to_string(val).c_str());
}

template <> void write_value_to_file<const char *>(std::string filename, const char *val) {
	assert(filename.compare("") != 0);

	FILE *file = fopen(filename.c_str(), "w+");

	if (file == nullptr) {
		throw std::runtime_error(strerror(errno));
	}

	int status = fputs(val, file);
	if (status <= 0 || ferror(file) != 0) {
		throw std::runtime_error(strerror(errno));
	}

	if (fclose(file) != 0) {
		throw std::runtime_error(strerror(errno));
	}
}
