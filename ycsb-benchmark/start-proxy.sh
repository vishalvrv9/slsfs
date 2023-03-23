
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
           --max-function-count       "$MAX_FUNCTION_COUNT" \
           --blocksize                "$BACKEND_BLOCKSIZE"
