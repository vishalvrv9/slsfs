#!/bin/bash
source start-proxy-args.sh;

TESTNAME="${BACKEND_CONFIG_NAME}_T+${CLIENT_TESTNAME}_P+${POLICY_FILETOWORKER}+${POLICY_LAUNCH}+${POLICY_KEEPALIVE}_H+${#hosts[@]}_TH+${TOTAL_CLIENT}"
echo "testname: $TESTNAME"

export UPLOAD_GDRIVE=1ird5QgDFKn2n1J6fdWh8hWJUXCwGm0NkG7BO2rimtis

bash -c 'cd ../functions/datafunction; make function;' &
bash -c 'cd ../proxy; make from-docker;' &
bash -c "cd ../ssbd; ./start.sh ${BACKEND_BLOCKSIZE}" &
wait < <(jobs -p);

screen -S proxy2 -dm /home/ubuntu/slsfs/ycsb-benchmark/start-proxy.sh

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
    ssh "$h" "rm -f /tmp/$h-$TESTNAME*";
    ssh "$h" "bash -c '/home/ubuntu/slsfs-client --total-times ${EACH_CLIENT_ISSUE} --total-clients ${TOTAL_CLIENT} --bufsize $BUFSIZE --zipf-alpha 1.2 --result /tmp/$h-$TESTNAME --test-name $CLIENT_TESTNAME'" &
done
wait < <(jobs -p);

mkdir -p $TESTNAME-result;

for h in "${hosts[@]}"; do
    scp "$h:/tmp/$h-$TESTNAME*" $TESTNAME-result/
done
cp start-proxy-args.sh $TESTNAME-result/
cp /tmp/proxy-report.json $TESTNAME-result/
wait < <(jobs -p);

cd "$TESTNAME-result/"
rm -f ${TESTNAME}_summary.csv ${TESTNAME}_summary_for_upload.csv;
python3 ../csv-merge.py ${TESTNAME}_summary.csv *${TESTNAME}*.csv
head -n 1200 ${TESTNAME}_summary.csv > ${TESTNAME}_summary_for_upload.csv
../upload.sh $UPLOAD_GDRIVE $TESTNAME ${TESTNAME}_summary_for_upload.csv
echo "finish test: $TESTNAME"
