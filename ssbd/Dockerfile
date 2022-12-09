FROM gcc:12 as builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends python3-pip make cmake ninja-build ccache && \
    pip3 install conan

RUN conan profile new default --detect &&\
    sed -i 's/compiler.libcxx=libstdc++/compiler.libcxx=libstdc++11/g' /root/.conan/profiles/default

ADD profiles /pre/profiles
ADD conanfile.txt /pre

RUN mkdir /pre/build && cd /pre/build && \
    conan install .. --profile ../profiles/release-native --build missing && \
    conan install .. --profile ../profiles/debug --build missing

ADD . /final

ARG debug

RUN --mount=type=cache,target=/ccache \
    cd /final && \
    CCACHE_DIR=/ccache bash -c 'echo debug=$debug; if [[ -z "$debug" ]]; then make release; else make debug; fi'

RUN strip /final/build/bin/client && strip /final/build/bin/run

FROM busybox:glibc

COPY --from=builder /final/build/bin/client /final/build/bin/client
COPY --from=builder /final/build/bin/run /final/build/bin/run

ENTRYPOINT ["/final/build/bin/run", "--listen", "12000"]
