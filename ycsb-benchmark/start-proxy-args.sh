#!/bin/bash

source avaliable-host.sh
export hosts=("${hosts1[@]}")

export EACH_CLIENT_ISSUE=5000
export TOTAL_CLIENT=1
export BUFSIZE=4096
export CLIENT_TESTNAME=fill
export BACKEND_CONFIG=/backend/ssbd-stripe.json
export BACKEND_CONFIG_NAME=$(echo ${BACKEND_CONFIG} | sed 's/\/backend\///g' | sed 's/.json//g')
export BACKEND_BLOCKSIZE=4096

# [random-assign, lowest-load]
export POLICY_FILETOWORKER=active-load-balance
export POLICY_FILETOWORKER_ARGS=""

# [const-average-load]
export POLICY_LAUNCH=const-average-load
export POLICY_LAUNCH_ARGS=10:6000

# [const-time, moving-interval]
export POLICY_KEEPALIVE=const-time
export POLICY_KEEPALIVE_ARGS=10000
