#include "basic.hpp"
#include "leveldb-serializer.hpp"
#include "rawblocks.hpp"

#include <boost/program_options.hpp>
#include <boost/log/trivial.hpp>
#include <boost/asio.hpp>
#include <boost/signals2.hpp>

#include <oneapi/tbb/concurrent_unordered_map.h>
#include <oneapi/tbb/concurrent_queue.h>

#include <leveldb/db.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <array>
#include <list>
#include <thread>
#include <vector>

using net::ip::tcp;

//class ssbd_merger : public leveldb::AssociativeMergeOperator
//{
//public:
//    virtual
//    bool Merge(leveldb::Slice const& key,
//               leveldb::Slice const* existing_value,
//               leveldb::Slice const& value,
//               std::string* new_value,
//               Logger* logger) const override = 0;
//    {
//        return true;
//    }
//
//    virtual const char* Name() const override { return "ssbd-merge"; }
//};

class tcp_connection : public std::enable_shared_from_this<tcp_connection>
{
    net::io_context& io_context_;
    tcp::socket socket_;
    net::io_context::strand write_io_strand_;
    std::shared_ptr<leveldb::DB> db_;

public:
    using pointer = std::shared_ptr<tcp_connection>;

    tcp_connection(net::io_context& io, tcp::socket socket, std::shared_ptr<leveldb::DB> db):
        io_context_{io},
        socket_{std::move(socket)},
        write_io_strand_{io},
        db_{db} {}

    void start_read_header()
    {
        BOOST_LOG_TRIVIAL(trace) << "start_read_header";
        auto read_buf = std::make_shared<std::array<leveldb_pack::unit_t, leveldb_pack::packet_header::bytesize>>();
        net::async_read(
            socket_,
            net::buffer(read_buf->data(), read_buf->size()),
            [self=shared_from_this(), read_buf] (boost::system::error_code ec, std::size_t /*length*/) {
                if (not ec)
                {
                    leveldb_pack::packet_pointer pack = std::make_shared<leveldb_pack::packet>();
                    pack->header.parse(read_buf->data());

                    switch (pack->header.type)
                    {
                    case leveldb_pack::msg_t::merge_request_commit:
                        BOOST_LOG_TRIVIAL(debug) << "merge_request_commit " << pack->header;
                        self->start_check_merge(pack);
                        break;

                    case leveldb_pack::msg_t::merge_execute_commit:
                        BOOST_LOG_TRIVIAL(debug) << "merge_execute_commit " << pack->header;
                        self->start_execute_commit(pack);
                        break;

                    case leveldb_pack::msg_t::merge_rollback_commit:
                        BOOST_LOG_TRIVIAL(debug) << "merge_rollback_commit " << pack->header;
                        self->start_read_header();
                        break;

                    case leveldb_pack::msg_t::get:
                        BOOST_LOG_TRIVIAL(debug) << "get " << pack->header;
                        self->start_db_read(pack);
                        self->start_read_header();
                        break;

                    case leveldb_pack::msg_t::err:
                    case leveldb_pack::msg_t::ack:
                    case leveldb_pack::msg_t::merge_ack_commit:
                    case leveldb_pack::msg_t::merge_vote_agree:
                    case leveldb_pack::msg_t::merge_vote_abort:
                    {
                        BOOST_LOG_TRIVIAL(error) << "server should not get (" << pack->header.type << "). error: " << pack->header;
                        leveldb_pack::packet_pointer resp = std::make_shared<leveldb_pack::packet>();
                        resp->header = pack->header;
                        resp->header.type = leveldb_pack::msg_t::err;
                        self->start_write_socket(resp);
                        self->start_read_header();
                        break;
                    }
                    }
                }
                else
                {
                    if (ec != boost::asio::error::eof)
                        BOOST_LOG_TRIVIAL(error) << ", start_read_header err: " << ec.message();
                }
            });
    }

    void start_check_merge(leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_check_merge " << pack->header;
        auto read_buf = std::make_shared<std::vector<leveldb_pack::unit_t>>(pack->header.datasize);
        net::async_read(
            socket_,
            net::buffer(read_buf->data(), read_buf->size()),
            [self=shared_from_this(), read_buf, pack] (boost::system::error_code ec, std::size_t length) {
                if (not ec)
                {
                    pack->data.parse(length, read_buf->data());

                    leveldb_pack::packet_pointer resp = std::make_shared<leveldb_pack::packet>();
                    resp->header = pack->header;

                    std::string const key = pack->header.as_string();
                    leveldb_pack::rawblocks rb;

                    resp->header.type = leveldb_pack::msg_t::merge_vote_agree;
                    if (rb.bind(self->db_, key).ok())
                    {
                        leveldb_pack::rawblocks::versionint_t requested_version;
                        std::memcpy(&requested_version, pack->data.buf.data(), sizeof(requested_version));
                        requested_version = leveldb_pack::ntoh(requested_version);
                        if (rb.version() > requested_version && false /*debug*/)
                            resp->header.type = leveldb_pack::msg_t::merge_vote_abort;
                        BOOST_LOG_TRIVIAL(debug) << "local: " << rb.version() << " req: " << requested_version;
                    }
                    resp->data.buf = pack->data.buf;

                    self->start_write_socket(resp);
                    self->start_read_header();
                }
                else
                    BOOST_LOG_TRIVIAL(error) << "start_check_merge: " << ec.message();
            });
    }

