cmake_minimum_required(VERSION 3.5.1)
#avoid cmake adds -Wl,-search_paths_first that makes mac compiled binary segmentation fault
set(HAVE_FLAG_SEARCH_PATHS_FIRST 0)

project(run)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "-Wall -Wno-stringop-overflow -Wno-restrict -DBOOST_THREAD_PROVIDES_EXECUTORS -DBOOST_THREAD_USES_MOVE -DZKPP_FUTURE_USE_BOOST")
set(CMAKE_CXX_FLAGS_DEBUG "-ggdb -Wextra -Werror")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

set(CMAKE_CXX_STANDARD 20)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)

set(Boost_INSTALL_DIR ${CONAN_BOOST_ROOT})

conan_basic_setup()

#set(CROSS_LINKER_FLAGS "-static -static-libgcc -static-libstdc++")
set(CROSS_LINKER_FLAGS "-lzkpp -lzookeeper_mt")


include_directories("${PROJECT_SOURCE_DIR}/zookeeper-cpp/src")

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(CROSS_LINKER_FLAGS "")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
endif()

add_executable(slsfs-proxy              main.cpp)
add_executable(slsfs-client             clients/slsfs-client.cpp)
add_executable(slsfs-perfunction-client clients/slsfs-perfunction-client.cpp)
add_executable(datafunction-profile     clients/datafunction-profile.cpp)
add_executable(ceph-zipf                clients/ceph-zipf.cpp)
add_executable(slsfs-cmd                clients/slsfs-cmd.cpp)
add_executable(slsfs-client-dynamic     clients/slsfs-client-dynamic.cpp)
add_executable(slsfs-client-ddf         clients/slsfs-client-ddf.cpp)

set(CMAKE_PCH_INSTANTIATE_TEMPLATES ON)

target_precompile_headers(slsfs-proxy PRIVATE <boost/asio.hpp>)
target_precompile_headers(slsfs-proxy PRIVATE <fmt/core.h>)
target_precompile_headers(slsfs-proxy PRIVATE <nlohmann/json.hpp>)
target_precompile_headers(slsfs-proxy PRIVATE <boost/beast/core.hpp>)
target_precompile_headers(slsfs-proxy PRIVATE <oneapi/tbb/concurrent_queue.h>)
target_precompile_headers(slsfs-proxy PRIVATE <oneapi/tbb/concurrent_hash_map.h>)
target_precompile_headers(slsfs-proxy PRIVATE <oneapi/tbb/concurrent_vector.h>)


target_precompile_headers(slsfs-client         REUSE_FROM slsfs-proxy)
target_precompile_headers(ceph-zipf            REUSE_FROM slsfs-proxy)
target_precompile_headers(slsfs-cmd            REUSE_FROM slsfs-proxy)
target_precompile_headers(slsfs-client-dynamic REUSE_FROM slsfs-proxy)
target_precompile_headers(slsfs-client-ddf     REUSE_FROM slsfs-proxy)


target_link_libraries(slsfs-proxy  -static-libstdc++ -static-libgcc ${CROSS_LINKER_FLAGS} ${CONAN_LIBS})
target_link_libraries(slsfs-client -static-libstdc++ -static-libgcc ${CROSS_LINKER_FLAGS} ${CONAN_LIBS})
target_link_libraries(slsfs-perfunction-client -static-libstdc++ -static-libgcc ${CROSS_LINKER_FLAGS} ${CONAN_LIBS})
target_link_libraries(datafunction-profile -static-libstdc++ -static-libgcc ${CROSS_LINKER_FLAGS} ${CONAN_LIBS})
target_link_libraries(ceph-zipf    -static-libstdc++ -static-libgcc ${CROSS_LINKER_FLAGS} ${CONAN_LIBS})
target_link_libraries(slsfs-cmd    -static-libstdc++ -static-libgcc ${CROSS_LINKER_FLAGS} ${CONAN_LIBS})
target_link_libraries(slsfs-client-dynamic -static-libstdc++ -static-libgcc ${CROSS_LINKER_FLAGS} ${CONAN_LIBS})
target_link_libraries(slsfs-client-ddf -static-libstdc++ -static-libgcc ${CROSS_LINKER_FLAGS} ${CONAN_LIBS})
