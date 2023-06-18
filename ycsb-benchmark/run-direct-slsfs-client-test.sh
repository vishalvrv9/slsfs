#!/bin/bash
source start-proxy-args.sh;

TESTNAME="${MEMO}_${BACKEND_CONFIG_NAME}_T+${CLIENT_TESTNAME}_H+${#hosts[@]}_TH+${TOTAL_CLIENT}"
echo "testname: $TESTNAME"

bash -c 'cd ../functions/datafunction; make function;' &
bash -c 'cd ../proxy; make from-docker; ./transfer_images.sh' &
bash -c "cd ../ssbd;  make from-docker; ./transfer_images.sh; ./start.sh ${BACKEND_BLOCKSIZE}" &
wait < <(jobs -p);

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

echo starting

for h in "${hosts[@]}"; do
    ssh "$h" "rm -f /tmp/$h-$TESTNAME*";
    ssh "$h" "bash -c 'ulimit -n 8192; /tmp/slsfs-client-dynamic --total-times ${EACH_CLIENT_ISSUE} --total-clients ${TOTAL_CLIENT} --bufsize $BUFSIZE --zipf-alpha 1.05 ${UNIFORM_DIST} --result /tmp/$h-$TESTNAME --test-name $CLIENT_TESTNAME'" &
done
wait < <(jobs -p);

mkdir -p $TESTNAME-result;

for h in "${hosts[@]}"; do
    scp "$h:/tmp/$h-$TESTNAME*" $TESTNAME-result/
done
cp start-proxy-args.sh $TESTNAME-result/

cd "$TESTNAME-result/"
rm -f ${TESTNAME}_summary.csv ${TESTNAME}_summary_for_upload.csv;
echo '{"df":[],"started_df":0,"total_duration":1}' > proxy-report-1.json;
echo '{"df":[],"started_df":0,"total_duration":1}' > proxy-report-2.json;
echo '{"df":[],"started_df":0,"total_duration":1}' > proxy-report-3.json;
python3 ../csv-merge.py ${TESTNAME}_summary.csv *${TESTNAME}*.csv
head -n 1200 ${TESTNAME}_summary.csv > ${TESTNAME}_summary_for_upload.csv
../upload.sh $UPLOAD_GDRIVE $TESTNAME ${TESTNAME}_summary_for_upload.csv
echo -e "\a finish test: $TESTNAME"
