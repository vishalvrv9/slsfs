#!/bin/bash

source avaliable-host.sh
export hosts=("${hosts16[@]}")

export EACH_CLIENT_ISSUE=5000
export TOTAL_CLIENT=16
export BUFSIZE=4096
export UNIFORM_DIST="--uniform-dist"
#export UNIFORM_DIST=""

#export CLIENT_TESTNAME=50-50
#export CLIENT_TESTNAME=fill
export CLIENT_TESTNAME=100-0
#export CLIENT_TESTNAME=0-100

#export BACKEND_CONFIG=/backend/cassandra-repl3.json
#export BACKEND_CONFIG=/backend/ssbd-basic-async.json
export BACKEND_CONFIG=/backend/ssbd.json
#export BACKEND_CONFIG=/backend/ssbd-debug.json
#export BACKEND_CONFIG=/backend/ssbd-single.json
#export BACKEND_CONFIG=/backend/ssbd-stripe.json
#export BACKEND_CONFIG=/backend/ssbd-basic.json

export MEMO="Proxy4+2kb"

export BACKEND_CONFIG_NAME=$(echo ${BACKEND_CONFIG} | sed 's/\/backend\///g' | sed 's/.json//g')
export BACKEND_BLOCKSIZE=4096

export UPLOAD_GDRIVE=11XxuGx1nAAUyJBq1e-Gltdml0h6UJDKWEKe_1CgxVoU
export UPLOAD_GDRIVE=1J4ZMcP0RF6zGHzocOtFMgNe8G92TPPG5Y1oE5Z7UyeI
export UPLOAD_GDRIVE=1luyQR39ALkms4cvKTsmQO1nVrQhDF-P8IDIQ9f8jDuU
export UPLOAD_GDRIVE=1lIPaPVSvfBi0ArjKag6Z-0spgltrRo1Yk2bJ3pgc7So #New best backend
#export UPLOAD_GDRIVE=1J4ZMcP0RF6zGHzocOtFMgNe8G92TPPG5Y1oE5Z7UyeI #repl
#export UPLOAD_GDRIVE=1luyQR39ALkms4cvKTsmQO1nVrQhDF-P8IDIQ9f8jDuU #ceph
#export UPLOAD_GDRIVE=1pApoEAeNjSc2zx_VSOvN0DfF4e_X8oX6n3DnKW9FB1g
#export UPLOAD_GDRIVE=1hSNHYNEQEh0MsqcPGYtKwDlxAI29vM4mwJ31cZydqqw
#export UPLOAD_GDRIVE=13v7F8u5T4oTz5y2FouoM_rOZlsJ5xpRRIzLriEz55K4

# [random-assign, lowest-load, active-load-balance]
export POLICY_FILETOWORKER=active-load-balance
export POLICY_FILETOWORKER_ARGS=""

# [const-average-load]
export POLICY_LAUNCH=max-queue
export POLICY_LAUNCH_ARGS=10:2000 #average queue = 2kb

# [const-time, moving-interval]
#export POLICY_KEEPALIVE=const-time
#export POLICY_KEEPALIVE_ARGS=100000
export POLICY_KEEPALIVE=moving-interval-global
export POLICY_KEEPALIVE_ARGS=100:60000:10:50

export INITINT=1;
export VERBOSE='-vv'
