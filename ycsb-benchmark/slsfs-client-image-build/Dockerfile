FROM ubuntu:23.04

ADD libzookeeper_mt.so.2 /lib/libzookeeper_mt.so.2
ADD librt.so.1 /lib/librt.so.1
ADD slsfs-client-dynamic /bin/slsfs-client-dynamic
RUN chmod +x /bin/slsfs-client-dynamic

ENTRYPOINT ["/bin/slsfs-client-dynamic"]