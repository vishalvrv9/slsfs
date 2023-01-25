
IP=$(ip addr | grep 'inet ' | grep ens3 | awk '{ print $2 }');
IP="${IP%???}"

source start-proxy-args.sh;
docker rm -f proxy2;
docker run --privileged -d \
       --rm \
       --name=proxy2 \
       --net=host \
       --volume=/tmp:/tmp \
       hare1039/transport:0.0.2 \
           --listen 12001 \
           --announce $IP \
           $VERBOSE \
           --report /tmp/proxy-report.json \
           --initint                  "$INITINT" \
           --policy-filetoworker      "$POLICY_FILETOWORKER" \
           --policy-filetoworker-args "$POLICY_FILETOWORKER_ARGS" \
           --policy-launch            "$POLICY_LAUNCH" \
           --policy-launch-args       "$POLICY_LAUNCH_ARGS" \
           --policy-keepalive         "$POLICY_KEEPALIVE" \
           --policy-keepalive-args    "$POLICY_KEEPALIVE_ARGS" \
           --worker-config            "$BACKEND_CONFIG" \
           --blocksize                "$BACKEND_BLOCKSIZE"
#docker logs -f proxy2;
exit 0;

source start-proxy-args.sh;
docker rm -f proxy2;
docker run --privileged -it \
       --rm \
       --name=proxy2 \
       --net=host \
       --volume=/tmp:/tmp \
       --entrypoint gdb \
       hare1039/transport:0.0.2 \
           -ex=r \
           --args /bin/run \
           --listen 12001 \
           --announce $IP \
           --report /tmp/proxy-report.json \
           --initint                  "$INITINT" \
           --policy-filetoworker      "$POLICY_FILETOWORKER" \
           --policy-filetoworker-args "$POLICY_FILETOWORKER_ARGS" \
           --policy-launch            "$POLICY_LAUNCH" \
           --policy-launch-args       "$POLICY_LAUNCH_ARGS" \
           --policy-keepalive         "$POLICY_KEEPALIVE" \
           --policy-keepalive-args    "$POLICY_KEEPALIVE_ARGS" \
           --worker-config            "$BACKEND_CONFIG" \
           --blocksize                "$BACKEND_BLOCKSIZE"
docker logs -f proxy2;
exit 0;


# clear;
# docker stop proxy2;
# docker run --privileged -it \
#       --rm \
#       --name=proxy2 \
#       --net=host \
#       --entrypoint gdb \
#       hare1039/transport:0.0.2 \
#           -ex=r \
#           --args /bin/run \
#           --listen 12001 \
#           --announce 192.168.0.224 \
#           --init \
#           --policy-filetoworker lowest-load \
#           --policy-launch const-limit-launch \
#           --policy-launch-args 50 \
#           --policy-keepalive moving-interval \
#           --policy-keepalive-args $((60 * 1000)) \
#           --worker-config /backend/ssbd-basic.json

# exit 0;

clear;
docker stop proxy2;
docker run --privileged -it \
      --rm \
      --name=proxy2 \
      --volume=/tmp:/tmp \
      --net=host \
      hare1039/transport:0.0.2 \
          --listen 12001 \
          --announce 192.168.0.224 \
          --init \
          --report /tmp/proxy-report.json \
          --policy-filetoworker active-load-balance \
          --policy-launch const-average-load \
          --policy-launch-args "1:1" \
          --policy-keepalive const-time \
          --policy-keepalive-args "60000" \
          --worker-config /backend/ssbd-stripe.json

exit 0;
        #   --policy-keepalive-args "20:10000:10:50" \

# clear;
# docker stop proxy2;
# docker run --privileged -it \
#        --rm \
#        --name=proxy2 \
#        --net=host \
#        --entrypoint gdb \
#        hare1039/transport:0.0.2 \
#            -ex=r \
#            --args /bin/run \
#            --listen 12001 \
#            --thread 16 \
#            --announce 192.168.0.224 \
#            --init \
#            --policy-filetoworker lowest-load \
#            --policy-launch const-limit-launch \
#            --policy-launch-args 50 \
#            --policy-keepalive const-time \
#            --policy-keepalive-args $((60 * 1000)) \
#            --worker-config /backend/cassandra-repl1.json

# #ssbd-basic.json

# exit 0;
