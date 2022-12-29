clear;
docker stop proxy2;
docker run --privileged -it \
       --rm \
       --name=proxy2 \
       --net=host \
       hare1039/transport:0.0.2 \
           --listen 12001 \
           --announce 192.168.0.224 \
           --init \
           --policy-filetoworker lowest-load \
           --policy-launch const-limit-launch \
           --policy-launch-args 50 \
           --policy-keepalive const-time \
           --policy-keepalive-args $((60 * 1000)) \
           --worker-config /backend/ssbd-basic.json
#           --worker-config "{{
#    "type": "wakeup",
#    "proxyhost": "192.168.0.224",
#    "proxyport": "12001",
#    "storagetype": "ssbd-stripe",
#    "storageconfig": {{
#        "hosts": [
#            {{"host": "192.168.0.94",  "port": "12000"}},
#            {{"host": "192.168.0.165", "port": "12000"}},
#            {{"host": "192.168.0.242", "port": "12000"}},
#            {{"host": "192.168.0.183", "port": "12000"}},
#            {{"host": "192.168.0.86",  "port": "12000"}},
#            {{"host": "192.168.0.207", "port": "12000"}},
#            {{"host": "192.168.0.143", "port": "12000"}},
#            {{"host": "192.168.0.184", "port": "12000"}},
#            {{"host": "192.168.0.8",   "port": "12000"}}
#        ],
#        "replication_size": 3
#    }}
#}}"

exit 0;
#
#docker stop proxy2;
#docker run --privileged -it \
#       --rm \
#       --name=proxy2 \
#       --net=host \
#       --entrypoint=gdb \
#       hare1039/transport:0.0.2 \
#           -ex=r \
#           --args /bin/run \
#           --listen 12001 \
#           --announce 192.168.0.224 \
#           --init \
#           --policy-filetoworker lowest-load \
#           --policy-launch const-limit-launch \
#           --policy-launch-args 50 \
#           --policy-keepalive const-time \
#           --policy-keepalive-args $((60 * 1000))
