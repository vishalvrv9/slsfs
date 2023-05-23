#pragma once
#ifndef CLIENTLIB_DIRECT_CLIENT_HPP__

#include <oneapi/tbb/concurrent_map.h>

#include "clientlib.hpp"

namespace slsfs::client
{

class direct_client
{
    boost::asio::io_context& io_context_;
    client client_;

    using df_map = oneapi::tbb::concurrent_hash_map<
        pack::packet_header,
        std::shared_ptr<boost::asio::ip::tcp::socket>,
        pack::packet_header_key_hash_compare>;
    df_map df_connection_map_;

    auto get_direct_worker_socket(slsfs::pack::packet_pointer pack)
        -> std::shared_ptr<tcp::socket>
    {
        if (df_map::accessor acc;
            df_connection_map_.find(acc, pack->header))
        {
            std::shared_ptr<tcp::socket> df_socket = acc->second;
            if (df_socket->is_open())
                return df_socket;
        }
        // no socket found. connect to worker (df)

        BOOST_LOG_TRIVIAL(trace) << "sending packet to proxy " << pack->header;
        // get address
        std::string raw_buffer = client_.send(pack);

        boost::asio::ip::address_v4::bytes_type host;
        std::memcpy(&host, raw_buffer.data(), sizeof(host));
        boost::asio::ip::address address = boost::asio::ip::make_address_v4(host);

        std::uint16_t port = 0;
        std::memcpy(&port, raw_buffer.data() + sizeof(host), sizeof(port));
        port = pack::hton(port);

        boost::asio::ip::tcp::endpoint endpoint(address, port);

        auto df_socket = std::make_shared<tcp::socket>(io_context_);
        try
        {
            df_connection_map_.emplace(pack->header, df_socket);
            df_socket->connect(endpoint);
        }
        catch (boost::exception & e)
        {
            using host_endpoint = boost::error_info<struct df_endpoint, boost::asio::ip::tcp::endpoint>;
            e << host_endpoint{endpoint};
            throw;
        }
        return df_socket;
    }

    auto send_request(pack::packet_pointer pack) -> std::string
    {
        //pack::packet_pointer proxy_pack = std::make_shared<pack::packet>();
        //proxy_pack->header = pack->header;
        std::shared_ptr<tcp::socket> selected = get_direct_worker_socket(pack);

        auto pbuf = pack->serialize();
        BOOST_LOG_TRIVIAL(trace) << "sending packet to df " << pack->header;
        boost::asio::write(*selected, boost::asio::buffer(pbuf->data(), pbuf->size()));

        pack::packet_pointer resp = std::make_shared<pack::packet>();
        std::vector<pack::unit_t> headerbuf(pack::packet_header::bytesize);
        boost::asio::read(*selected, boost::asio::buffer(headerbuf.data(), headerbuf.size()));

        resp->header.parse(headerbuf.data());

        std::string data(resp->header.datasize, '\0');
        boost::asio::read(*selected, boost::asio::buffer(data.data(), data.size()));
        return data;
    }

public:
    direct_client(boost::asio::io_context& io, std::string const& zkhost):
        io_context_{io}, client_{io, zkhost} {}

    auto send(pack::packet_pointer pack) -> std::string
    {
        do
        {
            try {
                return send_request(pack);
            } catch (boost::system::system_error const& e) {
                BOOST_LOG_TRIVIAL(error) << "getin' system error when sending request. retry. " << boost::diagnostic_information(e);
            }
        } while (true);
        return "";
    }
};

} // namespace client

#endif // CLIENTLIB_DIRECT_CLIENT_HPP__
