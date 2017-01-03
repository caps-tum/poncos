#include "poncos/job.hpp"

#include <fstream>

jobT::jobT(size_t nprocs, std::string command) : nprocs(std::move(nprocs)), command(std::move(command)) {}

YAML::Node jobT::emit() const {
	YAML::Node node;
	node["nprocs"] = nprocs;
	node["cmd"] = command;
	return node;
}

void jobT::load(const YAML::Node &node) {
	fast::load(nprocs, node["nprocs"]);
	fast::load(command, node["cmd"]);
}

job_queueT::job_queueT(std::vector<jobT> jobs) : jobs(std::move(jobs)) {}

job_queueT::job_queueT(const std::string &queue_filename) {
	std::fstream job_queue_file;
	job_queue_file.open(queue_filename);
	std::stringstream job_queue_stream;
	job_queue_stream << job_queue_file.rdbuf();
	from_string(job_queue_stream.str());
}

YAML::Node job_queueT::emit() const {
	YAML::Node node;
	node["job-list"] = jobs;

	return node;
}

void job_queueT::load(const YAML::Node &node) { fast::load(jobs, node["job-list"]); }

std::ostream &operator<<(std::ostream &os, const jobT &job) {
	os << "mpiexec -np ";
	os << job.nprocs;
	os << " -hosts <host_list> ";
	os << job.command;

	return os;
}
