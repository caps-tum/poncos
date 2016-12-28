#include "poncos/poncos.hpp"

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

// generate command of the form:
// 	mpiexec -np <nprocs> -hosts <host_list> <command p0 p1>
std::string jobT::generate_command(std::unordered_map<std::string, vm_pool_elemT> virt_cluster) {
	std::string host_list;
	for (auto cluster_elem : virt_cluster) {
		host_list += cluster_elem.second.name + ",";
	}
	host_list.replace(host_list.size(), 1, " ");
	return "mpiexec -np " + std::to_string(nprocs) + " -hosts " + host_list + command;
}

job_queueT::job_queueT(std::vector<jobT> jobs) : jobs(std::move(jobs)) {}

job_queueT::job_queueT(const std::string &yaml_str) { from_string(yaml_str); }

YAML::Node job_queueT::emit() const {
	YAML::Node node;
	node["job-list"] = jobs;

	return node;
}

void job_queueT::load(const YAML::Node &node) {
	fast::load(jobs, node["job-list"]);
}

std::ostream &operator<<(std::ostream &os, const jobT &job) {
	os << "mpiexec -np ";
	os << job.nprocs;
	os << " -hosts <host_list> ";
	os << job.command;

	return os;
}


