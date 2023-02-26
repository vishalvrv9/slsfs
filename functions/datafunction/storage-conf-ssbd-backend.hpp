#pragma once
#ifndef STORAGE_CONF_SSBD_BACKEND_HPP__
#define STORAGE_CONF_SSBD_BACKEND_HPP__

#include "storage-conf.hpp"

#include <slsfs.hpp>

#include <oneapi/tbb/concurrent_vector.h>
#include <boost/coroutine2/all.hpp>
#include <boost/asio.hpp>

#include <vector>
#include <semaphore>

namespace slsfsdf
{

// Storage backend configuration for SSBD stripe
class storage_conf_ssbd_backend : public storage_conf
{
    boost::asio::io_context& io_context_;
    int replication_size_ = 3;

    std::vector<std::shared_ptr<slsfs::backend::ssbd>> backendlist_;

    void connect() override
    {
        for (std::shared_ptr<slsfs::backend::ssbd>& host : backendlist_)
            host->connect();
    }

    static
    auto static_engine() -> std::mt19937&
    {
        static thread_local std::mt19937 mt;
        return mt;
    }

    int select_replica(slsfs::pack::key_t const& uuid,
                       int const partition,
                       int const index)
    {
        std::seed_seq seeds {uuid.begin(), uuid.end()};
        static_engine().seed(seeds);

        std::uniform_int_distribution<> dist(0, backendlist_.size() - 1);
        static_engine().discard(partition * (partition * index));

        return dist(static_engine());
    }

    static
    auto version() -> std::uint32_t
    {
        std::uint64_t v = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return static_cast<std::uint32_t>(v >> 6);
    }

    // 2pc stuff
    void start_2pc_prepare (slsfs::jsre::request_parser<slsfs::base::byte> input,
                            slsfs::backend::ssbd::handler_ptr next)
    {
        slsfs::log::log("start_2pc_prepare");
        std::uint32_t const realpos = input.position();
        std::uint32_t const endpos  = realpos + input.size();

        auto outstanding_requests = std::make_shared<std::atomic<int>>(0);
        auto all_ssbd_agree       = std::make_shared<std::atomic<bool>>(true);
        for (std::uint32_t currentpos = realpos, buffer_pointer_offset = 0; currentpos < endpos;)
        {
            std::uint32_t const blockid = currentpos / blocksize();
            std::uint32_t const offset  = currentpos % blocksize();
            std::uint32_t const blockwritesize = std::min<std::uint32_t>(endpos - currentpos,
                                                                         blocksize() - offset);
            slsfs::log::log("start_2pc_prepare sending: {}, {}, {}",
                            blockid, offset, blockwritesize);

            boost::asio::mutable_buffer partial_buffer_view (input.data() + buffer_pointer_offset,
                                                             blockwritesize);

            int const backend_index = select_replica(input.uuid(), blockid, 0);
            auto selected = backendlist_.at(backend_index);

            slsfs::leveldb_pack::packet_pointer request = slsfs::leveldb_pack::create_request(
                input.uuid(),
                slsfs::leveldb_pack::msg_t::two_pc_prepare,
                blockid,
                offset,
                blockwritesize);

            request->data.buf.resize(blockwritesize + headersize());
            std::uint32_t const version_number = slsfs::leveldb_pack::hton(version());
            std::memcpy(request->data.buf.data(), &version_number, sizeof(version_number));

            boost::asio::buffer_copy(partial_buffer_view,
                                     boost::asio::buffer(
                                         request->data.buf.data() + headersize(),
                                         blockwritesize));

            currentpos += blockwritesize;
            buffer_pointer_offset += blockwritesize;

            (*outstanding_requests)++;
            selected->start_send_request(
                request,
                [outstanding_requests, input, next, all_ssbd_agree, this]
                (slsfs::leveldb_pack::packet_pointer response) {
                    switch (response->header.type)
                    {
                    case slsfs::leveldb_pack::msg_t::two_pc_prepare_agree:
                        slsfs::log::log("2pc client agreed. Left {}", (*outstanding_requests - 1));
                        break;

                    case slsfs::leveldb_pack::msg_t::two_pc_prepare_abort:
                        slsfs::log::log("2pc abort: {}", response->header.print());
                        *all_ssbd_agree = false;
                        break;

                    default:
                        slsfs::log::log("unwanted header type {}", response->header.print());
                        *all_ssbd_agree = false;
                        break;
                    }

                    if (--(*outstanding_requests) == 0)
                        start_2pc_commit(input, *all_ssbd_agree, next);
                });
        }
    }

