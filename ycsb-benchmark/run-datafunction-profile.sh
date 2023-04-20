#!/bin/bash
source start-proxy-args.sh;

TOTAL_REQUEST=80000
TOTAL_DF=4
CONCURRENT_EXECUTER=32
ENDPORT=13000

TESTNAME="R1${BACKEND_CONFIG_NAME}_T+${CONCURRENT_EXECUTER}_EXEC+${ENDPORT}+${TOTAL_DF}_DF"
echo "testname: $TESTNAME"

bash -c 'cd ../functions/datafunction; make function;' &
bash -c 'source start-proxy-args.sh; cd ../proxy; make from-docker;' &
bash -c "cd ../ssbd;  make from-docker; ./transfer_images.sh; ./start.sh ${BACKEND_BLOCKSIZE}" &
wait < <(jobs -p);

while curl https://localhost:10001/invokers -k 2>&1 | grep -q unhealthy; do
    echo 'waiting invoker restart'
    sleep 1;
done

echo starting;

mkdir -p "test-${TESTNAME}-df-profile-${TESTNAME}";
cd "test-${TESTNAME}-df-profile-${TESTNAME}";

for i in $(seq 13000 $ENDPORT); do
    docker run \
           --privileged \
           -it \
           --rm \
           --entrypoint /bin/datafunction-profile \
           --network=host \
           hare1039/transport:0.0.2 \
               --port $i \
               --announce 192.168.0.224 \
               --worker-config /backend/ssbd.json \
               --total-request $TOTAL_REQUEST \
               --total-df $TOTAL_DF \
               --concurrent-executer $CONCURRENT_EXECUTER 2>&1 | \
        tee pxy_${i}.txt &
done

wait
