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
#include <list>
#include <string>
#include <vector>

#include <fast-lib/serializable.hpp>

struct job_queueT;
struct jobT;

struct vm_pool_elemT {
	std::string name;
	std::string mac_addr;
};

struct sched_configT {
	std::vector<unsigned char> cpus;
	std::vector<unsigned char> mems;
};

constexpr size_t SLOTS = 2;
extern const sched_configT co_configs[SLOTS];
extern std::list<vm_pool_elemT> vm_pool;

void read_file(std::string filename, std::vector<std::string> &command_queue);
void read_job_queue_file(std::string filename, job_queueT &job_queue);
std::string cgroup_name_from_id(size_t id);

struct jobT : public fast::Serializable {
	jobT() = default;
	jobT(size_t nprocs, std::string command);

	YAML::Node emit() const override;
	void load(const YAML::Node &node) override;
	std::string generate_command(std::unordered_map<std::string, vm_pool_elemT> virt_cluster);

	size_t nprocs;
	std::string command;
};
std::ostream &operator<<(std::ostream &os, const jobT &job);

struct job_queueT : public fast::Serializable {
	job_queueT() = default;
	job_queueT(const std::string &yaml_str);
	job_queueT(std::vector<jobT> jobs);

	YAML::Node emit() const override;
	void load(const YAML::Node &node) override;

	std::string title;
	std::vector<jobT> jobs;
	std::string id;
};

YAML_CONVERT_IMPL(jobT)
YAML_CONVERT_IMPL(job_queueT)

#endif /* end of include guard: pons_hpp */
