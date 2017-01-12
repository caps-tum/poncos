/**
 * Simple scheduler.
 *
 * Copyright 2016 by LRR-TUM
 * Jens Breitbart     <j.breitbart@tum.de>
 *
 * Licensed under GNU General Public License 2.0 or later.
 * Some rights reserved. See LICENSE
 */

#include <chrono>
#include <iomanip>
#include <iostream>

#include "poncos/cgroup_controller.hpp"
#include "poncos/poncos.hpp"
#include "poncos/scheduler.hpp"
#include "poncos/scheduler_multi_app.hpp"
#include "poncos/scheduler_two_app.hpp"
#include "poncos/vm_controller.hpp"

#include <fast-lib/message/migfra/time_measurement.hpp>
#include <fast-lib/mqtt_communicator.hpp>

// inititalize fast-lib log
FASTLIB_LOG_INIT(poncos_log, "poncos")
FASTLIB_LOG_SET_LEVEL_GLOBAL(poncos_log, trace);

// COMMAND LINE PARAMETERS
static std::string server;
static size_t port = 1883;
static std::string queue_filename;
static std::string machine_filename;
static std::string slot_path;
static std::chrono::seconds wait_time(20);
static bool use_vms = false;
static bool use_multi_sched = false;

[[noreturn]] static void print_help(const char *argv) {
	std::cout << argv << " supports the following flags:\n";
	std::cout << "\t --vm \t\t\t Enable the usage of VMs. \t\t\t Default: disabled\n";
	std::cout << "\t --multi-sched \t\t Use the multi-app scheduler. \t\t\t Default: disabled\n";
	std::cout << "\t --server \t\t URI of the MQTT broker. \t\t\t Required!\n";
	std::cout << "\t --port \t\t Port of the MQTT broker. \t\t\t Default: 1883\n";
	std::cout << "\t --queue \t\t Filename for the job queue. \t\t\t Required!\n";
	std::cout << "\t --machine \t\t Filename containing node names. \t\t Required!\n";
	std::cout << "\t --slot-path \t\t VM only: Path to XML slot specifications. \t Required!\n";
	std::cout << "\t --wait \t\t Seconds to wait before starting distgen. \t Default: 20\n";

	exit(0);
}

static void parse_options(size_t argc, const char **argv) {
	if (argc == 1) {
		print_help(argv[0]);
	}

	for (size_t i = 1; i < argc; ++i) {
		std::string arg(argv[i]);

		if (arg == "--server") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			server = std::string(argv[i + 1]);
			++i;
			continue;
		}
		if (arg == "--port") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			port = std::stoul(std::string(argv[i + 1]));
			++i;
			continue;
		}

		if (arg == "--queue") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			queue_filename = std::string(argv[i + 1]);
			++i;
			continue;
		}
		if (arg == "--machine") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			machine_filename = std::string(argv[i + 1]);
			++i;
			continue;
		}

		if (arg == "--vm") {
			use_vms = true;
			continue;
		}
		if (arg == "--slot-path") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			slot_path = std::string(argv[i + 1]);
			++i;
			continue;
		}

		if (arg == "--wait") {
			if (i + 1 >= argc) {
				print_help(argv[0]);
			}
			wait_time = std::chrono::seconds(std::stoul(std::string(argv[i + 1])));
			++i;
			continue;
		}

		if (arg == "--multi-sched") {
			use_multi_sched = true;
			continue;
		}
	}

	if (queue_filename == "" || machine_filename == "") print_help(argv[0]);
	if (use_vms && slot_path == "") print_help(argv[0]);
}

int main(int argc, char const *argv[]) {
	parse_options(static_cast<size_t>(argc), argv);

	FASTLIB_LOG(poncos_log, trace) << "Reading job queue " << queue_filename << " ...";
	job_queueT job_queue(queue_filename);

	FASTLIB_LOG(poncos_log, trace) << "Job queue:";
	FASTLIB_LOG(poncos_log, trace) << "==============";
	for (const auto &job : job_queue.jobs) {
		FASTLIB_LOG(poncos_log, trace) << job;
	}
	FASTLIB_LOG(poncos_log, trace) << "==============";

	auto comm = std::make_shared<fast::MQTT_communicator>("fast/poncos", "fast/poncos", "fast/poncos", server,
														  static_cast<int>(port), 60);

	controllerT *controller;

	if (use_vms)
		controller = new vm_controller(comm, machine_filename, slot_path);
	else
		controller = new cgroup_controller(comm, machine_filename);

	schedulerT *sched;
	if (use_multi_sched)
		sched = new multi_app_sched();
	else
		sched = new two_app_sched();

	// Create Time_measurement instance
	fast::msg::migfra::Time_measurement timers(true);

	// start virtual clusters on all slots
	timers.tick("Start time");
	controller->init();
	timers.tock("Start time");

	// subscribe to the various topics
	for (const std::string &mach : controller->machines) {
		std::string topic = "fast/agent/" + mach + "/mmbwmon/response";
		comm->add_subscription(topic);
	}

	FASTLIB_LOG(poncos_log, trace) << "MQTT ready!";

	timers.tick("Runtime");
	sched->schedule(job_queue, *comm, *controller, wait_time);
	timers.tock("Runtime");

	timers.tick("Stop time");
	controller->dismantle();
	timers.tock("Stop time");

	double total_time = 0;
	for (auto timer : timers.emit()) {
		total_time += timer.second.as<double>();
	}

	// print timer
	std::stringstream total_time_stream;
	total_time_stream << std::fixed << total_time;
	std::string total_time_str = total_time_stream.str();
	FASTLIB_LOG(poncos_log, trace) << "Start time: " <<  timers.emit()["Start time"].as<double>() << " s";
	FASTLIB_LOG(poncos_log, trace) << "Runtime   : " << timers.emit()["Runtime"].as<double>() << " s";
	FASTLIB_LOG(poncos_log, trace) << "Stop time : " << timers.emit()["Stop time"].as<double>() << " s";
	FASTLIB_LOG(poncos_log, trace) << "Total time: " << total_time_str << " s";

	delete sched;
	delete controller;
}
