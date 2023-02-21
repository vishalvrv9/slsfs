#pragma once

#ifndef SLSFS_BACKEND_SSBD_HPP__
#define SLSFS_BACKEND_SSBD_HPP__

//#include "backend.hpp"
#include "storage.hpp"
#include "basetypes.hpp"
#include "scope-exit.hpp"
#include "leveldb-serializer.hpp"
#include "debuglog.hpp"
#include "socket-writer.hpp"

#include <oneapi/tbb/concurrent_hash_map.h>

#include <boost/signals2.hpp>
#include <boost/asio.hpp>

namespace slsfs::backend
{

namespace detail
{

class job
{
    boost::signals2::signal<void(leveldb_pack::packet_pointer)> next_;

public:
    job() = default;

    template<typename Next>
    job(Next && callable) { next_.connect(callable); }

    template<typename Next>
    void register_next_job(Next && callable)
    {
        next_.disconnect_all_slots();
        next_.connect(callable);
    }

    void run(leveldb_pack::packet_pointer ptr) { next_(ptr); }
};

using job_ptr = std::shared_ptr<job>;

} // namespace detail


class ssbd
{
    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::socket socket_;
    std::string const host_, port_;
    using jobmap =
        oneapi::tbb::concurrent_hash_map<
            leveldb_pack::packet_header,
            detail::job_ptr,
            leveldb_pack::packet_header_key_hash_compare>;

    std::once_flag read_started_flag_;
    jobmap outstanding_jobs_;

    using jobmap_accessor = decltype(outstanding_jobs_)::accessor;

    socket_writer::socket_writer<leveldb_pack::packet, std::vector<leveldb_pack::unit_t>> writer_;

    void start_read_loop() {
        std::call_once(read_started_flag_, [this](){ start_read_one(); });
    }

    void start_read_one()
    {
        leveldb_pack::packet_pointer resp = std::make_shared<leveldb_pack::packet>();
        auto headerbuf = std::make_shared<std::vector<leveldb_pack::unit_t>> (leveldb_pack::packet_header::bytesize);
        log::log("async_read read head called headersize: {}", leveldb_pack::packet_header::bytesize);
        boost::asio::async_read(
            socket_,
            boost::asio::buffer(headerbuf->data(), headerbuf->size()),
            [this, resp, headerbuf] (boost::system::error_code const& ec, std::size_t transferred_size) {
                if (ec)
                {
                    log::log("ssbd backend: {} have boost error: {} on start_read_one() header {}", host_, ec.message(), resp->header.print());
                    start_read_one();
                    return;
                }

                assert(headerbuf->size() == transferred_size);
                resp->header.parse(headerbuf->data());
                start_read_one_body(resp);
            });
    }

    void start_read_one_body(leveldb_pack::packet_pointer resp)
    {
        auto bodybuf = std::make_shared<base::buf>(resp->header.datasize, 0);

        log::log("async_read read body called readsize={}", bodybuf->size());
        boost::asio::async_read(
            socket_,
            boost::asio::buffer(bodybuf->data(), bodybuf->size()),
            [this, resp, bodybuf] (boost::system::error_code const& ec, std::size_t transferred_size) {
                assert(bodybuf->size() == transferred_size);

                if (ec)
                {
                    log::log("ssbd backend: {} have boost error: {} on start_read_one() -> body header {}", host_, ec.message(), resp->header.print());
                    return;
                }

                resp->data.parse(transferred_size, bodybuf->data());
                jobmap_accessor it;
                [[maybe_unused]]
                bool found = outstanding_jobs_.find(it, resp->header);
                assert(found);

                log::log("async read body executing with header {}", resp->header.print());

                it->second->run(resp);
                outstanding_jobs_.erase(it);
                start_read_one();
            });
    }

public:
    ssbd(boost::asio::io_context& io, std::string const& host, std::string const& port):
        io_context_{io}, socket_(io),
        host_{host}, port_{port},
        writer_{io, socket_} {}

    using handler_ptr = std::shared_ptr<std::function<void(base::buf)>>;

    void connect()
    {
        log::log("connect to {}:{}", host_, port_);
        boost::asio::ip::tcp::resolver resolver (io_context_);
        boost::asio::connect (socket_, resolver.resolve(host_, port_));
    }

    void start_send_request (leveldb_pack::packet_pointer request,
                             std::function<void(leveldb_pack::packet_pointer)> on_response)
    {
        detail::job_ptr newjob = std::make_shared<detail::job>(
            [on_response=std::move(on_response)] (leveldb_pack::packet_pointer resp) {
                std::invoke(on_response, resp);
            });

        [[maybe_unused]]
        bool ok = outstanding_jobs_.emplace(request->header, newjob);
        assert(ok);

        auto next = std::make_shared<socket_writer::boost_callback>(
            [this, request] (boost::system::error_code const& ec, std::size_t) {
                if (ec)
                {
                    log::log("ssbd backend: {} boost error: {}; start_send_request: {}",
                             host_, ec.message(), request->header.print());
                    return;
                }
                start_read_loop();
            });
        writer_.start_write_socket(request, next, request->serialize_header());
    }

    void start_meta_append (std::shared_ptr<pack::key_t> const name, boost::asio::mutable_buffer buffer, std::function<void(base::buf)> on_metadata_addend) {}
    void start_meta_update (std::shared_ptr<pack::key_t> const name, boost::asio::mutable_buffer buffer, std::function<void(base::buf)> on_metadata_update) {}
    void start_meta_remove (std::shared_ptr<pack::key_t> const name, std::function<void(base::buf)> on_metadata_remove) {}
};

} // namespace slsfs::backend


#endif // SLSFS_BACKEND_SSBD_HPP__
