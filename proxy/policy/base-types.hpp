#pragma once
#ifndef POLICY_BASE_TYPES_HPP__
#define POLICY_BASE_TYPES_HPP__

#include "../worker.hpp"
#include "../worker-config.hpp"
#include "../launcher-job.hpp"

#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/concurrent_hash_map.h>

#include <type_traits>
#include <concepts>

namespace slsfs::launcher
{

using worker_set = oneapi::tbb::concurrent_hash_map<df::worker_ptr, int /* not used */>;
using worker_set_accessor = worker_set::accessor;

using fileid_map =
    oneapi::tbb::concurrent_hash_map<
        pack::packet_header,
        df::worker_ptr,
        pack::packet_header_key_hash_compare>;

using fileid_to_worker_accessor = fileid_map::accessor;
using fileid_worker_pair        = fileid_map::value_type;

} // namespace slsfs::launcher

#endif // POLICY_BASE_TYPES_HPP__
