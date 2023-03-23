#!/bin/bash
source start-proxy-args.sh;

echo "Username: $1";

TESTNAME="${BACKEND_CONFIG_NAME}_T+${1}_P+${POLICY_FILETOWORKER}+${POLICY_LAUNCH}+${POLICY_KEEPALIVE}"
echo "testname: $TESTNAME"

ssh proxy-1 docker rm -f proxy2&

bash -c 'cd ../functions/datafunction; make function-debug;' &
bash -c 'cd ../proxy; make debug-from-docker; ./transfer_images.sh' &
bash -c "cd ../ssbd; make debug-from-docker; ./transfer_images.sh; ./start.sh ${BACKEND_BLOCKSIZE}" &
bash -c "cd ../../soufiane/serverlessfs/bench/trace-emulator; make from-docker; "&
wait < <(jobs -p);

start-proxy-remote()
{
    local h=192.168.0.135;
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

wait < <(jobs -p);

mkdir -p trace_emulator_rerun/$TESTNAME-result;
mkdir -p trace_emulator_rerun/$TESTNAME-result/activation-list/;

start-watching-ow()
{
    for i in {1..10000}; do
        wsk -i activation list --limit 200 --skip 0 > trace_emulator_rerun/$TESTNAME-result/activation-list/ow-activation-list-$i.txt;
        sleep 60;
    done
}

start-watching-ow &
OW_WATCHING=$!

ssh ow-invoker-1

cd "trace_emulator_rerun/$TESTNAME-result/";

cp ../../start-proxy-args.sh .
scp proxy-1:/tmp/proxy-report.json proxy-report-1.json;
ssh proxy-1 docker logs proxy2 2>  docker-log-ow-invoker-1.backup.txt;
scp ow-invoker-1:report.csv "report.csv"
cat activation-list/*.txt \
    | awk '!seen[$0]++' > "activation-list.txt" && \
    rm -r activation-list;

kill $OW_WATCHING >/dev/null 2>&1

wait < <(jobs -p);

echo -e "\a finish test: $TESTNAME"
