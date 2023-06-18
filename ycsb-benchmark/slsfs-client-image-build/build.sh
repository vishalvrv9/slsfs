rm -f slsfs-client-dynamic;
docker run --rm --entrypoint cat hare1039/transport:0.0.2 /bin/slsfs-client-dynamic > slsfs-client-dynamic;
docker run --rm --entrypoint cat hare1039/transport:0.0.2 /lib/libzookeeper_mt.so.2 > libzookeeper_mt.so.2;
docker run --rm --entrypoint cat hare1039/transport:0.0.2 /lib/librt.so.1           > librt.so.1;
chmod +x slsfs-client-dynamic

docker build -t hare1039/slsfs-client:0.0.2 .
