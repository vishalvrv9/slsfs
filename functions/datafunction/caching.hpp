#pragma once

#ifndef CACHING_HPP__
#define CACHING_HPP__

#include <oneapi/tbb/concurrent_hash_map.h>
#include <slsfs.hpp>
#include <optional>
#include <vector>

namespace slsfsdf::cache
{

/**
 * Represents a previously read or written part of a file.
*/
struct cached_data {
    std::uint32_t position, size;
};

using frequency_map =
    oneapi::tbb::concurrent_hash_map<slsfs::pack::key_t,
                                     int,
                                     slsfs::uuid::hash_compare<slsfs::pack::key_t>>;
using caching_map =
    oneapi::tbb::concurrent_hash_map<slsfs::pack::key_t,
                                     slsfs::base::buf,
                                     slsfs::uuid::hash_compare<slsfs::pack::key_t>>;

using file_log_map =
    oneapi::tbb::concurrent_hash_map<slsfs::pack::key_t,
                                     std::vector<cached_data>,
                                     slsfs::uuid::hash_compare<slsfs::pack::key_t>>;


/**
 * Cache approach #1:
 * On first request, we fetch the whole file and place it in the cache.
 * subsequent writes only modify part of the file that they need to modify
 * writes are applied to the cache once they are commited to disk
 * reads can be taken from the cache.
 *
 * Cache approach #2: (IMPLEMENTING)
 * We cache what the first request asks for based on LFU.
 * So we do not cache the entire file unless the request asks for it.
 *
 *  edge case #1:
 *  we read part of a file that is not cached
 *
*/
class cache
{
    frequency_map frequency_map_;
    caching_map   caching_map_;
    file_log_map  file_log_map_;

public:
    auto read_from_cache(slsfs::jsre::request_parser<slsfs::base::byte> const& input)
        -> std::optional<slsfs::base::buf>
    {
        frequency_map::accessor freq_it;
        if (frequency_map_.find(freq_it, input.uuid()))
            frequency_map_.emplace(input.uuid(), freq_it->second + 1);
        else
            frequency_map_.emplace(input.uuid(), 0);
        freq_it.release();


        caching_map::const_accessor acc;
        // Case 1: file is cached
        if (caching_map_.find(acc, input.uuid()))
        {
            file_log_map::const_accessor log_acc;
            file_log_map_.find(log_acc, input.uuid());

            /**
             * Looking through the log of read ranges to check if the buffer contains the part
             * of the file we need
             * OPITMIZATION: insert into vector such that vector is sorted by lenght of range
            **/
            for (auto log_pos : log_acc->second)
            {
                if ((log_pos.position >= input.position()) &&
                    (log_pos.position + log_pos.size) <= (input.size() + input.position())) {
                // Case 2: Cache hit
                    auto&& full_buf = acc->second;

                    slsfs::base::buf result(input.size());

                    std::copy_n(std::next(full_buf.begin(), input.position()), input.size(), result.begin());
                    return std::optional<slsfs::base::buf>(result);
                }
            }
        }
        // Case 3: CACHE MISS
        // Case 3a: cache miss because no buffer
        // Case 3b: cache miss because missing part of file
        return std::nullopt;
    }

    bool write_to_cache(slsfs::jsre::request_parser<slsfs::base::byte> const& input,
                        slsfs::base::buf const& data)
    {
        frequency_map::accessor freq_it;
        if (frequency_map_.find(freq_it, input.uuid()))
            frequency_map_.emplace(input.uuid(), freq_it->second + 1);
        else
            frequency_map_.emplace(input.uuid(), 0);
        freq_it.release();

        /**
         * Writing to cache
         * Case 1: parts of the file are cached
         * case 1a: update cached parts
         * case 1b: add new parts of the file
         * Case 2: no part of the file has been cached yet
         *
        */
        caching_map::accessor acc;
        if (caching_map_.find(acc, input.uuid()))
        {

        }
        else
        {
            file_log_map::accessor log_acc;
            if (file_log_map_.find(log_acc, input.uuid()))
            {

            }
            else
                file_log_map_.emplace(input.uuid(),
                                      std::vector<cached_data>{cached_data(input.position(), input.size())});

            caching_map_.emplace(input.uuid(), data);
            return true;
        }
        return false;
    }
};
} // namespace slsfsdf::cache

#endif // CACHING_HPP__
