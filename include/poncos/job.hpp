#ifndef poncos_job
#define poncos_job

#include <fast-lib/serializable.hpp>

struct jobT : public fast::Serializable {
	jobT() = default;
	jobT(size_t nprocs, size_t threads_per_proc, std::string command, bool uses_sr_protocol);

	YAML::Node emit() const override;
	void load(const YAML::Node &node) override;
	size_t req_cpus(void) { return nprocs * threads_per_proc; };

	size_t nprocs;
	size_t threads_per_proc;
	std::string command;
	bool uses_sr_protocol;
};
std::ostream &operator<<(std::ostream &os, const jobT &job);

struct job_queueT : public fast::Serializable {
	job_queueT() = default;
	job_queueT(const std::string &queue_filename);
	job_queueT(std::vector<jobT> jobs);

	YAML::Node emit() const override;
	void load(const YAML::Node &node) override;

	std::string title;
	std::vector<jobT> jobs;
	std::string id;
};

YAML_CONVERT_IMPL(jobT)
YAML_CONVERT_IMPL(job_queueT)

#endif /* end of include guard: poncos_job */
