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

namespace detail
{

class recoder
{
    using filecheckmap =
        oneapi::tbb::concurrent_hash_map<slsfs::pack::key_t,
                                         int /* not used */,
                                         slsfs::uuid::hash_compare<slsfs::pack::key_t>>;
    filecheckmap map_;

public:
    bool is_checked (slsfs::pack::key_t const& uuid)
    {
        filecheckmap::accessor it;
        return map_.find(it, uuid);
    }

    void mark_checked (slsfs::pack::key_t const& uuid) {
        map_.emplace(uuid, 0);
    }

    bool erase_checked (slsfs::pack::key_t const& uuid) {
        return map_.erase(uuid);
    }
};

} // namespace

// Storage backend configuration for SSBD stripe
class storage_conf_ssbd_backend : public storage_conf
{
    boost::asio::io_context& io_context_;
    int replication_size_ = 3, replication_start_index_ = 0;

    // first half is normal operation; second half is backup operation
    std::vector<std::shared_ptr<slsfs::backend::ssbd>> backend_list_;

    detail::recoder recoder_;

    void connect() override
    {
        for (std::shared_ptr<slsfs::backend::ssbd> host : backend_list_)
            host->connect();
    }

    void close() override
    {
        for (std::shared_ptr<slsfs::backend::ssbd> host : backend_list_)
            host->close();
    }

    static
    auto static_engine() -> std::mt19937&
    {
        static thread_local std::mt19937 mt;
        return mt;
    }

    int select_replica(slsfs::pack::key_t const& uuid,
                       int const partition,
                       int const replication_index)
    {
        std::seed_seq seeds {uuid.begin(), uuid.end()};
        static_engine().seed(seeds);

        std::uniform_int_distribution<> dist(0, replication_start_index_ - 1);
        static_engine().discard(partition * (partition * replication_index));

        return dist(static_engine());
    }

    static
    auto version () -> std::uint32_t
    {
        std::uint64_t v = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return static_cast<std::uint32_t>(v >> 6);
    }

    // 2pc stuff
    void start_2pc_prepare (slsfs::jsre::request_parser<slsfs::base::byte> input,
                            slsfs::backend::ssbd::handler_ptr next)
    {
        auto request_dearline_timer = std::make_shared<boost::asio::steady_timer>(io_context_);
        using namespace std::chrono_literals;

        // every request must finish in 30s
        request_dearline_timer->expires_from_now(30s);
        request_dearline_timer->async_wait(
            [next, request_dearline_timer, input, this] (boost::system::error_code ec) {
                switch (ec.value())
                {
                case boost::system::errc::operation_canceled: // timer canceled
                    break;
                case boost::system::errc::success: // timer timeout
                    recoder_.erase_checked(input.uuid());
                    std::invoke(*next, slsfs::base::to_buf("Error: request timeout internally"));
                    [[fallthrough]];
                default:
                    slsfs::log::log<slsfs::log::level::error>("timer_reset: write job '{}:{}' timeout.", input.print(), input.pack->header.print());
                    break;
                }
        });

        std::uint32_t const realpos = input.position();
        std::uint32_t const endpos  = realpos + input.size();
        std::uint32_t const selected_version = version();

        slsfs::log::log("start_2pc_prepare: {}", input.print());

        auto outstanding_requests = std::make_shared<std::atomic<int>>(0);
        auto all_ssbd_agree       = std::make_shared<std::atomic<bool>>(true);
        for (std::uint32_t currentpos = realpos, buffer_pointer_offset = 0; currentpos < endpos;)
        {
            std::uint32_t const blockid = currentpos / blocksize();
            std::uint32_t const offset  = currentpos % blocksize();
            std::uint32_t const blockwritesize = std::min<std::uint32_t>(endpos - currentpos,
                                                                         blocksize() - offset);
            slsfs::log::log("start_2pc_prepare sending: bid={}, @{}, size={}",
                            blockid, offset, blockwritesize);

            int const backend_index = select_replica(input.uuid(), blockid, 0);
            auto selected = backend_list_.at(backend_index);

            slsfs::leveldb_pack::packet_pointer request = slsfs::leveldb_pack::create_request(
                input.uuid(),
                recoder_.is_checked(input.uuid())?
                    slsfs::leveldb_pack::msg_t::two_pc_prepare_quick:
                    slsfs::leveldb_pack::msg_t::two_pc_prepare,
                selected_version,
                blockid,
                offset,
                blockwritesize);

            request->data.buf.resize(blockwritesize + headersize());

            std::memcpy(request->data.buf.data() + headersize(),
                        input.data() + buffer_pointer_offset,
                        blockwritesize);

            currentpos += blockwritesize;
            buffer_pointer_offset += blockwritesize;

            (*outstanding_requests)++;
            selected->start_send_request(
                request,
                [outstanding_requests, input, all_ssbd_agree, selected_version, request_dearline_timer, next, this]
                (slsfs::leveldb_pack::packet_pointer response) {
                    switch (response->header.type)
                    {
                    case slsfs::leveldb_pack::msg_t::two_pc_prepare_agree:
                        slsfs::log::log("2pc client agreed. Left {}", (*outstanding_requests - 1));
                        break;

                    case slsfs::leveldb_pack::msg_t::two_pc_prepare_abort:
                        slsfs::log::log("2pc abort: {}", response->header.print());
                        slsfs::log::log("2pc input: {}", input.print());
                        *all_ssbd_agree = false;
                        break;

                    default:
                        //slsfs::log::log("unwanted header type {}", static_cast<int>(response->header.type));
                        *all_ssbd_agree = false;
                        break;
                    }

                    if (--(*outstanding_requests) == 0)
                    {
                        request_dearline_timer->cancel();

                        if (*all_ssbd_agree)
                        {
                            recoder_.mark_checked(input.uuid());
                            std::invoke(*next, slsfs::base::to_buf("OK"));
                        }
                        else
                        {
                            recoder_.erase_checked(input.uuid());
                            std::invoke(*next, slsfs::base::to_buf("Error: Found Pending 2PC Log"));
                        }

                        start_2pc_commit (input,
                                          *all_ssbd_agree,
                                          selected_version,
                                          nullptr);
                    }
                });
        }
    }

