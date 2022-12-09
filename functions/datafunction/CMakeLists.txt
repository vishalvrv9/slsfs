
cmake_minimum_required(VERSION 3.5.1)
#avoid cmake adds -Wl,-search_paths_first that makes mac compiled binary segmentation fault
set(HAVE_FLAG_SEARCH_PATHS_FIRST 0)

project(exec)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "-Wall -DAS_ACTIONLOOP -DBOOST_THREAD_PROVIDES_EXECUTORS -DBOOST_THREAD_USES_MOVE -DZKPP_FUTURE_USE_BOOST")
set(CMAKE_CXX_FLAGS_DEBUG "-ggdb -Wextra -Werror -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
set(CMAKE_EXE_LINKER_FLAGS "-static -static-libgcc -static-libstdc++")

set(CMAKE_CXX_STANDARD 20)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)

set(Boost_INSTALL_DIR ${CONAN_BOOST_ROOT})

conan_basic_setup()

if ("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(CROSS_LINKER_FLAGS "")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
endif()

include_directories(/action/libslsfs)
include_directories(/action/libslsfs/slsfs)
include_directories(/minio-cpp-build/vcpkg_installed/x64-linux/include)

link_directories(/minio-cpp-build/vcpkg_installed/x64-linux/lib)

add_executable(exec entry.cpp)
add_executable(test_swift test_swift.cpp)

set(CMAKE_PCH_INSTANTIATE_TEMPLATES ON)
target_precompile_headers(exec PRIVATE <boost/asio.hpp>)
target_precompile_headers(exec PRIVATE <fmt/core.h>)
target_precompile_headers(exec PRIVATE <oneapi/tbb/concurrent_hash_map.h>)

#set(CUSTOM_LIBRARIES "-ltbb -ltbbmalloc -laws-cpp-sdk-s3 -laws-cpp-sdk-core -laws-crt-cpp -laws-c-mqtt -laws-c-s3 -laws-c-auth -laws-c-event-stream -laws-checksums -laws-c-http -laws-c-io -laws-c-compression -laws-c-cal -laws-c-sdkutils -ls2n -laws-c-common")
set(CUSTOM_LIBRARIES "-ltbb -ltbbmalloc -lminiocpp -lcurlpp -lpugixml -linih")

target_link_libraries(exec       ${CUSTOM_LIBRARIES} ${CROSS_LINKER_FLAGS} ${CONAN_LIBS})
target_link_libraries(test_swift ${CUSTOM_LIBRARIES} ${CROSS_LINKER_FLAGS} ${CONAN_LIBS})
