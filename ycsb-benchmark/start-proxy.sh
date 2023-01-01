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

exit 0;

clear;
docker stop proxy2;
docker run --privileged -it \
       --rm \
       --name=proxy2 \
       --net=host \
       --entrypoint gdb \
       --privileged \
       hare1039/transport:0.0.2 \
           -ex=r \
           --args /bin/run \
           --listen 12001 \
           --announce 192.168.0.224 \
           --init \
           --policy-filetoworker lowest-load \
           --policy-launch const-limit-launch \
           --policy-launch-args 50 \
           --policy-keepalive const-time \
           --policy-keepalive-args $((60 * 1000)) \
           --worker-config /backend/ssbd-basic.json

exit 0;
