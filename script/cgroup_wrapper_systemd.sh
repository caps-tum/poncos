#!/bin/bash

CGROUP="/sys/fs/cgroup/cpuset/$1"
FREEZER="/sys/fs/cgroup/freezer/$1"

echo "$HOSTNAME: writing $$ to $CGROUP/tasks"
echo $$ > $CGROUP/tasks

echo "$HOSTNAME: writing $$ to $FREEZER/tasks"
echo $$ > $FREEZER/tasks

echo "$HOSTNAME: starting ${@:2}"
${@:2}
