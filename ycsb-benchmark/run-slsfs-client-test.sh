#!/bin/bash
source start-proxy-args.sh;

TESTNAME="CRETRUEV${BACKEND_CONFIG_NAME}_T+${CLIENT_TESTNAME}_P+skip_H+${#hosts[@]}_TH+${TOTAL_CLIENT}_2"
echo "testname: $TESTNAME"

ssh proxy-1 docker rm -f proxy2&
ssh proxy-2 docker rm -f proxy2&
ssh proxy-3 docker rm -f proxy2&

bash -c 'cd ../functions/datafunction; make function;' &
bash -c 'cd ../proxy; make from-docker; ./transfer_images.sh' &
bash -c "cd ../ssbd; ./transfer_images.sh; ./start.sh ${BACKEND_BLOCKSIZE}" &
wait < <(jobs -p);

start-proxy-remote()
{
    local h=$1;
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
start-proxy-remote proxy-2 noinit
start-proxy-remote proxy-3 noinit
#./start-proxy.sh

rm -f /tmp/slsfs-client;
docker run --rm --entrypoint cat hare1039/transport:0.0.2 /bin/slsfs-client > /tmp/slsfs-client;

chmod +x /tmp/slsfs-client

for h in "${hosts[@]}"; do
    ssh $h rm -f /tmp/slsfs-client || exit 0;
    scp /tmp/slsfs-client "$h": &
done
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

while ! nc -z -v -w1 192.168.0.215 12001 2>&1 | grep -q succeeded; do
    echo 'waiting proxy1 192.168.0.215:12001'
    sleep 1;
done

while ! nc -z -v -w1 192.168.0.149 12001 2>&1 | grep -q succeeded; do
    echo 'waiting proxy1 192.168.0.149:12001'
    sleep 1;
done

echo starting;

for h in "${hosts[@]}"; do
    ssh "$h" "rm -f /tmp/$h-$TESTNAME*";
    ssh "$h" "bash -c '/home/ubuntu/slsfs-client --total-times ${EACH_CLIENT_ISSUE} --total-clients ${TOTAL_CLIENT} --bufsize $BUFSIZE --zipf-alpha 1.05 ${UNIFORM_DIST} --result /tmp/$h-$TESTNAME --test-name $CLIENT_TESTNAME'" &
done
wait < <(jobs -p);

mkdir -p $TESTNAME-result;

for h in "${hosts[@]}"; do
    scp "$h:/tmp/$h-$TESTNAME*" $TESTNAME-result/
done
cp start-proxy-args.sh $TESTNAME-result/
scp proxy-1:/tmp/proxy-report.json $TESTNAME-result/proxy-report-1.json
scp proxy-2:/tmp/proxy-report.json $TESTNAME-result/proxy-report-2.json
scp proxy-3:/tmp/proxy-report.json $TESTNAME-result/proxy-report-3.json
wait < <(jobs -p);

cd "$TESTNAME-result/"
rm -f ${TESTNAME}_summary.csv ${TESTNAME}_summary_for_upload.csv;
python3 ../csv-merge.py ${TESTNAME}_summary.csv *${TESTNAME}*.csv
head -n 1200 ${TESTNAME}_summary.csv > ${TESTNAME}_summary_for_upload.csv
../upload.sh $UPLOAD_GDRIVE $TESTNAME ${TESTNAME}_summary_for_upload.csv
echo -e "\a finish test: $TESTNAME"
