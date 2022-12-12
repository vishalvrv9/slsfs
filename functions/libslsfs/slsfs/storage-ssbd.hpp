#pragma once

#ifndef SLSFS_STORAGE_SSBD_HPP__
#define SLSFS_STORAGE_SSBD_HPP__

#include "storage.hpp"
#include "basetypes.hpp"
#include "scope-exit.hpp"
#include "rocksdb-serializer.hpp"
#include "debuglog.hpp"

#include <boost/asio.hpp>

namespace slsfs::storage
{

class ssbd : public interface
{
    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context::strand write_strand_;
    char const * host_;
    char const * port_;
    boost::asio::steady_timer timer_;

public:
    ssbd(boost::asio::io_context& io, char const * host, char const * port):
        io_context_{io}, socket_(io), write_strand_{io}, host_{host}, port_{port}, timer_{io} {}

    void connect() override
    {
        slsfs::log::logstring(fmt::format("connect to {}:{}", host_, port_));
        boost::asio::ip::tcp::resolver resolver (io_context_);
        boost::asio::connect (socket_, resolver.resolve(host_, port_));
    }

    auto read_key(pack::key_t const& name, std::size_t partition,
                  std::size_t location, std::size_t size) -> base::buf override
    {
        { // send get
            rocksdb_pack::packet_pointer ptr = std::make_shared<rocksdb_pack::packet>();
            ptr->header.type = rocksdb_pack::msg_t::get;
            ptr->header.uuid = name;
            ptr->header.blockid = partition;
            ptr->header.position = location;
            ptr->header.datasize = size;
            auto buf = ptr->serialize_header();
            boost::asio::write(socket_, boost::asio::buffer(buf->data(), buf->size()));
        }

        { // read resp
            rocksdb_pack::packet_pointer resp = std::make_shared<rocksdb_pack::packet>();
            std::vector<rocksdb_pack::unit_t> headerbuf(rocksdb_pack::packet_header::bytesize);
            boost::asio::read(socket_, boost::asio::buffer(headerbuf.data(), headerbuf.size()));

            resp->header.parse(headerbuf.data());
            base::buf bodybuf(resp->header.datasize, 0);

            boost::asio::read(socket_, boost::asio::buffer(bodybuf.data(), bodybuf.size()));
            return bodybuf;
        } // read resp
    }

    void start_read_key (std::shared_ptr<pack::key_t> const name, std::size_t partition,
                         std::size_t location, std::size_t size,
                         std::function<void(base::buf)> completeion_handler) override
    {
         // send get
        rocksdb_pack::packet_pointer ptr = std::make_shared<rocksdb_pack::packet>();
        ptr->header.type     = rocksdb_pack::msg_t::get;
        ptr->header.uuid     = *name;
        ptr->header.blockid  = partition;
        ptr->header.position = location;
        ptr->header.datasize = size;
        auto buf = ptr->serialize_header();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(buf->data(), buf->size()),
            boost::asio::bind_executor(
                write_strand_,
                [this, buf, handler=std::move(completeion_handler)] (boost::system::error_code const& ec, std::size_t) {
                    start_read_response(handler);
                }));
    }

    void start_read_response(std::function<void(base::buf)> completeion_handler)
    {
        rocksdb_pack::packet_pointer resp = std::make_shared<rocksdb_pack::packet>();
        auto headerbuf = std::make_shared<std::vector<rocksdb_pack::unit_t>> (rocksdb_pack::packet_header::bytesize);
        boost::asio::async_read(
            socket_,
            boost::asio::buffer(headerbuf->data(), headerbuf->size()),
            boost::asio::bind_executor(
                write_strand_,
                [this, resp, headerbuf, handler=std::move(completeion_handler)] (boost::system::error_code const& ec, std::size_t) {
                    resp->header.parse(headerbuf->data());
                    auto bodybuf = std::make_shared<base::buf>(resp->header.datasize, 0);

                    boost::asio::async_read(
                        socket_,
                        boost::asio::buffer(bodybuf->data(), bodybuf->size()),
                        boost::asio::bind_executor(
                            write_strand_,
                            [bodybuf, handler=std::move(handler)] (boost::system::error_code const&, std::size_t) {
                                std::invoke (handler, *bodybuf);
                            }));
                }));
    }

