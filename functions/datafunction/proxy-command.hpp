#pragma once

#ifndef PROXY_COMMAND_HPP__
#define PROXY_COMMAND_HPP__

#include <oneapi/tbb/concurrent_hash_map.h>
#include <boost/signals2.hpp>

#include <slsfs.hpp>

namespace slsfsdf::server
{

namespace
{
    using boost::asio::ip::tcp;
    using namespace std::chrono_literals;
}

using queue_map = oneapi::tbb::concurrent_hash_map<slsfs::uuid::uuid,
                                                   std::shared_ptr<boost::asio::io_context::strand>,
                                                   slsfs::uuid::hash_compare>;
using queue_map_accessor = queue_map::accessor;

//std::invocable<slsfs::base::buf(slsfsdf::storage_conf*, jsre::request_parser<base::byte> const&)>;
template<typename Func>
concept StorageOperationConcept = requires(Func func)
{
    { std::invoke(func,
                  std::declval<slsfsdf::storage_conf*>(),
                  std::declval<slsfs::jsre::request_parser<slsfs::base::byte> const&>()) }
    -> std::convertible_to<slsfs::base::buf>;
};

class proxy_command;

using proxy_set = oneapi::tbb::concurrent_hash_map<std::shared_ptr<proxy_command>, int /* unused */>;

class proxy_command : public std::enable_shared_from_this<proxy_command>
{
    boost::asio::io_context&     io_context_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer    recv_deadline_;

    using time_point = std::chrono::time_point<std::chrono::steady_clock>;
    static auto now() -> time_point { return std::chrono::steady_clock::now(); }
    time_point                last_update_ = now();
    std::chrono::milliseconds waittime_ = 10s;

    std::shared_ptr<storage_conf> datastorage_conf_;
    slsfs::socket_writer::socket_writer<slsfs::pack::packet, std::vector<slsfs::pack::unit_t>> writer_;

    queue_map& queue_map_;
    proxy_set& proxy_set_;

    static
    auto log_timer(std::chrono::steady_clock::time_point now) -> std::string
    {
        // Convert the time point to a duration since the epoch
        std::chrono::duration<double> time_since_epoch = now.time_since_epoch();

        // Convert the duration to a number of seconds
        double seconds = time_since_epoch.count();

        // Convert the number of seconds to a time_t object
        std::time_t time = std::time_t(seconds);

        // Convert the time_t object to a tm object
        std::tm tm = *std::gmtime(&time);

        // Create a stringstream and use put_time to format the tm object
        std::stringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

        // Get the string from the stringstream
        return ss.str();
    }

    void timer_reset()
    {
        slsfs::log::log("timer_reset");
        recv_deadline_.cancel();

        std::chrono::steady_clock::time_point timeout_time = last_update_ + waittime_;
        recv_deadline_.expires_at(timeout_time);
        recv_deadline_.async_wait(
            [self=this->shared_from_this()] (boost::system::error_code ec) {
                switch (ec.value())
                {
                case boost::system::errc::success: // timer timeout
                {
                    self->close();
                    break;
                }
                case boost::system::errc::operation_canceled: // timer canceled
                    break;
                default:
                    slsfs::log::log<slsfs::log::level::error>("timer_reset: read header timeout. close connection()");
                    break;
                }
        });
    }

public:
    proxy_command(boost::asio::io_context& io_context,
                  std::shared_ptr<storage_conf> conf,
                  queue_map& qm,
                  proxy_set& ps)
        : io_context_{io_context},
          socket_{io_context_},
          recv_deadline_{io_context_},
          datastorage_conf_{conf},
          writer_{io_context_, socket_},
          queue_map_{qm},
          proxy_set_{ps} {}

    void close()
    {
        slsfs::pack::packet_pointer pack = std::make_shared<slsfs::pack::packet>();
        pack->header.type = slsfs::pack::msg_t::worker_dereg;
        start_write(
            pack,
            [self=shared_from_this()] (boost::system::error_code ec, std::size_t length) {
                self->socket_.shutdown(tcp::socket::shutdown_receive, ec);
                slsfs::log::log("timer_reset: send shutdown");
                std::exit(0);
            });
    }