    void start_execute_commit(leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_execute_commit " << pack->header;
        auto read_buf = std::make_shared<std::string>(pack->header.datasize, 0);
        net::async_read(
            socket_,
            net::buffer(read_buf->data(), read_buf->size()),
            [self=shared_from_this(), read_buf, pack] (boost::system::error_code ec, std::size_t length) {
                if (not ec)
                {
                    pack->data.parse(length, read_buf->data());
                    std::string const key = pack->header.as_string();
                    self->db_->Put(leveldb::WriteOptions(), key, *read_buf);

                    leveldb_pack::packet_pointer resp = std::make_shared<leveldb_pack::packet>();
                    resp->header = pack->header;
                    resp->header.type = leveldb_pack::msg_t::ack;

                    self->start_write_socket(resp);
                    self->start_read_header();
                }
                else
                    BOOST_LOG_TRIVIAL(error) << "start_execute_commit: " << ec.message();
            });
    }

    void start_db_write(leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_db_write";
        net::post(
            io_context_,
            [self=shared_from_this(), pack] {
                std::string value;
                //resp->buf;
                std::string const key = pack->header.as_string();

                self->db_->Get(leveldb::ReadOptions(), key, &value);
                std::copy(pack->data.buf.begin(), pack->data.buf.end(), std::next(value.begin(), pack->header.position));
                self->db_->Put(leveldb::WriteOptions(), key, value);

                leveldb_pack::packet_pointer resp = std::make_shared<leveldb_pack::packet>();
                resp->header = pack->header;
                resp->header.type = leveldb_pack::msg_t::ack;
                self->start_write_socket(resp);
            });
    }

    void start_db_read(leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_db_read";
        net::post(
            io_context_,
            [self=shared_from_this(), pack] {
                leveldb_pack::rawblocks rb;
                rb.bind(self->db_, pack->header.as_string());

                leveldb_pack::packet_pointer resp = std::make_shared<leveldb_pack::packet>();
                resp->header = pack->header;
                resp->header.type = leveldb_pack::msg_t::ack;

                rb.read(pack->header.position, std::back_inserter(resp->data.buf), resp->header.datasize);

                self->start_write_socket(resp);
            });
    }

    void start_write_socket(leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_write_socket: " << pack->header;
        auto buf_pointer = pack->serialize();

        std::stringstream ss;
        ss << "[ ";
        for (int i : *buf_pointer)
            ss << i << " ";
        BOOST_LOG_TRIVIAL(trace) << ss.str() << "]";

        net::async_write(
            socket_,
            net::buffer(buf_pointer->data(), buf_pointer->size()),
            net::bind_executor(
                write_io_strand_,
                [self=shared_from_this(), buf_pointer] (boost::system::error_code ec, std::size_t /*length*/) {
                    if (not ec)
                        BOOST_LOG_TRIVIAL(debug) << "sent msg";
                }));
    }
};

class tcp_server
{
    net::io_context& io_context_;
    tcp::acceptor acceptor_;
    std::shared_ptr<leveldb::DB> db_;

public:
    tcp_server(net::io_context& io_context, net::ip::port_type port, char const * dbname)
        : io_context_(io_context),
          acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
    {
        leveldb::DB* db;
        leveldb::Options options;
        options.create_if_missing = true;
        leveldb::Status status = leveldb::DB::Open(options, dbname, &db);
        if (not status.ok())
            BOOST_LOG_TRIVIAL(error) << status.ToString() << "\n";

        db_.reset(db);
        start_accept();
    }

    void start_accept()
    {
        acceptor_.async_accept(
            [this] (boost::system::error_code const& error, tcp::socket socket) {
                if (not error)
                {
                    socket.set_option(tcp::no_delay(true));
                    auto accepted = std::make_shared<tcp_connection>(
                        io_context_,
                        std::move(socket),
                        db_);
                    accepted->start_read_header();
                    start_accept();
                }
            });
    }
};

int main(int argc, char* argv[])
{
    basic::init_log();

    namespace po = boost::program_options;
    po::options_description desc{"Options"};
    desc.add_options()
        ("help,h", "Print this help messages")
        ("listen,l", po::value<unsigned short>()->default_value(12000), "listen on this port");
    po::positional_options_description pos_po;
    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv)
              .options(desc)
              .positional(pos_po).run(), vm);
    po::notify(vm);

    if (vm.count("help"))
    {
        BOOST_LOG_TRIVIAL(info) << desc;
        return EXIT_FAILURE;
    }

    int const worker = std::thread::hardware_concurrency();
    net::io_context ioc {worker};
    net::signal_set listener(ioc, SIGINT, SIGTERM);
    listener.async_wait(
        [&ioc](boost::system::error_code const&, int signal_number) {
            BOOST_LOG_TRIVIAL(info) << "Stopping... sig=" << signal_number;
            ioc.stop();
        });

    unsigned short const port = vm["listen"].as<unsigned short>();

    tcp_server server{ioc, port, "/tmp/haressbd/db.db"};
    BOOST_LOG_TRIVIAL(info) << "listen on " << port;

    std::vector<std::thread> v;
    v.reserve(worker);
    for(int i = 1; i < worker; i++)
        v.emplace_back([&ioc] { ioc.run(); });
    ioc.run();

    for (std::thread& th : v)
        th.join();

    return EXIT_SUCCESS;
}
