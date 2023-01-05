#bash -c 'cd ../functions/datafunction; make function' &
bash -c 'cd ../proxy; make from-docker' &
wait < <(jobs -p);

screen -S proxy2 -dm /home/ubuntu/slsfs/ycsb-benchmark/start-proxy.sh
docker run --rm --entrypoint cat hare1039/transport:0.0.2 /bin/slsfs-client > /tmp/slsfs-client;

chmod +x /tmp/slsfs-client

hosts=(ow-invoker-1 ow-invoker-2 ow-invoker-3 ow-invoker-4 ow-invoker-5 ow-invoker-6 ow-invoker-7 ow-invoker-8 ow-invoker-9 ow-invoker-10 ow-invoker-11 ow-invoker-12 ow-invoker-13 ow-invoker-14 ow-invoker-15 ow-invoker-16);
hosts=(ow-invoker-1 ow-invoker-2 ow-invoker-3 ow-invoker-4 ow-invoker-5 ow-invoker-6 ow-invoker-7 ow-invoker-8);
#hosts=(ow-invoker-1 ow-invoker-2 ow-invoker-3 ow-invoker-4);
#hosts=(ow-invoker-1 ow-invoker-2);
#hosts=(ow-invoker-1);

EACH_CLIENT_ISSUE=2000
TOTAL_CLIENT=8
BUFSIZE=4092
CLIENT_TESTNAME=95-5

TESTNAME="ssbd-stripe_random+adaptive-max-load+const-time_testname+${CLIENT_TESTNAME}_host+${#hosts[@]}_thread+${TOTAL_CLIENT}"
echo "testname: $TESTNAME"

for h in "${hosts[@]}"; do
    scp /tmp/slsfs-client "$h": &
done
wait < <(jobs -p);

echo wait until proxy open

while ! nc -z -v -w1 localhost 12001 2>&1 | grep -q succeeded; do
    echo 'waiting localhost:12001'
    sleep 1;
done

echo starting;

for h in "${hosts[@]}"; do
    ssh "$h" "rm /tmp/$h-$TESTNAME*";
    ssh "$h" "bash -c '/home/ubuntu/slsfs-client --total-times ${EACH_CLIENT_ISSUE} --total-clients ${TOTAL_CLIENT} --bufsize $BUFSIZE --result /tmp/$h-$TESTNAME --test-name $CLIENT_TESTNAME'" &
done
wait < <(jobs -p);

mkdir -p result-$TESTNAME;

for h in "${hosts[@]}"; do
    scp "$h:/tmp/$h-$TESTNAME*" result-$TESTNAME/
done
cp /tmp/proxy-report.json result-$TESTNAME/
wait < <(jobs -p);

cd "result-$TESTNAME/"
python3 ../csv-merge.py ${TESTNAME}_summary.csv *${TESTNAME}*.csv
../upload.sh $TESTNAME ${TESTNAME}_summary.csv

#echo "finish test $TESTNAME"
#echo "use ./upload.sh $TESTNAME result-$TESTNAME/ to upload"
