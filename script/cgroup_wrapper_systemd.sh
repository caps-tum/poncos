#!/bin/bash

CGROUP="/sys/fs/cgroup/cpuset/$1"
FREEZER="/sys/fs/cgroup/freezer/$1"

echo "$HOSTNAME: creating cgroup $CGROUP"
mkdir -p $CGROUP
mkdir -p $FREEZER

echo "$HOSTNAME: writing $2 to $CGROUP/cpuset.cpus"
echo $2 > $CGROUP/cpuset.cpus

echo "$HOSTNAME: writing $3 to $CGROUP/cpuset.mems"
echo $3 > $CGROUP/cpuset.mems

echo "$HOSTNAME: writing $$ to $CGROUP/tasks"
echo $$ > $CGROUP/tasks

echo "$HOSTNAME: writing $$ to $FREEZER/tasks"
echo $$ > $FREEZER/tasks

echo "$HOSTNAME: starting ${@:4}"
${@:4}

# ignore error TODO: we need a post hook deleting the cgroups
rmdir $CGROUP || true
rmdir $FREEZER || true
