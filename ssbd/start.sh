#!/bin/bash

hosts=(ssbd-1 ssbd-2 ssbd-3 ssbd-4 ssbd-5 ssbd-6 ssbd-7 ssbd-8 ssbd-9)

images=(hare1039/ssbd:0.0.1)

for i in "${images[@]}"; do
    for h in "${hosts[@]}"; do
        ssh "$h" "docker rm -f ${h}; docker run --name ${h} --net=host -d -v /tmp:/tmp hare1039/ssbd:0.0.1; echo start $h done" &
    done
    wait < <(jobs -p);
done
