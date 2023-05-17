#!/bin/bash

bash -c 'cd ../functions/datafunction; make function;' &
bash -c 'source start-proxy-args.sh; cd ../proxy; make from-docker; ./transfer_images.sh;' &
bash -c "cd ../ssbd;  make from-docker; ./transfer_images.sh" &

wait;

for qsize in {3..3}; do
    for qtest in {4..4}; do
        cat start-proxy-args-template.sh | \
            sed "s/QSIZE_ARGS/$qsize/" |
            sed "s/QTEST_ARGS/$qtest/" \
                > start-proxy-args.sh;
        ./run-slsfs-client-test.sh
    done
done

echo -e "\a finish all test"