    void start_2pc_commit (slsfs::jsre::request_parser<slsfs::base::byte> input,
                           bool all_ssbd_agree,
                           slsfs::backend::ssbd::handler_ptr next)
    {
        slsfs::log::log("start_2pc_commit");
        std::uint32_t const realpos = input.position();
        std::uint32_t const endpos  = realpos + input.size();

        auto outstanding_requests = std::make_shared<std::atomic<int>>(0);
        for (std::uint32_t currentpos = realpos, buffer_pointer_offset = 0; currentpos < endpos;)
        {
            std::uint32_t const blockid = currentpos / blocksize();
            std::uint32_t const offset  = currentpos % blocksize();
            std::uint32_t const blockwritesize = std::min<std::uint32_t>(endpos - currentpos,
                                                                         blocksize() - offset);
            slsfs::log::log("start_2pc_commit: {}, {}, {}",
                            blockid, offset, blockwritesize);

            boost::asio::mutable_buffer partial_buffer_view (input.data() + buffer_pointer_offset,
                                                             blockwritesize);

            int const selected_index = select_replica(input.uuid(), blockid, 0);
            auto selected = backendlist_.at(selected_index);

            slsfs::leveldb_pack::packet_pointer request = slsfs::leveldb_pack::create_request(
                input.uuid(),
                all_ssbd_agree?
                    slsfs::leveldb_pack::msg_t::two_pc_commit_execute:
                    slsfs::leveldb_pack::msg_t::two_pc_commit_rollback,
                blockid,
                offset,
                /*blockwritesize*/ 0);

            currentpos += blockwritesize;
            buffer_pointer_offset += blockwritesize;

            (*outstanding_requests)++;
            selected->start_send_request(
                request,
                [outstanding_requests, input, next, all_ssbd_agree, this]
                (slsfs::leveldb_pack::packet_pointer response) {
                    switch (response->header.type)
                    {
                    case slsfs::leveldb_pack::msg_t::two_pc_commit_ack:
                        break;

                    default:
                        slsfs::log::log("start_2pc_commit unwanted header type {}", response->header.print());
                        break;
                    }

                    if (--(*outstanding_requests) == 0)
                    {
                        if (next)
                            std::invoke(*next, slsfs::base::buf{'O', 'K'});

                        if (all_ssbd_agree)
                            start_replication(input, nullptr);
                    }
                });
        }
    }

    void start_replication (slsfs::jsre::request_parser<slsfs::base::byte> input,
                            slsfs::backend::ssbd::handler_ptr next)
    {
        slsfs::log::log("start_replication");
        std::uint32_t const realpos = input.position();
        std::uint32_t const endpos  = realpos + input.size();

        auto outstanding_requests = std::make_shared<std::atomic<int>>(0);
        for (std::uint32_t currentpos = realpos, buffer_pointer_offset = 0; currentpos < endpos;)
        {
            std::uint32_t const blockid = currentpos / blocksize();
            std::uint32_t const offset  = currentpos % blocksize();
            std::uint32_t const blockwritesize = std::min<std::uint32_t>(endpos - currentpos,
                                                                         blocksize() - offset);
            slsfs::log::log("start_replication: {}, {}, {}",
                            blockid, offset, blockwritesize);

            boost::asio::mutable_buffer partial_buffer_view (input.data() + buffer_pointer_offset,
                                                             blockwritesize);

            currentpos += blockwritesize;
            buffer_pointer_offset += blockwritesize;
            // we already have one copy at replica_index 0 from the 2pc part, so start at replica 1
            for (int replica_index = 1; replica_index < replication_size_; replica_index++)
            {
                int const selected_index = select_replica(input.uuid(), blockid, replica_index);
                auto selected = backendlist_.at(selected_index);

                slsfs::leveldb_pack::packet_pointer request = slsfs::leveldb_pack::create_request(
                    input.uuid(),
                    slsfs::leveldb_pack::msg_t::replication,
                    blockid,
                    offset,
                    blockwritesize);

                (*outstanding_requests)++;
                selected->start_send_request(
                    request,
                    [outstanding_requests, replica_index, input, next, this]
                    (slsfs::leveldb_pack::packet_pointer response) {
                        switch (response->header.type)
                        {
                        case slsfs::leveldb_pack::msg_t::ack:
                            break;

                        default:
                            slsfs::log::log("start_replication unwanted header type {}",
                                            response->header.print());
                            break;
                        }

                        if (--(*outstanding_requests) == 0 and next)
                            std::invoke(*next, slsfs::base::buf{'O', 'K'});
                    });
            }
        }
    }

