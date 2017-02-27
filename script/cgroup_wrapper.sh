#!/bin/bash

CGROUP="/sys/fs/cgroup/$1"

echo "$HOSTNAME: writing $$ to $CGROUP/tasks"
echo $$ > $CGROUP/tasks

echo "$HOSTNAME: starting ${@:2}"
${@:2}
