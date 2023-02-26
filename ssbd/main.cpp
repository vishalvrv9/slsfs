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

    slsfs::socket_writer::socket_writer<slsfs::leveldb_pack::packet, std::vector<slsfs::leveldb_pack::unit_t>> writer_;
    leveldb::DB& db_, &db_log_;

public:
    using pointer = std::shared_ptr<tcp_connection>;

    tcp_connection(net::io_context& io, tcp::socket socket, leveldb::DB& db, leveldb::DB& db_log):
        io_context_{io},
        socket_{std::move(socket)},
        writer_{io, socket_},
        db_{db}, db_log_{db_log} {}

    void start_read_header()
    {
        BOOST_LOG_TRIVIAL(trace) << "start_read_header";
        auto read_buf = std::make_shared<std::array<slsfs::leveldb_pack::unit_t, slsfs::leveldb_pack::packet_header::bytesize>>();
        net::async_read(
            socket_,
            net::buffer(read_buf->data(), read_buf->size()),
            [self=shared_from_this(), read_buf] (boost::system::error_code ec, std::size_t /*length*/) {
                if (ec)
                    BOOST_LOG_TRIVIAL(error) << "start_read_header err: " << ec.message();
                else
                {
                    slsfs::leveldb_pack::packet_pointer pack = std::make_shared<slsfs::leveldb_pack::packet>();
                    pack->header.parse(read_buf->data());
                    BOOST_LOG_TRIVIAL(trace) << "start_read_header start with header: " << pack->header;

                    switch (pack->header.type)
                    {
                    case slsfs::leveldb_pack::msg_t::two_pc_prepare:
                        BOOST_LOG_TRIVIAL(debug) << "two_pc_prepare " << pack->header;
                        self->start_two_pc_prepare(pack);
                        break;

                    case slsfs::leveldb_pack::msg_t::two_pc_commit_execute:
                        BOOST_LOG_TRIVIAL(debug) << "two_pc_commit_execute " << pack->header;
                        self->start_two_pc_commit_execute(pack);
                        break;

                    case slsfs::leveldb_pack::msg_t::two_pc_commit_rollback:
                        BOOST_LOG_TRIVIAL(debug) << "two_pc_commit_rollback " << pack->header;
                        self->start_two_pc_commit_rollback(pack);
                        break;

                    case slsfs::leveldb_pack::msg_t::replication:
                        BOOST_LOG_TRIVIAL(debug) << "replication " << pack->header;
                        self->start_replication(pack);
                        break;

                    case slsfs::leveldb_pack::msg_t::get:
                        BOOST_LOG_TRIVIAL(debug) << "get " << pack->header;
                        self->start_db_read(pack);
                        break;

                    case slsfs::leveldb_pack::msg_t::err:
                    case slsfs::leveldb_pack::msg_t::ack:
                    case slsfs::leveldb_pack::msg_t::two_pc_commit_ack:
                    case slsfs::leveldb_pack::msg_t::two_pc_prepare_agree:
                    case slsfs::leveldb_pack::msg_t::two_pc_prepare_abort:
                    {
                        BOOST_LOG_TRIVIAL(error) << "server should not get (" << pack->header.type << "). " << pack->header;
                        slsfs::leveldb_pack::packet_pointer resp = std::make_shared<slsfs::leveldb_pack::packet>();
                        resp->header = pack->header;
                        resp->header.type = slsfs::leveldb_pack::msg_t::err;
                        self->start_write_socket(resp);
                        self->start_read_header();
                        break;
                    }
                    }
                }
            });
    }

    void start_two_pc_prepare(slsfs::leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_two_pc_prepare " << pack->header;
        auto read_buf = std::make_shared<std::string>(pack->header.datasize, 0);
        net::async_read(
            socket_,
            net::buffer(read_buf->data(), read_buf->size()),
            [self=shared_from_this(), read_buf, pack] (boost::system::error_code ec, std::size_t length) {
                if (ec)
                    BOOST_LOG_TRIVIAL(error) << "start_two_pc_prepare: " << ec.message();
                else
                {
                    BOOST_LOG_TRIVIAL(trace) << "start_two_pc_prepare process packet: " << pack->header;

                    pack->data.parse(length, read_buf->data());

                    slsfs::leveldb_pack::packet_pointer resp = std::make_shared<slsfs::leveldb_pack::packet>();
                    resp->header = pack->header;

                    std::string const key = pack->header.as_string();
                    slsfs::leveldb_pack::rawblocks rb;

                    resp->header.type = slsfs::leveldb_pack::msg_t::two_pc_prepare_agree;
                    if (rb.bind(self->db_, key).ok())
                    {
                        slsfs::leveldb_pack::rawblocks::versionint_t requested_version = 0;
                        std::memcpy(&requested_version, pack->data.buf.data(), sizeof(requested_version));
                        requested_version = slsfs::leveldb_pack::ntoh(requested_version);
                        if (rb.version() > requested_version)
                            resp->header.type = slsfs::leveldb_pack::msg_t::two_pc_prepare_abort;

                        std::string log_entry;
                        self->db_log_.Get(leveldb::ReadOptions(), key, &log_entry);
                        if (not log_entry.empty())
                        {
                            BOOST_LOG_TRIVIAL(error) << "Abort for prepare request due to non empty log";
                            resp->header.type = slsfs::leveldb_pack::msg_t::two_pc_prepare_abort;
                        }

                        BOOST_LOG_TRIVIAL(debug) << "start_two_pc_prepare local: " << rb.version()
                                                 << " req: " << requested_version;

                        std::memcpy(&requested_version, pack->data.buf.data(), sizeof(requested_version));
                        slsfs::leveldb_pack::rawblocks::versionint_t sendversion = rb.version();
                        sendversion = slsfs::leveldb_pack::ntoh(sendversion);
                        std::memcpy(pack->data.buf.data(), &sendversion, sizeof(sendversion));
                    }

                    self->db_log_.Put(leveldb::WriteOptions(), key, *read_buf);
                    BOOST_LOG_TRIVIAL(trace) << "start_two_pc_prepare return packet: " << pack->header;
                    self->start_write_socket(resp);
                    self->start_read_header();
                }
            });
    }

    void start_two_pc_commit_execute(slsfs::leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_two_pc_commit_execute " << pack->header;
        auto read_buf = std::make_shared<std::string>(pack->header.datasize, 0);
        net::async_read(
            socket_,
            net::buffer(read_buf->data(), read_buf->size()),
            [self=shared_from_this(), read_buf, pack] (boost::system::error_code ec, std::size_t length) {
                if (ec)
                    BOOST_LOG_TRIVIAL(error) << "error start_two_pc_commit_execute: " << ec.message();
                else
                {
                    pack->data.parse(length, read_buf->data());
                    std::string const key = pack->header.as_string();

                    std::string value;
                    self->db_log_.Get(leveldb::ReadOptions(), key, &value);
                    self->db_.Put(leveldb::WriteOptions(), key, value);
                    self->db_log_.Put(leveldb::WriteOptions(), key, "");

                    slsfs::leveldb_pack::packet_pointer resp = std::make_shared<slsfs::leveldb_pack::packet>();
                    resp->header = pack->header;
                    resp->header.type = slsfs::leveldb_pack::msg_t::two_pc_commit_ack;

                    self->start_write_socket(resp);
                    self->start_read_header();
                }
            });
    }

    void start_two_pc_commit_rollback(slsfs::leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_two_pc_commit_rollback " << pack->header;
        std::string const key = pack->header.as_string();
        db_log_.Put(leveldb::WriteOptions(), key, "");

        slsfs::leveldb_pack::packet_pointer resp = std::make_shared<slsfs::leveldb_pack::packet>();
        resp->header = pack->header;
        resp->header.type = slsfs::leveldb_pack::msg_t::two_pc_commit_ack;

        start_write_socket(resp);
        start_read_header();
    }

    void start_replication(slsfs::leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_replication " << pack->header;
        auto read_buf = std::make_shared<std::string>(pack->header.datasize, 0);
        net::async_read(
            socket_,
            net::buffer(read_buf->data(), read_buf->size()),
            [self=shared_from_this(), read_buf, pack] (boost::system::error_code ec, std::size_t length) {
                if (ec)
                    BOOST_LOG_TRIVIAL(error) << "start_replication: " << ec.message();
                else
                {
                    pack->data.parse(length, read_buf->data());

                    slsfs::leveldb_pack::packet_pointer resp = std::make_shared<slsfs::leveldb_pack::packet>();
                    resp->header = pack->header;
                    resp->header.type = slsfs::leveldb_pack::msg_t::ack;

                    std::string const key = pack->header.as_string();

                    self->db_.Put(leveldb::WriteOptions(), key, *read_buf);
                    self->start_write_socket(resp);
                    self->start_read_header();
                }
            });
    }


    void start_db_write(slsfs::leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_db_write";
        net::post(
            io_context_,
            [self=shared_from_this(), pack] {
                std::string value;
                std::string const key = pack->header.as_string();

                self->db_.Get(leveldb::ReadOptions(), key, &value);
                std::copy(pack->data.buf.begin(), pack->data.buf.end(),
                          std::next(value.begin(), pack->header.position));
                self->db_.Put(leveldb::WriteOptions(), key, value);

                slsfs::leveldb_pack::packet_pointer resp = std::make_shared<slsfs::leveldb_pack::packet>();
                resp->header = pack->header;
                resp->header.type = slsfs::leveldb_pack::msg_t::ack;
                resp->data.buf = std::vector<slsfs::leveldb_pack::unit_t>{'O', 'K'};
                self->start_write_socket(resp);
            });
    }

    void start_db_read(slsfs::leveldb_pack::packet_pointer pack)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_db_read";
        net::post(
            io_context_,
            [self=shared_from_this(), pack] {
                slsfs::leveldb_pack::rawblocks rb;
                leveldb::Status status = rb.bind(self->db_, pack->header.as_string());

                slsfs::leveldb_pack::packet_pointer resp = std::make_shared<slsfs::leveldb_pack::packet>();
                resp->header = pack->header;
                if (status.ok())
                {
                    resp->header.type = slsfs::leveldb_pack::msg_t::ack;

                    std::vector<slsfs::leveldb_pack::unit_t> buf(resp->header.datasize);

                    rb.read(pack->header.position, buf.begin(), resp->header.datasize);

                    resp->data.buf.swap(buf);
                }
                else
                    resp->header.type = slsfs::leveldb_pack::msg_t::err;

                self->start_write_socket(resp);
                self->start_read_header();
            });
    }

    void start_write_socket(slsfs::leveldb_pack::packet_pointer pack)
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
    std::unique_ptr<leveldb::DB> db_ = nullptr, db_log_ = nullptr;

