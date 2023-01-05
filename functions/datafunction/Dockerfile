FROM alpine:3.16 AS builder

RUN apk update && \
    apk add --no-cache \
        py3-pip cmake make ccache git wget bash g++ gcc linux-headers perl pkgconf \
        zlib-dev openssl-dev zstd-dev build-base ninja zip unzip curl-dev curl inih-dev jq && \
    pip install conan

RUN git clone --branch 2022.11.14 https://github.com/Microsoft/vcpkg.git /vcpkg && \
    bash -c 'export VCPKG_FORCE_SYSTEM_BINARIES=1; /vcpkg/bootstrap-vcpkg.sh -disableMetrics'

RUN git clone https://github.com/minio/minio-cpp /minio-cpp && \
    mkdir /minio-cpp-build && \
    cat /minio-cpp/vcpkg.json |\
        jq '.overrides = [{ name: "openssl", "version-string": "1.1.1n" }]' |\
        jq '."builtin-baseline" = "09adfdc8cdad76345b7cc7f3305899e1cbd66297"' > m.json && \
    mv m.json /minio-cpp/vcpkg.json && \
    bash -c 'export VCPKG_FORCE_SYSTEM_BINARIES=1; cmake -B /minio-cpp-build -S /minio-cpp -DCMAKE_TOOLCHAIN_FILE=/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_C_COMPILER=cc -DCMAKE_CXX_COMPILER=c++' && \
    bash -c 'export VCPKG_FORCE_SYSTEM_BINARIES=1; cd /minio-cpp-build && make && make install'

RUN git clone --branch v2021.7.0 https://github.com/oneapi-src/oneTBB.git && \
    mkdir -p /oneTBB/build && cd /oneTBB/build && \
    cmake -DTBB_TEST=OFF -DBUILD_SHARED_LIBS=OFF .. && \
    make && make install

RUN conan profile new default --detect &&\
    conan profile update settings.compiler.libcxx=libstdc++11 default

ADD datafunction/profiles      /pre/profiles
ADD datafunction/conanfile.txt /pre

RUN mkdir /pre/build && cd /pre/build && \
    conan install .. --profile ../profiles/release --build=missing --build=librdkafka --build=cassandra-cpp-driver --build=poco --build=openssl --build=m4 --build=libcurl --build=libuv && \
    conan install .. --profile ../profiles/debug   --build=missing --build=librdkafka --build=cassandra-cpp-driver --build=poco --build=openssl --build=m4 --build=libcurl --build=libuv

ADD libslsfs /action/libslsfs
ADD datafunction /action/

ARG debug
ARG actionloop

RUN --mount=type=cache,target=/action/build mkdir -p /action/build && \
    cd /action/build && \
    bash -c 'if [[ -z "$debug" ]]; then conan install .. --profile ../profiles/release --build missing; else conan install .. --profile ../profiles/debug --build missing; fi' && \
    bash -c 'if [[ -z "$debug" ]]; then cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_AS_ACTIONLOOP=$actionloop; else cmake .. -DCMAKE_BUILD_TYPE=Debug -DBUILD_AS_ACTIONLOOP=$actionloop; fi' && \
    cmake --build . && \
    cp /action/build/bin/exec /action/