    void start_2pc_commit (slsfs::jsre::request_parser<slsfs::base::byte> input,
                           bool          const all_ssbd_agree,
                           std::uint32_t const selected_version,
                           slsfs::backend::ssbd::handler_ptr next)
    {
        std::uint32_t const realpos = input.position();
        std::uint32_t const endpos  = realpos + input.size();

        auto outstanding_requests = std::make_shared<std::atomic<int>>(0);
        for (std::uint32_t currentpos = realpos, buffer_pointer_offset = 0; currentpos < endpos;)
        {
            std::uint32_t const blockid = currentpos / blocksize();
            std::uint32_t const offset  = currentpos % blocksize();
            std::uint32_t const blockwritesize = std::min<std::uint32_t>(endpos - currentpos,
                                                                         blocksize() - offset);
            slsfs::log::log("start_2pc_commit: bid={}, @{}, size={}",
                            blockid, offset, blockwritesize);

            int const selected_index = select_replica(input.uuid(), blockid, 0);
            auto selected = backend_list_.at(selected_index);

            slsfs::leveldb_pack::packet_pointer request = slsfs::leveldb_pack::create_request(
                input.uuid(),
                all_ssbd_agree?
                    slsfs::leveldb_pack::msg_t::two_pc_commit_execute:
                    slsfs::leveldb_pack::msg_t::two_pc_commit_rollback,
                selected_version,
                blockid,
                offset,
                /*blockwritesize*/ 0);

            currentpos += blockwritesize;
            buffer_pointer_offset += blockwritesize;

            (*outstanding_requests)++;
            selected->start_send_request(
                request,
                [outstanding_requests, input, all_ssbd_agree, selected_version, next, this]
                (slsfs::leveldb_pack::packet_pointer response) {
                    switch (response->header.type)
                    {
                    case slsfs::leveldb_pack::msg_t::two_pc_commit_ack:
                        break;

                    default:
                        slsfs::log::log("start_2pc_commit unwanted header type {}", response->header.print());

                        if (next)
                        {
                            slsfs::log::log("running request error reply");
                            recoder_.erase_checked(input.uuid());
                            std::invoke(*next, slsfs::base::to_buf("Error: Commit Message Get Error Reply"));
                        }
                    }

                    if (--(*outstanding_requests) == 0)
                    {
                        if (next)
                            std::invoke(*next, slsfs::base::to_buf("OK"));

                        if (all_ssbd_agree && replication_size_ > 1)
                            start_replication (input,
                                               selected_version,
                                               nullptr);
                    }
                });
        }
    }

