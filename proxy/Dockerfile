FROM gcc:12 AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends python3-pip make cmake ninja-build ccache git wget libzookeeper-mt-dev clang && \
    pip3 install conan==1.59 --break-system-packages

RUN conan profile new default --detect &&\
    conan profile update settings.compiler.libcxx=libstdc++11 default

ADD profiles /pre/profiles
ADD conanfile.txt /pre

RUN --mount=type=cache,target=/ccache \
    mkdir /pre/build && cd /pre/build && \
    conan install .. --profile ../profiles/release-native --build missing && \
    conan install .. --profile ../profiles/debug --build missing

ADD zookeeper-cpp /zookeeper-cpp

RUN mkdir /zookeeper-cpp/build && cd /zookeeper-cpp/build && \
    conan install .. --profile /pre/profiles/release-native --build missing && \
    cmake .. -G Ninja\
             -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_CXX_FLAGS='-std=c++17 -include "utility" -DZKPP_FUTURE_USE_BOOST=1' && \
    cmake --build . && ninja install

ADD . /final

ARG debug

RUN --mount=type=cache,target=/final/build \
    cd /final && \
    bash -c 'if [[ -z "$debug" ]]; then make release; else make debug; fi' && \
    cp /final/build/bin/* /bin && \
    chmod +x /bin/slsfs-proxy

#FROM busybox:glibc
FROM ubuntu:23.04

RUN apt-get update && apt-get install -y --no-install-recommends gdb

ADD backend /backend

COPY --from=builder /usr/lib/x86_64-linux-gnu/libzookeeper_mt.so.2 /lib/libzookeeper_mt.so.2
COPY --from=builder /lib/x86_64-linux-gnu/librt.so.1 /lib/librt.so.1
COPY --from=builder /lib/x86_64-linux-gnu/libdl.so.2 /lib/libdl.so.2

COPY --from=builder /bin/slsfs-proxy  /bin/slsfs-proxy
COPY --from=builder /bin/slsfs-client /bin/slsfs-client
COPY --from=builder /bin/slsfs-perfunction-client /bin/slsfs-perfunction-client
COPY --from=builder /bin/ceph-zipf    /bin/ceph-zipf
COPY --from=builder /bin/datafunction-profile /bin/datafunction-profile
COPY --from=builder /bin/slsfs-cmd    /bin/slsfs-cmd
COPY --from=builder /bin/slsfs-client-dynamic /bin/slsfs-client-dynamic
COPY --from=builder /bin/slsfs-client-ddf /bin/slsfs-client-ddf

ENTRYPOINT ["/bin/slsfs-proxy"]