    struct buf_stat_t
    {
        std::atomic<bool> ready = false;
        slsfs::base::buf  buf;
    };

    void start_read (slsfs::jsre::request_parser<slsfs::base::byte> const input,
                     slsfs::backend::ssbd::handler_ptr next)
    {
        slsfs::log::log("ssbd start_read");

        std::uint32_t const realpos  = input.position();
        std::uint32_t const readsize = input.size();
        std::uint32_t const endpos   = realpos + readsize;

        if (readsize == 0)
        {
            std::invoke(*next, slsfs::base::buf{});
            return;
        }

        int result_vector_size = 0;
        for (std::uint32_t currentpos = realpos; currentpos < endpos; result_vector_size++)
        {
            std::uint32_t const offset = currentpos % blocksize();
            std::uint32_t const blockreadsize = std::min<std::uint32_t>(endpos - currentpos,
                                                                        blocksize() - offset);
            currentpos += blockreadsize;
        }

        auto result_accumulator = std::make_shared<oneapi::tbb::concurrent_vector<buf_stat_t>>(result_vector_size);

        for (std::uint32_t currentpos = realpos, index = 0; currentpos < endpos; index++)
        {
            std::uint32_t const blockid = currentpos / blocksize();
            std::uint32_t const offset  = currentpos % blocksize();
            std::uint32_t const blockreadsize = std::min<std::uint32_t>(endpos - currentpos,
                                                                        blocksize() - offset);
            slsfs::log::log("start_replication: {}, {}, {}",
                            blockid, offset, blockreadsize);

            slsfs::leveldb_pack::packet_pointer request = slsfs::leveldb_pack::create_request(
                input.uuid(),
                slsfs::leveldb_pack::msg_t::get,
                blockid,
                offset,
                blockreadsize);

            int const selected_index = select_replica(input.uuid(), blockid, 0);
            auto selected = backendlist_.at(selected_index);
            selected->start_send_request(
                request,
                [result_accumulator, input, next, index, this]
                (slsfs::leveldb_pack::packet_pointer resp) {
                    result_accumulator->at(index).ready = true;
                    result_accumulator->at(index).buf   = std::move(resp->data.buf);

                    for (buf_stat_t& bufstat : *result_accumulator)
                        if (not bufstat.ready)
                            return;

                    slsfs::base::buf collect;
                    collect.reserve((result_accumulator->size() + 2 /* head and tail */) * blocksize());
                    for (buf_stat_t& bufstat : *result_accumulator)
                        collect.insert(collect.begin(),
                                       bufstat.buf.begin(),
                                       bufstat.buf.end());

                    slsfs::log::log("read executing next with bufsize = {}", collect.size());
                    std::invoke(*next, std::move(collect));
                });
            currentpos += blockreadsize;
        }
    }