    void start_replication (slsfs::jsre::request_parser<slsfs::base::byte> input,
                            std::uint32_t const selected_version,
                            slsfs::backend::ssbd::handler_ptr next)
    {
        slsfs::log::log("start_replication with {}", input.print());
        std::uint32_t const realpos = input.position();
        std::uint32_t const endpos  = realpos + input.size();

        auto outstanding_requests = std::make_shared<std::atomic<int>>(0);
        for (std::uint32_t currentpos = realpos, buffer_pointer_offset = 0; currentpos < endpos;)
        {
            std::uint32_t const blockid = currentpos / blocksize();
            std::uint32_t const offset  = currentpos % blocksize();
            std::uint32_t const blockwritesize = std::min<std::uint32_t>(endpos - currentpos,
                                                                         blocksize() - offset);
            slsfs::log::log("start_replication: bid={}, @{}, size={}",
                            blockid, offset, blockwritesize);

            currentpos += blockwritesize;
            buffer_pointer_offset += blockwritesize;
            // we already have one copy at replica_index 0 from the 2pc part, so start at replica 1
            for (int replica_index = 1; replica_index < replication_size_; replica_index++)
            {
                int const selected_index =
                    replication_start_index_ + select_replica(input.uuid(), blockid, replica_index);
                auto selected = backend_list_.at(selected_index);

                slsfs::leveldb_pack::packet_pointer request = slsfs::leveldb_pack::create_request(
                    input.uuid(),
                    slsfs::leveldb_pack::msg_t::replication,
                    selected_version,
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
                            if (next)
                            {
                                recoder_.erase_checked(input.uuid());
                                std::invoke(*next, slsfs::base::to_buf("Error: Replication Failed"));
                            }
                            return;
                        }

                        if (--(*outstanding_requests) == 0 and next)
                        {
                            slsfs::log::log("replication finished {}", response->header.print());
                            std::invoke(*next, slsfs::base::to_buf("OK"));
                        }
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
        auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
        using namespace std::chrono_literals;
        timer->expires_from_now(30s);
        timer->async_wait(
            [next, timer, input, this] (boost::system::error_code ec) {
                switch (ec.value())
                {
                case boost::system::errc::operation_canceled: // timer canceled
                    break;
                case boost::system::errc::success: // timer timeout
                    recoder_.erase_checked(input.uuid());
                    std::invoke(*next, slsfs::base::to_buf("Error: Read Request Timeout"));
                    [[fallthrough]];

                default:
                    slsfs::log::log<slsfs::log::level::error>("timer_reset: read job '{}:{}' timeout.", input.print(), input.pack->header.print());
                    break;
                }
        });

        std::uint32_t const realpos  = input.position();
        std::uint32_t const readsize = input.size();
        std::uint32_t const endpos   = realpos + readsize;

        if (readsize == 0)
        {
            std::invoke(*next, slsfs::base::buf{});
            timer->cancel();
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
            slsfs::log::log("start_read: {}, {}, {}",
                            blockid, offset, blockreadsize);

            slsfs::leveldb_pack::packet_pointer request = slsfs::leveldb_pack::create_request(
                input.uuid(),
                slsfs::leveldb_pack::msg_t::get,
                0 /* version number. not use for read request */,
                blockid,
                offset,
                blockreadsize);

            int const selected_index = select_replica(input.uuid(), blockid, 0);
            auto selected = backend_list_.at(selected_index);
            selected->start_send_request(
                request,
                [result_accumulator, input, next, index, timer, this]
                (slsfs::leveldb_pack::packet_pointer resp) {
                    result_accumulator->at(index).ready = true;
                    result_accumulator->at(index).buf   = std::move(resp->data.buf);

                    for (buf_stat_t& bufstat : *result_accumulator)
                        if (not bufstat.ready)
                            return;

                    timer->cancel();
                    slsfs::base::buf collect;
                    collect.reserve((result_accumulator->size() + 2 /* head and tail */) * blocksize());
                    for (buf_stat_t& bufstat : *result_accumulator)
                        collect.insert(collect.end(),
                                       bufstat.buf.begin(),
                                       bufstat.buf.end());

                    std::invoke(*next, std::move(collect));
                });
            currentpos += blockreadsize;
        }
    }

    void start_meta_addfile (slsfs::jsre::request_parser<slsfs::base::byte> const input,
                             slsfs::backend::ssbd::handler_ptr next,
                             std::uint32_t readsize = 4096)
    {
        slsfs::log::log("start_meta_addfile {}", input.print());
        slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();
        ptr->header.gen();
        ptr->header.key = input.uuid();

        slsfs::jsre::request read_request {
            .type      = slsfs::jsre::type_t::file,
            .operation = slsfs::jsre::operation_t::read,
            .position  = 0,
            .size      = readsize
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
                slsfs::log::log("start_meta_addfile get read resp: size={}", file_content.size());
                slsfs::jsre::meta::stats stat;
                if (file_content.size() < sizeof(stat))
                {
                    recoder_.erase_checked(input.uuid());
                    std::invoke(*next, slsfs::base::to_buf("Error: No such directory"));
                    return;
                }

                // get number of files
                std::memcpy(std::addressof(stat), file_content.data(), sizeof(stat));
                stat.to_host_format();

                // not enough content. reread all file
                if (stat.file_count * sizeof(slsfs::jsre::meta::filemeta) > file_content.size())
                {
                    start_meta_addfile(input, next, stat.file_count * sizeof(slsfs::jsre::meta::filemeta));
                    return;
                }

                // get metadata (owner, permission, filename) from request (network format)
                slsfs::jsre::meta::filemeta filemeta;
                std::memcpy(std::addressof(filemeta), input.data(), sizeof(filemeta));

                // calculate the append address
                std::uint32_t const position = (++stat.file_count) * sizeof(filemeta);

                // append stat
                file_content.resize(position + sizeof(filemeta));
                std::memcpy(file_content.data() + position,
                            std::addressof(filemeta), sizeof(filemeta));

                std::string f(filemeta.filename.begin(), filemeta.filename.end());
                slsfs::log::log("start_meta_addfile save filename={}", f);
                slsfs::log::log("start_meta_addfile stat now have {} files", stat.file_count);

                stat.to_network_format();

                // update at position 0
                std::memcpy(file_content.data(), std::addressof(stat), sizeof(stat));

                slsfs::jsre::request write_request {
                    .type      = slsfs::jsre::type_t::file,
                    .operation = slsfs::jsre::operation_t::write,
                    .position  = 0,
                    .size      = static_cast<std::uint32_t>(file_content.size())
                };

                write_request.to_network_format();
                slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();
                ptr->header.gen();
                ptr->header.key = input.uuid();

                ptr->data.buf.resize(sizeof (write_request) + file_content.size());
                std::memcpy(ptr->data.buf.data(), &write_request, sizeof (write_request));
                std::memcpy(ptr->data.buf.data() + sizeof (write_request), file_content.data(), file_content.size());

                slsfs::jsre::request_parser<slsfs::base::byte> write_request_parser{ptr};

                // send the write request, and at finish, write back to client
                start_2pc_prepare(write_request_parser, next);
            });

        slsfs::jsre::request_parser<slsfs::base::byte> read_request_input {ptr};
        start_read(read_request_input, readnext);
    }

    void start_meta_mkdir (slsfs::jsre::request_parser<slsfs::base::byte> const input,
                           slsfs::backend::ssbd::handler_ptr next)
    {
        slsfs::log::log("start_meta_mkdir {}", input.print());
        slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();
        ptr->header.gen();
        ptr->header.key = input.uuid();

        // create a stat and put into ssbd
        slsfs::jsre::meta::stats stat;
        stat.to_network_format();

        slsfs::jsre::request write_request {
            .type      = slsfs::jsre::type_t::file,
            .operation = slsfs::jsre::operation_t::write,
            .position  = 0,
            .size      = sizeof(stat)
        };

        write_request.to_network_format();
        ptr->data.buf.resize(sizeof (write_request) + sizeof (stat));
        std::memcpy(ptr->data.buf.data(), &write_request, sizeof (write_request));
        std::memcpy(ptr->data.buf.data() + sizeof (write_request), &stat, sizeof (stat));

        slsfs::jsre::request_parser<slsfs::base::byte> write_request_input {ptr};
        start_2pc_prepare(write_request_input, next);
    }

    void start_meta_ls (slsfs::jsre::request_parser<slsfs::base::byte> const input,
                        slsfs::backend::ssbd::handler_ptr next,
                        std::uint32_t readsize = 4096)
    {
        slsfs::log::log("start_meta_ls {}", input.print());
        slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();
        ptr->header.gen();
        ptr->header.key = input.uuid();

        slsfs::jsre::request read_request {
            .type      = slsfs::jsre::type_t::file,
            .operation = slsfs::jsre::operation_t::read,
            .position  = 0,
            .size      = readsize
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
                slsfs::log::log("start_meta_ls get file resp: size={}", file_content.size());
                slsfs::jsre::meta::stats stat;
                if (file_content.size() < sizeof(stat))
                {
                    recoder_.erase_checked(input.uuid());
                    std::invoke(*next, slsfs::base::to_buf("Error: No such directory"));
                    return;
                }

                // get number of files
                std::memcpy(std::addressof(stat), file_content.data(), sizeof(stat));
                stat.to_host_format();

                // not enough content. reread all file
                if (stat.file_count * sizeof(slsfs::jsre::meta::filemeta) > file_content.size())
                {
                    start_meta_ls(input, next, stat.file_count * sizeof(slsfs::jsre::meta::filemeta));
                    return;
                }

                slsfs::log::log("start_meta_ls stat have {} files", stat.file_count);

                // get metadata (owner, permission, filename) from request (network format)
                slsfs::base::buf filenames;

                std::string const total = fmt::format("total: {} files\n", stat.file_count);
                filenames.insert(filenames.end(),
                                 total.begin(), total.end());

                for (unsigned int i = 1; i <= stat.file_count; i++)
                {
                    slsfs::jsre::meta::filemeta filemeta;
                    unsigned int const pos = i * sizeof(filemeta);
                    std::memcpy(std::addressof(filemeta), file_content.data() + pos, sizeof(filemeta));

                    auto end = std::find(filemeta.filename.begin(), filemeta.filename.end(), '\0');
                    filenames.insert(filenames.end(),
                                     filemeta.filename.begin(), end);

                    filenames.push_back('\n');
                }

                std::invoke(*next, filenames);
            });

        slsfs::jsre::request_parser<slsfs::base::byte> read_request_input {ptr};
        start_read(read_request_input, readnext);
    }

public:
    storage_conf_ssbd_backend(boost::asio::io_context& io): io_context_{io} {}