public:
    tcp_server(net::io_context& io_context, net::ip::port_type port, char const * dbname)
        : io_context_(io_context),
          acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
    {
        { // init db_
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
        }
        { // init db_log_
            leveldb::DB* db = nullptr;
            leveldb::Options options;
            options.create_if_missing = true;
            std::string const dblog = std::string(dbname) + "_log";
            leveldb::Status status = leveldb::DB::Open(options, dblog, &db);
            if (not status.ok())
            {
                BOOST_LOG_TRIVIAL(error) << status.ToString() << "\n";
                throw std::runtime_error("cannot open db");
            }

            BOOST_LOG_TRIVIAL(debug) << "open db log ptr: " << db << "\n";
            db_log_.reset(db);
        }
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
                        *db_,
                        *db_log_);
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

    slsfs::leveldb_pack::rawblocks {}.fullsize() = size;

    ssbd::tcp_server server{ioc, port, "/tmp/haressbd/db.db"};
    BOOST_LOG_TRIVIAL(info) << "listen on " << port << " block size=" << size;
    BOOST_LOG_TRIVIAL(trace) << "trace enabled";

    std::vector<std::thread> v;
    v.reserve(worker);
    for(int i = 1; i < worker; i++)
        v.emplace_back([&ioc] { ioc.run(); });
    ioc.run();

    for (std::thread& th : v)
        th.join();

    return EXIT_SUCCESS;
}
