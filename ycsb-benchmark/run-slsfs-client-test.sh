#!/bin/bash
source start-proxy-args.sh;

TESTNAME="${BACKEND_CONFIG_NAME}_testname+${CLIENT_TESTNAME}_policy+${POLICY_FILETOWORKER}+${POLICY_LAUNCH}+${POLICY_KEEPALIVE}_host+${#hosts[@]}_thread+${TOTAL_CLIENT}"
echo "testname: $TESTNAME"

export UPLOAD_GDRIVE=1fAcBZKe6ansCEEXjMR0L2ogVUrz11oPMHduwDDuOxB4

bash -c 'cd ../functions/datafunction; make function;' &
bash -c 'cd ../proxy; make from-docker; screen -S proxy2 -dm /home/ubuntu/slsfs/ycsb-benchmark/start-proxy.sh' &
bash -c "cd ../ssbd; ./start.sh ${BACKEND_BLOCKSIZE}" &
wait < <(jobs -p);

docker run --rm --entrypoint cat hare1039/transport:0.0.2 /bin/slsfs-client > /tmp/slsfs-client;

chmod +x /tmp/slsfs-client

for h in "${hosts[@]}"; do
    scp /tmp/slsfs-client "$h": &
done
wait < <(jobs -p);

echo wait until proxy open

while ! nc -z -v -w1 localhost 12001 2>&1 | grep -q succeeded; do
    echo 'waiting localhost:12001'
    sleep 1;
done

while curl https://localhost:10001/invokers -k 2>&1 | grep -q unhealthy; do
    echo 'waiting invoker restart'
    sleep 1;
done

echo starting;

for h in "${hosts[@]}"; do
    ssh "$h" "rm /tmp/$h-$TESTNAME*";
    ssh "$h" "bash -c '/home/ubuntu/slsfs-client --total-times ${EACH_CLIENT_ISSUE} --total-clients ${TOTAL_CLIENT} --bufsize $BUFSIZE --zipf-alpha 0.99 --result /tmp/$h-$TESTNAME --test-name $CLIENT_TESTNAME'" &
done
wait < <(jobs -p);

mkdir -p result-$TESTNAME;

for h in "${hosts[@]}"; do
    scp "$h:/tmp/$h-$TESTNAME*" result-$TESTNAME/
done
cp start-proxy-args.sh result-$TESTNAME/
cp /tmp/proxy-report.json result-$TESTNAME/
wait < <(jobs -p);

cd "result-$TESTNAME/"
rm -f ${TESTNAME}_summary.csv;
python3 ../csv-merge.py ${TESTNAME}_summary.csv *${TESTNAME}*.csv
../upload.sh $UPLOAD_GDRIVE $TESTNAME ${TESTNAME}_summary.csv
echo "finish test: $TESTNAME"