    auto headersize() -> std::uint32_t { return 0; };
    virtual
    auto blocksize() -> std::uint32_t override {
        return storage_conf::blocksize() - headersize();
    }

    void init(slsfs::base::json const& config) override
    {
        replication_size_ = config["replication_size"].get<int>();

        // setup normal operating host
        for (auto&& element : config["hosts"])
        {
            std::string const host = element["host"].get<std::string>();
            std::string const port = element["port"].get<std::string>();
            slsfs::log::log("adding {}:{}", host, port);

            backend_list_.push_back(std::make_shared<slsfs::backend::ssbd>(io_context_, host, port));
        }

        replication_start_index_ = backend_list_.size();
         // setup replication operating host (but as the second half of the vector)
        for (auto&& element : config["hosts"])
        {
            std::string const host = element["host"].get<std::string>();
            std::string const port = element["port"].get<std::string>();
            slsfs::log::log("adding replication {}:{}", host, port);

            backend_list_.push_back(std::make_shared<slsfs::backend::ssbd>(io_context_, host, port));
        }
        storage_conf::init(config);
    }

    bool use_async() override { return true; }

    void start_perform (slsfs::jsre::request_parser<slsfs::base::byte> const& input,
                        std::function<void(slsfs::base::buf)> next) override
    {
        auto next_ptr = std::make_shared<std::function<void(slsfs::base::buf)>>(std::move(next));
        switch (input.operation())
        {
        case slsfs::jsre::operation_t::write:
            slsfs::log::log("start_perform -> slsfs::jsre::operation_t::write");
            start_2pc_prepare(input, next_ptr);
            break;

        case slsfs::jsre::operation_t::read:
            slsfs::log::log("start_perform -> slsfs::jsre::operation_t::read");
            start_read(input, next_ptr);
            break;
        }
    }

    void start_perform_metadata (slsfs::jsre::request_parser<slsfs::base::byte> const& input,
                                 std::function<void(slsfs::base::buf)> next) override
    {
        auto next_ptr = std::make_shared<std::function<void(slsfs::base::buf)>>(std::move(next));
        switch (input.meta_operation())
        {
        case slsfs::jsre::meta_operation_t::addfile:
            start_meta_addfile(input, next_ptr);
            break;

        case slsfs::jsre::meta_operation_t::mkdir:
            start_meta_mkdir(input, next_ptr);
            break;

        case slsfs::jsre::meta_operation_t::ls:
            start_meta_ls(input, next_ptr);
            break;
        }
    }
};

} // namespace slsfsdf


#endif // STORAGE_CONF_SSBD_BACKEND_HPP__
