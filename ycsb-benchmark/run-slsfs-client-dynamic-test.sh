#!/bin/bash
source start-proxy-args.sh;

TESTNAME="${MEMO}_${BACKEND_CONFIG_NAME}_T+${CLIENT_TESTNAME}_H+${#hosts[@]}_TH+${TOTAL_CLIENT}"
echo "testname: $TESTNAME"

for h in "${hosts[@]}"; do
    ssh "$h" "bash -c 'pkill -f /tmp/slsfs-client-dynamic'" &
done

ssh proxy-1 docker rm -f proxy2&
ssh proxy-2 docker rm -f proxy2&
ssh proxy-3 docker rm -f proxy2&
ssh zookeeper-1 docker rm -f proxy2&
ssh zookeeper-2 docker rm -f proxy2&
ssh zookeeper-3 docker rm -f proxy2&
wait < <(jobs -p);

bash -c 'cd ../functions/datafunction; make function;' &
bash -c 'source start-proxy-args.sh; cd ../proxy; make debug-from-docker; ./transfer_images.sh; cd -; start-proxy-remote proxy-1;' &
bash -c "cd ../ssbd;  make from-docker; ./transfer_images.sh; ./cleanup.sh; ./start.sh ${BACKEND_BLOCKSIZE}" &

bash -c "cd ../ssbd; ./cleanup.sh; ./start.sh ${BACKEND_BLOCKSIZE}" &

wait < <(jobs -p);

hostparis=("proxy-1:12001")
#           "proxy-2:12001"
#           "proxy-3:12001"
#           "zookeeper-1:12001"
#           "zookeeper-2:12001"
#           "zookeeper-3:12001");

rm -f /tmp/slsfs-client-dynamic;
docker run --rm --entrypoint cat hare1039/transport:0.0.2 /bin/slsfs-client-dynamic > /tmp/slsfs-client-dynamic;
chmod +x /tmp/slsfs-client-dynamic

for h in "${hosts[@]}"; do
    ssh $h rm -f /tmp/slsfs-client-dynamic;
    scp /tmp/slsfs-client-dynamic "$h":/tmp/slsfs-client-dynamic;
done
wait < <(jobs -p);

while curl https://localhost:10001/invokers -k 2>&1 | grep -q unhealthy; do
    echo 'waiting invoker restart'
    sleep 1;
done

echo wait until proxy open

for pair in "${hostparis[@]}"; do
    p=(`echo $pair | tr ':' ' '`)
    while ! nc -z -v -w1 ${p[0]} ${p[1]} 2>&1 | grep -q succeeded; do
        echo "waiting $pair";
        sleep 1;
    done
done

echo starting;

for h in "${hosts[@]}"; do
    ssh "$h" "rm -f /tmp/$h-$TESTNAME*";
    ssh "$h" -t "bash -c 'ulimit -n 1048576; gdb -ex run --args /tmp/slsfs-client-dynamic --total-times ${EACH_CLIENT_ISSUE} --total-clients ${TOTAL_CLIENT} --total-duration ${TOTAL_TIME_AVAILABLE} --bufsize $BUFSIZE --zipf-alpha 1.2 ${UNIFORM_DIST} --result /tmp/$h-$TESTNAME --test-name $CLIENT_TESTNAME'" ;
done


bash -c 'sleep 10; source start-proxy-args.sh; start-proxy-remote proxy-2 noinit;' &
bash -c 'sleep 20; source start-proxy-args.sh; start-proxy-remote proxy-3 noinit;' &
bash -c 'sleep 30; source start-proxy-args.sh; start-proxy-remote zookeeper-1 noinit;' &
bash -c 'sleep 40; source start-proxy-args.sh; start-proxy-remote zookeeper-2 noinit;' &
bash -c 'sleep 50; source start-proxy-args.sh; start-proxy-remote zookeeper-3 noinit;' &

bash -c 'sleep 70;  ssh zookeeper-3 docker stop proxy2;' &
bash -c 'sleep 80;  ssh zookeeper-2 docker stop proxy2;' &
bash -c 'sleep 90;  ssh zookeeper-1 docker stop proxy2;' &
bash -c 'sleep 100; ssh proxy-3     docker stop proxy2;' &
bash -c 'sleep 110; ssh proxy-2     docker stop proxy2;' &

wait < <(jobs -p);

rm -rf $TESTNAME-result/;
mkdir -p $TESTNAME-result;

cd "$TESTNAME-result/";

for h in "${hosts[@]}"; do
    scp "$h:/tmp/$h-$TESTNAME*" .
done
cp ../start-proxy-args.sh .
scp proxy-1:/tmp/proxy-report.json proxy-report-1.json
scp proxy-2:/tmp/proxy-report.json proxy-report-2.json
scp proxy-3:/tmp/proxy-report.json proxy-report-3.json
scp zookeeper-1:/tmp/proxy-report.json proxy-report-4.json
scp zookeeper-2:/tmp/proxy-report.json proxy-report-5.json
scp zookeeper-3:/tmp/proxy-report.json proxy-report-6.json
wait < <(jobs -p);

rm -f ${TESTNAME}_summary.csv ${TESTNAME}_summary_for_upload.csv;
python3 ../csv-merge.py ${TESTNAME}_summary.csv *${TESTNAME}*.csv
cp ${TESTNAME}_summary.csv ${TESTNAME}_summary_original.csv
head -n 1200 ${TESTNAME}_summary.csv > ${TESTNAME}_summary_for_upload.csv
../upload.sh $UPLOAD_GDRIVE $TESTNAME ${TESTNAME}_summary_for_upload.csv
echo -e "finish test: $TESTNAME"
