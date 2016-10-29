# poncos
[![Build Status](https://travis-ci.org/lrr-tum/poncos.svg?branch=master)](https://travis-ci.org/lrr-tum/poncos)
Poor Man's Co-Scheduler

Poncos is a simple scheduler that applies co-scheduling of applications based
on main memory bandwidth utilization. It supports multiple nodes

## Requirements
The source code in this repository is essential self-contained and all unusual
dependencies are automatically built when compiling poncos. Nonetheless, you
require:

* A compiler supporting C++14.
* CMake (version >= 3.1)

## Setup
To use poncos you must:

1. first install [mmbwmon](https://github.com/lrr-tum/mmbwmon) on all nodes
   you plan to use for scheduling.
2. start mmbwmon on all nodes while they are idle. Wait until the initialization
   phase of mmbwmon has completed.
3. if you are not using a SandyBridge EP system, adjust the file
   system_config/sandybridge_EP.cpp to match your system (or create a new one
   and add a new build target to CMake).
4. compile poncos as every other CMake based project.
5. create a machine-file containing all hostnames of the nodes you want to use
   for your applications. Use linebreaks to separate the hostnames (i.e. one
   hostname per line).
6. create a queue file containing one application command line per line
   (see examples).
7. run the poncos binary with --help and read the options on how to pass the
   machine-file and queue.