    template<typename Endpoint>
    void start_connect(Endpoint endpoint)
    {
        boost::asio::async_connect(
            socket_,
            endpoint,
            [self=shared_from_this()] (boost::system::error_code const & ec, tcp::endpoint const& endpoint) {
                self->socket_.set_option(tcp::no_delay(true));
                if (ec)
                    slsfs::log::log("connect to proxy error: {}", ec.message());
                else
                {
                    slsfs::log::log("connected. start write and listen");
                    slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();
                    ptr->header.type = slsfs::pack::msg_t::worker_reg;
                    ptr->header.gen();

                    self->start_write(ptr);
                    self->start_listen_commands();
                    self->timer_reset();
                }
            });
    }

    void start_listen_commands()
    {
        slsfs::log::log("start_listen_commands called");
        auto readbuf = std::make_shared<std::array<slsfs::pack::unit_t, slsfs::pack::packet_header::bytesize>>();

        boost::asio::async_read(
            socket_, boost::asio::buffer(readbuf->data(), readbuf->size()),
            [self=this->shared_from_this(), readbuf] (boost::system::error_code ec, std::size_t /*length*/) {
                slsfs::log::log("start_listen_commands get cmd");

                if (not ec)
                {
                    slsfs::log::log("start_listen_commands cancel timer");

                    slsfs::log::log<slsfs::log::level::debug>("get cmd");
                    slsfs::pack::packet_pointer pack = std::make_shared<slsfs::pack::packet>();
                    pack->header.parse(readbuf->data());

                    if (pack->header.type != slsfs::pack::msg_t::set_timer)
                        self->recv_deadline_.cancel();

                    self->start_listen_commands_body(pack);
                }
                else
                    slsfs::log::log("error listen command {}", ec.message());
            });
    }

    void start_listen_commands_body(slsfs::pack::packet_pointer pack)
    {
        auto read_buf = std::make_shared<std::vector<slsfs::pack::unit_t>>(pack->header.datasize);
        boost::asio::async_read(
            socket_,
            boost::asio::buffer(read_buf->data(), read_buf->size()),
            [self=this->shared_from_this(), read_buf, pack] (boost::system::error_code ec, std::size_t length) {
                if (not ec)
                {
                    pack->data.parse(length, read_buf->data());

                    switch (pack->header.type)
                    {
                    case slsfs::pack::msg_t::proxyjoin:
                    {
                        slsfs::log::log("switch proxy master");
                        std::uint32_t addr;
                        std::memcpy(&addr, pack->data.buf.data(), sizeof(addr));
                        addr = slsfs::pack::ntoh(addr);
                        boost::asio::ip::address_v4 new_host{addr};
                        boost::asio::ip::port_type  new_port;
                        std::memcpy(&new_port, pack->data.buf.data() + 4, sizeof(new_port));
                        new_port = slsfs::pack::ntoh(new_port);

                        boost::asio::ip::tcp::endpoint ep{new_host, new_port};
                        {
                            std::stringstream ss;
                            ss << ep;
                            slsfs::log::log("try connect to {}", ss.str());
                        }

                        std::list<boost::asio::ip::tcp::endpoint> ep_list {ep};
                        auto proxy_command_ptr = std::make_shared<slsfsdf::server::proxy_command>(
                            self->io_context_,
                            self->datastorage_conf_,
                            self->queue_map_,
                            self->proxy_set_);

                        proxy_command_ptr->start_connect(ep_list);
                        self->proxy_set_.emplace(proxy_command_ptr, 0);
                        break;
                    }

                    case slsfs::pack::msg_t::set_timer:
                    {
                        slsfs::pack::waittime_type duration_in_ms = 0;
                        std::memcpy(&duration_in_ms, read_buf->data(), sizeof(slsfs::pack::waittime_type));
                        self->waittime_ = slsfs::pack::ntoh(duration_in_ms) * 1ms;
                        slsfs::log::log("set timer wait time to {}ms", slsfs::pack::ntoh(duration_in_ms));
                        break;
                    }

                    default:
                        self->start_job(pack);
                    }

                    slsfs::pack::packet_pointer ok = std::make_shared<slsfs::pack::packet>();
                    ok->header = pack->header;
                    ok->header.type = slsfs::pack::msg_t::ack;

                    slsfs::log::log<slsfs::log::level::debug>(fmt::format("return: {}", pack->header.print()));

                    self->start_write(ok);
                    if (pack->header.type != slsfs::pack::msg_t::set_timer)
                    {
                        self->last_update_ = now();
                        //slsfs::log::log("last update set to: {}", log_timer(self->last_update_));
                    }

                    self->timer_reset();
                    self->start_listen_commands();
                }
                else
                    slsfs::log::log<slsfs::log::level::error>("start_listen_commands_body: "); // + ec.messag$
            });
    }