    bool check_version_ok(pack::key_t const& name, std::size_t partition,
                          std::uint32_t& version) override
    {
        // request commit
        log::logstring("storage-ssbd.hpp check version start");

        rocksdb_pack::packet_pointer ptr = std::make_shared<rocksdb_pack::packet>();
        ptr->header.type = rocksdb_pack::msg_t::merge_request_commit;
        ptr->header.uuid = name;
        ptr->header.blockid = partition;

        std::remove_reference_t<decltype(version)> bigendian_version = rocksdb_pack::hton(version);
        ptr->data.buf.resize(sizeof(bigendian_version));

        std::vector<rocksdb_pack::unit_t> b (sizeof(bigendian_version));
        std::swap(ptr->data.buf, b);
        std::memcpy(ptr->data.buf.data(), &bigendian_version, sizeof(bigendian_version));

        auto buf = ptr->serialize();
        boost::asio::write(socket_, boost::asio::buffer(buf->data(), buf->size()));

        // read resp
        rocksdb_pack::packet_pointer resp = std::make_shared<rocksdb_pack::packet>();
        std::vector<rocksdb_pack::unit_t> headerbuf(rocksdb_pack::packet_header::bytesize);

        log::logstring("storage-ssbd.hpp check version read");
        boost::asio::read(socket_, boost::asio::buffer(headerbuf.data(), headerbuf.size()));

        resp->header.parse(headerbuf.data());

        std::remove_reference_t<decltype(version)> updated_version;
        assert(resp->header.datasize == sizeof(updated_version));

        boost::asio::read(socket_, boost::asio::buffer(std::addressof(updated_version), sizeof(updated_version)));

        updated_version = rocksdb_pack::hton(updated_version);

        bool resp_ok = true;
        switch (resp->header.type)
        {
        case rocksdb_pack::msg_t::merge_vote_agree:
            resp_ok = true;
            break;

        case rocksdb_pack::msg_t::merge_vote_abort:
            version = updated_version;
            resp_ok = false;
            break;

        default:
            log::logstring("unwanted header type ");
            resp_ok = false;
            break;
        }
        //log::logstring("resp_ok set and return");
        return resp_ok;
    }

    void start_check_version_ok(std::shared_ptr<pack::key_t> const name, std::size_t partition,
                                std::uint32_t version,
                                std::function<void(bool)> completeion_handler) override
    {
        // request commit
        log::logstring("storage-ssbd.hpp check version start");

        rocksdb_pack::packet_pointer ptr = std::make_shared<rocksdb_pack::packet>();
        ptr->header.type = rocksdb_pack::msg_t::merge_request_commit;
        ptr->header.uuid = *name;
        ptr->header.blockid = partition;

        std::remove_reference_t<decltype(version)> bigendian_version = rocksdb_pack::hton(version);
        ptr->data.buf.resize(sizeof(bigendian_version));

        std::vector<rocksdb_pack::unit_t> b (sizeof(bigendian_version));
        std::swap(ptr->data.buf, b);
        std::memcpy(ptr->data.buf.data(), &bigendian_version, sizeof(bigendian_version));

        auto buf = ptr->serialize();

        boost::asio::async_write(
            socket_,
            boost::asio::buffer(buf->data(), buf->size()),
            boost::asio::bind_executor(
                write_strand_,
                [buf, version, handler=std::move(completeion_handler), this] (boost::system::error_code const&, std::size_t) {
                    start_check_version_ok_read_response(version, handler);
                })
            );
    };

    void start_check_version_ok_read_response(std::uint32_t version, std::function<void(bool)> next)
    {
        // read resp
        rocksdb_pack::packet_pointer resp = std::make_shared<rocksdb_pack::packet>();
        auto headerbuf = std::make_shared<std::vector<rocksdb_pack::unit_t>>(rocksdb_pack::packet_header::bytesize);

        log::logstring("storage-ssbd.hpp check version read");

        boost::asio::async_read(
            socket_,
            boost::asio::buffer(headerbuf->data(), headerbuf->size()),
            boost::asio::bind_executor(
                write_strand_,
                [this, resp, version, headerbuf, next=std::move(next)] (boost::system::error_code const&, std::size_t) {
                    resp->header.parse(headerbuf->data());

                    auto updated_version = std::make_shared<std::remove_reference_t<decltype(version)>>();
                    assert(resp->header.datasize == sizeof(*updated_version));

                    boost::asio::async_read(
                        socket_,
                        boost::asio::buffer(updated_version.get(), sizeof(*updated_version)),
                        boost::asio::bind_executor(
                            write_strand_,
                            [this, resp, updated_version, next=std::move(next)] (boost::system::error_code const&, std::size_t) {
                                *updated_version = rocksdb_pack::hton(*updated_version);

                                bool resp_ok = true;
                                switch (resp->header.type)
                                {
                                case rocksdb_pack::msg_t::merge_vote_agree:
                                    resp_ok = true;
                                    break;

                                case rocksdb_pack::msg_t::merge_vote_abort:
                                    resp_ok = false;
                                    break;

                                default:
                                    log::logstring("unwanted header type ");
                                    resp_ok = false;
                                    break;
                                }
                                std::invoke(next, resp_ok);
                            }));
                }));
    }



