#!/bin/bash

for qsize in {1..1}; do
    for qtest in {1..5}; do
        cat start-proxy-args-template.sh | \
            sed "s/QSIZE_ARGS/$qsize/" |
            sed "s/QTEST_ARGS/$qtest/" \
                > start-proxy-args.sh;
        ./run-slsfs-client-test.sh
    done
done

echo -e "\a finish all test"