    void start_meta_addfile (slsfs::jsre::request_parser<slsfs::base::byte> const input,
                             slsfs::backend::ssbd::handler_ptr next)
    {
        slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();
        ptr->header.gen();
        ptr->header.key = input.uuid();

        slsfs::jsre::request read_request {
            .type      = slsfs::jsre::type_t::file,
            .operation = slsfs::jsre::operation_t::read,
            .uuid      = input.uuid(),
            .position  = 0,
            .size      = blocksize()
        };

        read_request.to_network_format();
        ptr->data.buf.resize(sizeof (read_request));
        std::memcpy(ptr->data.buf.data(), &read_request, sizeof (read_request));

        /*
           Read first block: in the first block for a directory,
           end of file list (== number of files) (network format). i.e.
           struct slsfsdf::ssbd::meta::stats {
               std::uint32_t files;
               std::uint32_t last;
           }

           0: 4K block
           [[{meta::stats}: 64 bytes], [{owner, permission, filename}: 64 bytes], ...]
        */

        auto readnext = std::make_shared<slsfs::backend::ssbd::handler>(
            [input, next, this]
            (slsfs::base::buf file_content) {
                slsfs::jsre::meta::stats stat;
                if (file_content.size() < sizeof(stat))
                {
                    std::invoke(*next, slsfs::base::buf {'N', 'O', 'S', 'U', 'C', 'H', 'D', 'I', 'R'});
                    return;
                }

                // get number of files
                std::memcpy(std::addressof(stat), file_content.data(), sizeof(stat));
                stat.to_host_format();

                // get metadata (owner, permission, filename) from request (network format)
                slsfs::jsre::meta::filemeta meta;
                std::memcpy(std::addressof(meta), input.data(), sizeof(meta));

                std::uint32_t const position = (stat.file_count) * sizeof(meta);

                // put metadata to position
                if (position < blocksize())
                {// update at first block => reuse file_content

                    // append stat
                    std::memcpy(file_content.data() + position,
                                std::addressof(meta), sizeof(meta));

                    stat.file_count++;
                    stat.to_network_format();

                    // update at position 0
                    std::memcpy(file_content.data(), std::addressof(stat), sizeof(stat));

                    //slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();
                    //slsfs::jsre::request_parser<slsfs::base::byte> read_request_input;
                    //start_read(read_request_input, readnext);
                    //start_2pc_prepare()
                }
            });

        slsfs::jsre::request_parser<slsfs::base::byte> read_request_input {ptr};
        start_read(read_request_input, readnext);
    }

public:
    storage_conf_ssbd_backend(boost::asio::io_context& io): io_context_{io} {}

    auto headersize() -> std::uint32_t { return sizeof(std::uint32_t); };
    virtual
    auto blocksize() -> std::uint32_t override {
        return storage_conf::blocksize() - headersize();
    }

    void init(slsfs::base::json const& config) override
    {
        replication_size_ = config["replication_size"].get<int>();
        for (auto&& element : config["hosts"])
        {
            std::string const host = element["host"].get<std::string>();
            std::string const port = element["port"].get<std::string>();
            slsfs::log::log("adding {}:{}", host, port);

            backendlist_.push_back(std::make_shared<slsfs::backend::ssbd>(io_context_, host, port));
        }
        storage_conf::init(config);
    }

    bool use_async() override { return true; }

    void start_perform (slsfs::jsre::request_parser<slsfs::base::byte> const& input,
                        std::function<void(slsfs::base::buf)> next) override
    {
        switch (input.operation())
        {
        case slsfs::jsre::operation_t::write:
        {
            auto next_ptr = std::make_shared<std::function<void(slsfs::base::buf)>>(std::move(next));
            slsfs::log::log("slsfs::jsre::operation_t::write");
            start_2pc_prepare(input, next_ptr);
            break;
        }

        case slsfs::jsre::operation_t::read:
        {
            auto next_ptr = std::make_shared<std::function<void(slsfs::base::buf)>>(std::move(next));
            slsfs::log::log("slsfs::jsre::operation_t::read");
            start_read(input, next_ptr);
            break;
        }
        }
    }

    void start_perform_metadata (slsfs::jsre::request_parser<slsfs::base::byte> const& input,
                                 std::function<void(slsfs::base::buf)> next) override
    {
        switch (input.meta_operation())
        {
        case slsfs::jsre::meta_operation_t::addfile:
        {
            auto next_ptr = std::make_shared<std::function<void(slsfs::base::buf)>>(std::move(next));

            break;
        }

        case slsfs::jsre::meta_operation_t::mkdir:
        {
            break;
        }

        case slsfs::jsre::meta_operation_t::ls:
        {
            auto next_ptr = std::make_shared<std::function<void(slsfs::base::buf)>>(std::move(next));
            break;
        }
        }
    }
};

} // namespace slsfsdf


#endif // STORAGE_CONF_SSBD_BACKEND_HPP__
