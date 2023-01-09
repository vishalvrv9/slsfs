#!/bin/bash

source avaliable-host.sh
export hosts=("${hosts16[@]}")

export EACH_CLIENT_ISSUE=5000
export TOTAL_CLIENT=16
export BUFSIZE=4096
export CLIENT_TESTNAME=50-50
export BACKEND_CONFIG=/backend/cassandra-repl3.json
export BACKEND_CONFIG_NAME=$(echo ${BACKEND_CONFIG} | sed 's/\/backend\///g' | sed 's/.json//g')
export BACKEND_BLOCKSIZE=4096

export UPLOAD_GDRIVE=13v7F8u5T4oTz5y2FouoM_rOZlsJ5xpRRIzLriEz55K4

# [random-assign, lowest-load, active-load-balance]
export POLICY_FILETOWORKER=active-load-balance
export POLICY_FILETOWORKER_ARGS=""

# [const-average-load]
export POLICY_LAUNCH=const-average-load
export POLICY_LAUNCH_ARGS=10:8000

# [const-time, moving-interval]
#export POLICY_KEEPALIVE=const-time
#export POLICY_KEEPALIVE_ARGS=100000
export POLICY_KEEPALIVE=moving-interval
export POLICY_KEEPALIVE_ARGS=20:10000:10:50
export INITINT=1;