    void start_write(slsfs::pack::packet_pointer pack) {
        start_write(pack, [](boost::system::error_code, std::size_t) {});
    }

    template<typename Func>
    void start_write(slsfs::pack::packet_pointer pack, Func next)
    {
        slsfs::log::log("start write");

        auto next_warpper = std::make_shared<slsfs::socket_writer::boost_callback>(
            [self=this->shared_from_this(), next=std::move(next)] (boost::system::error_code ec, std::size_t length) {
                if (not ec)
                    slsfs::log::log("worker sent msg");
                std::invoke(next, ec, length);
            });

        writer_.start_write_socket(pack, next_warpper);
    }

    void start_job(slsfs::pack::packet_pointer pack)
    {
        queue_map_accessor it;
        boost::asio::io_context::strand * ptr = nullptr;
        if (not queue_map_.find(it, slsfs::uuid::to_uuid(pack->header.key)))
        {
            auto new_strand = std::make_unique<boost::asio::io_context::strand>(io_context_);
            ptr = new_strand.get();
            queue_map_.emplace(slsfs::uuid::to_uuid(pack->header.key), std::move(new_strand));
        }
        else
            ptr = it->second.get();

        boost::asio::post(
            boost::asio::bind_executor(
                *ptr,
                [self=this->shared_from_this(), pack] {
                    auto const start = std::chrono::high_resolution_clock::now();

                    slsfs::jsre::request_parser<slsfs::base::byte> input {pack};

                    if (self->datastorage_conf_->use_async())
                    {
                        self->start_storage_perform(
                            input,
                            [self=self->shared_from_this(), pack, start] (slsfs::base::buf buf) {
                                pack->header.type = slsfs::pack::msg_t::worker_response;
                                pack->data.buf.resize(buf.size());// = std::vector<slsfs::pack::unit_t>(v.size(), '\0');
                                std::memcpy(pack->data.buf.data(), buf.data(), buf.size());

                                self->start_write(pack);
                                auto const end = std::chrono::high_resolution_clock::now();
                                auto relativetime = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                                slsfs::log::log<slsfs::log::level::debug>("req finish in: {}", relativetime);
                            });
                    }
                    else
                    {
                        slsfs::base::buf v = self->storage_perform(input);
                        pack->header.type = slsfs::pack::msg_t::worker_response;
                        pack->data.buf.resize(v.size());// = std::vector<slsfs::pack::unit_t>(v.size(), '\0');
                        std::memcpy(pack->data.buf.data(), v.data(), v.size());

                        self->start_write(pack);
                        auto const end = std::chrono::high_resolution_clock::now();
                        auto relativetime = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                        slsfs::log::log("req finish in: {}", relativetime);
                    }
                }
            )
        );
    }

    auto storage_perform(slsfs::jsre::request_parser<slsfs::base::byte> const& single_input)
        -> slsfs::base::buf
    {
        switch (single_input.type())
        {
        case slsfs::jsre::type_t::file:
        {
            return slsfsdf::perform_single_request(*datastorage_conf_.get(), single_input);
            break;
        }

        case slsfs::jsre::type_t::metadata:
        {
            break;
        }

        case slsfs::jsre::type_t::wakeup:
        {
            break;
        }

        case slsfs::jsre::type_t::storagetest:
        {
            slsfs::log::log("end read from storage (1000)");
        }

        }
        return {};
    }

    void start_storage_perform(slsfs::jsre::request_parser<slsfs::base::byte> const& single_input,
                               std::function<void(slsfs::base::buf)> next)
    {
        switch (single_input.type())
        {
        case slsfs::jsre::type_t::file:
        {
            datastorage_conf_->start_perform(single_input, std::move(next));
            break;
        }

        case slsfs::jsre::type_t::metadata:
        {
            break;
        }

        case slsfs::jsre::type_t::wakeup:
        {
            break;
        }

        case slsfs::jsre::type_t::storagetest:
        {
            slsfs::log::log("end read from storage (1000)");
        }

        }
    }
};

} // namespace slsfsdf::server

#endif // PROXY_COMMAND_HPP__
