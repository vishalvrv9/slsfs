#include "basic.hpp"
#include "leveldb-serializer.hpp"
#include "rawblocks.hpp"
#include "socket-writer.hpp"

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

namespace ssbd
{

class tcp_connection : public std::enable_shared_from_this<tcp_connection>
{
    net::io_context& io_context_;
    tcp::socket      socket_;

    slsfs::socket_writer::socket_writer<leveldb_pack::packet, std::vector<leveldb_pack::unit_t>> writer_;
    leveldb::DB& db_;

public:
    using pointer = std::shared_ptr<tcp_connection>;

    tcp_connection(net::io_context& io, tcp::socket socket, leveldb::DB& db):
        io_context_{io},
        socket_{std::move(socket)},
        writer_{io, socket_},
        db_{db} {}

    void start_read_header()
    {
        BOOST_LOG_TRIVIAL(trace) << "start_read_header";
        auto read_buf = std::make_shared<std::array<leveldb_pack::unit_t, leveldb_pack::packet_header::bytesize>>();
        net::async_read(
            socket_,
            net::buffer(read_buf->data(), read_buf->size()),
            [self=shared_from_this(), read_buf] (boost::system::error_code ec, std::size_t /*length*/) {
                if (ec)
                    BOOST_LOG_TRIVIAL(error) << "start_read_header err: " << ec.message();
                else
                {
                    leveldb_pack::packet_pointer pack = std::make_shared<leveldb_pack::packet>();
                    pack->header.parse(read_buf->data());
                    BOOST_LOG_TRIVIAL(trace) << "start_read_header start with header: " << pack->header;

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
                if (ec)
                    BOOST_LOG_TRIVIAL(error) << "start_check_merge: " << ec.message();
                else
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
                if (ec)
                    BOOST_LOG_TRIVIAL(error) << "error start_execute_commit: " << ec.message();
                else
                {
                    pack->data.parse(length, read_buf->data());
                    std::string const key = pack->header.as_string();
                    self->db_.Put(leveldb::WriteOptions(), key, *read_buf);

                    leveldb_pack::packet_pointer resp = std::make_shared<leveldb_pack::packet>();
                    resp->header = pack->header;
                    resp->header.type = leveldb_pack::msg_t::ack;

                    self->start_write_socket(resp);
                    self->start_read_header();
                }
            });
    }

    void start_db_write(leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_db_write";
        net::post(
            io_context_,
            [self=shared_from_this(), pack] {
                std::string value;
                std::string const key = pack->header.as_string();

                self->db_.Get(leveldb::ReadOptions(), key, &value);
                std::copy(pack->data.buf.begin(), pack->data.buf.end(), std::next(value.begin(), pack->header.position));
                self->db_.Put(leveldb::WriteOptions(), key, value);

                leveldb_pack::packet_pointer resp = std::make_shared<leveldb_pack::packet>();
                resp->header = pack->header;
                resp->header.type = leveldb_pack::msg_t::ack;
                resp->data.buf = std::vector<leveldb_pack::unit_t>{'O', 'K'};
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
                leveldb::Status status = rb.bind(self->db_, pack->header.as_string());

                leveldb_pack::packet_pointer resp = std::make_shared<leveldb_pack::packet>();
                resp->header = pack->header;
                if (status.ok())
                {
                    resp->header.type = leveldb_pack::msg_t::ack;

                    std::vector<leveldb_pack::unit_t> buf(resp->header.datasize);

                    rb.read(pack->header.position, buf.begin(), resp->header.datasize);

                    resp->data.buf.swap(buf);
                }
                else
                {
                    resp->header.type = leveldb_pack::msg_t::err;
                }


                //self->start_write_socket(resp);
            });
    }

    void start_write_socket(leveldb_pack::packet_pointer pack)
    {
        auto next = std::make_shared<slsfs::socket_writer::boost_callback>(
            [self=shared_from_this(), pack] (boost::system::error_code ec, std::size_t transferred_size) {
                if (ec)
                {
                    BOOST_LOG_TRIVIAL(error) << "write error " << ec.message() << " while " << pack->header << " size=" << transferred_size;
                    return;
                }

                BOOST_LOG_TRIVIAL(trace) << "write sent " << transferred_size << " bytes, count=" << self.use_count() << " " << pack->header;
            });

        writer_.start_write_socket(pack, next);
    }
};

class tcp_server
{
    net::io_context& io_context_;
    tcp::acceptor acceptor_;
    std::shared_ptr<leveldb::DB> db_ = nullptr;

public:
    tcp_server(net::io_context& io_context, net::ip::port_type port, char const * dbname)
        : io_context_(io_context),
          acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
    {
        leveldb::DB* db = nullptr;
        leveldb::Options options;
        options.create_if_missing = true;
        leveldb::Status status = leveldb::DB::Open(options, dbname, &db);
        if (not status.ok())
        {
            BOOST_LOG_TRIVIAL(error) << status.ToString() << "\n";
            throw std::runtime_error("cannot open db");
        }

        BOOST_LOG_TRIVIAL(debug) << "open db ptr: " << db << "\n";
        db_.reset(db);
        start_accept();
    }

    void start_accept()
    {
        acceptor_.async_accept(
            [this] (boost::system::error_code const& error, tcp::socket socket) {
                if (error)
                    BOOST_LOG_TRIVIAL(error) << "accept error: " << error.message() << "\n";
                else
                {
                    socket.set_option(tcp::no_delay(true));
                    auto accepted = std::make_shared<tcp_connection>(
                        io_context_,
                        std::move(socket),
                        *db_);
                    accepted->start_read_header();
                    start_accept();
                }
            });
    }
};

} // namespace ssbd

int main(int argc, char* argv[])
{
    ssbd::basic::init_log();

    namespace po = boost::program_options;
    po::options_description desc{"Options"};
    desc.add_options()
        ("help,h", "Print this help messages")
        ("listen,l", po::value<unsigned short>()->default_value(12000), "listen on this port")
        ("blocksize,b", po::value<std::size_t>()->default_value(4 * 1024), "set block size (in bytes)");
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
    ssbd::net::io_context ioc {worker};
    ssbd::net::signal_set listener(ioc, SIGINT, SIGTERM);
    listener.async_wait(
        [&ioc](boost::system::error_code const&, int signal_number) {
            BOOST_LOG_TRIVIAL(info) << "Stopping... sig=" << signal_number;
            ioc.stop();
        });

    unsigned short const port = vm["listen"].as<unsigned short>();
    std::size_t    const size = vm["blocksize"].as<std::size_t>();

    leveldb_pack::rawblocks {}.fullsize() = size;

    ssbd::tcp_server server{ioc, port, "/tmp/haressbd/db.db"};
    BOOST_LOG_TRIVIAL(info) << "listen on " << port << " block size=" << size;

    std::vector<std::thread> v;
    v.reserve(worker);
    for(int i = 1; i < worker; i++)
        v.emplace_back([&ioc] { ioc.run(); });
    ioc.run();

    for (std::thread& th : v)
        th.join();

    return EXIT_SUCCESS;
}