    void write_key(pack::key_t const& name, std::size_t partition,
                   base::buf const& buffer, std::size_t location,
                   std::uint32_t version) override
    {
        rocksdb_pack::packet_pointer ptr = std::make_shared<rocksdb_pack::packet>();
        ptr->header.type = rocksdb_pack::msg_t::merge_execute_commit;
        std::copy(name.begin(), name.end(), ptr->header.uuid.begin());
        ptr->header.blockid = partition;
        ptr->header.position = location;

        version = rocksdb_pack::hton(version);
        ptr->data.buf = std::vector<rocksdb_pack::unit_t> (sizeof(version) + buffer.size());
        std::memcpy(ptr->data.buf.data(), &version, sizeof(version));
        std::copy(buffer.begin(), buffer.end(),
                  std::next(ptr->data.buf.begin(), sizeof(version)));

        auto buf = ptr->serialize();
        boost::asio::write(socket_, boost::asio::buffer(buf->data(), buf->size()));

        { // read resp

            rocksdb_pack::packet_pointer resp = std::make_shared<rocksdb_pack::packet>();
            std::vector<rocksdb_pack::unit_t> headerbuf(rocksdb_pack::packet_header::bytesize);

            boost::asio::read(socket_, boost::asio::buffer(headerbuf.data(), headerbuf.size()));

            resp->header.parse(headerbuf.data());

        } // read resp
    }

    void start_write_key(std::shared_ptr<pack::key_t> const name, std::size_t partition,
                         std::shared_ptr<base::buf> const buffer, std::size_t location,
                         std::uint32_t version,
                         std::function<void(base::buf)> completeion_handler)
    {
        rocksdb_pack::packet_pointer ptr = std::make_shared<rocksdb_pack::packet>();
        ptr->header.type = rocksdb_pack::msg_t::merge_execute_commit;
        std::copy(name->begin(), name->end(), ptr->header.uuid.begin());
        ptr->header.blockid = partition;
        ptr->header.position = location;

        version = rocksdb_pack::hton(version);
        ptr->data.buf = std::vector<rocksdb_pack::unit_t> (sizeof(version) + buffer->size());
        std::memcpy(ptr->data.buf.data(), &version, sizeof(version));
        std::copy(buffer->begin(), buffer->end(),
                  std::next(ptr->data.buf.begin(), sizeof(version)));

        auto buf = ptr->serialize();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(buf->data(), buf->size()),
            boost::asio::bind_executor(
                write_strand_,
                [this, buf, handler=std::move(completeion_handler)] (boost::system::error_code const&, std::size_t) {
                    start_read_response(handler);
                })
            );

//        { // read resp
//            rocksdb_pack::packet_pointer resp = std::make_shared<rocksdb_pack::packet>();
//            std::vector<rocksdb_pack::unit_t> headerbuf(rocksdb_pack::packet_header::bytesize);
//            boost::asio::read(socket_,
//                              boost::asio::buffer(headerbuf.data(), headerbuf.size()));
//
//            resp->header.parse(headerbuf.data());
//            auto bodybuf = std::make_unique<base::buf>(resp->header.datasize, 0);
//
//            boost::asio::async_read(
//                socket_,
//                boost::asio::buffer(bodybuf->data(), bodybuf->size()),
//                [bodybuf = std::move(bodybuf)] (boost::system::error_code const&, std::size_t) {
//                    std::invoke (completeion_handler, *bodybuf);
//                });
//        } // read resp
    };

    void append_list_key(pack::key_t const& name, base::buf const& buffer) override
    {
    }

    void merge_list_key(pack::key_t const& name, std::function<void(std::vector<base::buf> const&)> reduce) override
    {
    }

    auto  get_list_key(pack::key_t const& name) -> base::buf override
    {
        return {};
    }
};

} // namespace storage

#endif // SLSFS_STORAGE_SSBD_HPP__
