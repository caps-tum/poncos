#ifndef poncos_job
#define poncos_job

#include <fast-lib/serializable.hpp>

struct jobT : public fast::Serializable {
	jobT() = default;
	jobT(size_t nprocs, std::string command);

	YAML::Node emit() const override;
	void load(const YAML::Node &node) override;

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

// reads a job_queueT from file
job_queueT read_job_queue_from_file(std::string filename);

#endif /* end of include guard: poncos_job */
