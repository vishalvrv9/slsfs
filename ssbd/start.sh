#!/bin/bash

hosts1=(ssbd-1 ssbd-2 ssbd-3 ssbd-4 ssbd-5 ssbd-6 ssbd-7 ssbd-8 ssbd-9)
#hosts2=(ssbd-4 ssbd-5 ssbd-6 ssbd-7 ssbd-8 ssbd-9)

images=(hare1039/ssbd:0.0.1)

BLOCKSIZE=$1;
if [[ "$BLOCKSIZE" == "" ]]; then
    BLOCKSIZE=4096;
fi

for i in "${images[@]}"; do
    for h in "${hosts1[@]}"; do
        ssh debian@"$h" "docker rm -f ${h}-1; docker run --name ${h}-1 --net=host -d -v /tmp:/tmp hare1039/ssbd:0.0.1 --listen 12000 --blocksize ${BLOCKSIZE} --db /tmp/haressbd/db1; echo start $h-1 done" &
        ssh debian@"$h" "docker rm -f ${h}-2; docker run --name ${h}-2 --net=host -d -v /tmp:/tmp hare1039/ssbd:0.0.1 --listen 12001 --blocksize ${BLOCKSIZE} --db /tmp/haressbd/db2; echo start $h-2 done" &
        ssh debian@"$h" "docker rm -f ${h}-3; docker run --name ${h}-3 --net=host -d -v /tmp:/tmp hare1039/ssbd:0.0.1 --listen 12002 --blocksize ${BLOCKSIZE} --db /tmp/haressbd/db3; echo start $h-3 done" &
    done
    wait < <(jobs -p);
done

#for i in "${images[@]}"; do
#    for h in "${hosts2[@]}"; do
#        ssh "$h" "docker rm -f ${h}; docker run --name ${h} --net=host -d -v /tmp:/tmp hare1039/ssbd:0.0.1 --blocksize ${BLOCKSIZE}; echo start $h done" &
#    done
#    wait < <(jobs -p);
#done
