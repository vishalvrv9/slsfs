#!/bin/bash
source start-proxy-args.sh;

echo "Username: $1";

TESTNAME="${BACKEND_CONFIG_NAME}_T+${1}_P+${POLICY_FILETOWORKER}+${POLICY_LAUNCH}+${POLICY_KEEPALIVE}"
echo "testname: $TESTNAME"

ssh proxy-1 docker rm -f proxy2&

bash -c 'cd ../functions/datafunction; make function;' &
bash -c 'cd ../proxy; make from-docker; ./transfer_images.sh' &
bash -c "cd ../ssbd; ./transfer_images.sh; ./start.sh ${BACKEND_BLOCKSIZE}" &
bash -c "cd ../../soufiane/serverlessfs/bench/trace-emulator; make from-docker; "&
wait < <(jobs -p);

start-proxy-remote()
{
    local h=192.168.0.135;
    docker save hare1039/transport:0.0.2  | pv | ssh "$h" docker load &
    scp start-proxy* $h:
    if [[ "$2" == "noinit" ]]; then
        ssh $h "echo 'INITINT=0' >> ./start-proxy-args.sh"
    fi
    scp avaliable-host.sh $h:
    ssh $h "/home/ubuntu/start-proxy.sh"
}

echo starting remote hosts

start-proxy-remote proxy-1
#./start-proxy.sh

docker run --rm hare1039/trace_emulator:build cat /bin/trace_emulator > /tmp/trace_emulator

chmod +x /tmp/trace_emulator

scp /tmp/trace_emulator ow-invoker-1:

ssh ow-invoker-9 "chmod +x trace_emulator"

wait < <(jobs -p);

while curl https://localhost:10001/invokers -k 2>&1 | grep -q unhealthy; do
    echo 'waiting invoker restart'
    sleep 1;
done

echo wait until proxy open

while ! nc -z -v -w1 192.168.0.135 12001 2>&1 | grep -q succeeded; do
    echo 'waiting proxy1 192.168.0.135:12001'
    sleep 1;
done

echo starting;

ssh ow-invoker-1
wait < <(jobs -p);

mkdir -p trace_emulator_12hour/$TESTNAME-result;

cp start-proxy-args.sh trace_emulator_results/$TESTNAME-result/
scp proxy-1:/tmp/proxy-report.json trace_emulator_12hour/$TESTNAME-result/proxy-report-1.json
ssh proxy-1 docker logs proxy2 2> trace_emulator_12hour/$TESTNAME-result/docker-log-ow-invoker-1.backup.txt
wait < <(jobs -p);

cd "trace_emulator_12hour/$TESTNAME-result/"
scp ow-invoker-1:report.csv .
echo -e "\a finish test: $TESTNAME"
