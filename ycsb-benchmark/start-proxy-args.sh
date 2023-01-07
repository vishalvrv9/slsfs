#!/bin/bash

source avaliable-host.sh
export hosts=("${hosts16[@]}")

export EACH_CLIENT_ISSUE=50000
export TOTAL_CLIENT=12
export BUFSIZE=4096
export CLIENT_TESTNAME=100-0
export BACKEND_CONFIG=/backend/ssbd-stripe.json
export BACKEND_CONFIG_NAME=$(echo ${BACKEND_CONFIG} | sed 's/\/backend\///g' | sed 's/.json//g')
export BACKEND_BLOCKSIZE=4096

# [random-assign, lowest-load]
export POLICY_FILETOWORKER=lowest-load
export POLICY_FILETOWORKER_ARGS=""

# [const-limit-launch, prestart-one, adaptive-max-load]
export POLICY_LAUNCH=adaptive-max-load
export POLICY_LAUNCH_ARGS=1000:200:5

# [const-time, moving-interval]
export POLICY_KEEPALIVE=const-time
export POLICY_KEEPALIVE_ARGS=2000
