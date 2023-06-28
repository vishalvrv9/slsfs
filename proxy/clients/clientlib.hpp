#pragma once

#ifndef CLIENT_CLIENTLIB_HPP__
#define CLIENT_CLIENTLIB_HPP__

#include "clientlib-packet-create.hpp"
#include "clientlib-zookeeper.hpp"
#include "../uuid.hpp"

#include <oneapi/tbb/concurrent_map.h>

namespace slsfs::client
{

class client
{
    boost::asio::io_context& io_context_;
    std::shared_ptr<zookeeper> zoo_client_;

    using proxy_map =
        oneapi::tbb::concurrent_map<slsfs::uuid::uuid,
                                    std::shared_ptr<boost::asio::ip::tcp::socket>>;

    // c++11 atomic_exchange solution instead of std::atomic<std::shared_ptr<proxy_map>>
    std::shared_ptr<proxy_map> proxys_ = std::make_shared<proxy_map>();

    void reconfigure(std::vector<uuid::uuid> &new_sorted_proxy_list)
    {
        //BOOST_LOG_TRIVIAL(trace) << "on reconfigure";
        BOOST_LOG_TRIVIAL(info) << "proxy reconfigured. new proxy size = " << new_sorted_proxy_list.size();

        auto new_proxy = std::make_shared<proxy_map>();

        for (uuid::uuid const & id : new_sorted_proxy_list)
        {
            if (auto acc = proxys_->find(id);
                acc != proxys_->end() && acc->second->is_open())
            {
                // copy the existing socket
                new_proxy->emplace(*acc);
                continue;
            }

            boost::asio::ip::tcp::endpoint endpoint = zoo_client_->get_uuid(id.encode_base64());
            BOOST_LOG_TRIVIAL(trace) << "connecting to " << id << " " << endpoint;

            try
            {
                auto proxy_socket = std::make_shared<boost::asio::ip::tcp::socket> (io_context_);
                proxy_socket->connect(endpoint);

                new_proxy->emplace(id, proxy_socket);
            }
            catch (boost::exception & e)
            {
                using host_endpoint = boost::error_info<struct tag_host_endpoint, boost::asio::ip::tcp::endpoint>;
                e << host_endpoint{endpoint};
                throw;
            }
        }

        std::atomic_exchange(&proxys_, new_proxy);
    }

    auto send_request(slsfs::pack::packet_pointer pack) -> std::string
    {
        std::shared_ptr<proxy_map> proxys_copy = proxys_;
        auto pbuf = pack->serialize();
        BOOST_LOG_TRIVIAL(trace) << "send " << pack->header;

        std::shared_ptr<boost::asio::ip::tcp::socket> selected = nullptr;
        {
            auto it = proxys_copy->upper_bound(slsfs::uuid::up_cast(pack->header.key));
            if (it == proxys_copy->end())
                it = proxys_copy->begin();

            selected = it->second;
            BOOST_LOG_TRIVIAL(trace) << "selected proxy " << selected->remote_endpoint() << " from " << proxys_copy->size() << " proxys";
        }

        try
        {
            BOOST_LOG_TRIVIAL(debug) << "sending packet to proxy " << pack->header;
            boost::asio::write(*selected, boost::asio::buffer(pbuf->data(), pbuf->size()));

            slsfs::pack::packet_pointer resp = std::make_shared<slsfs::pack::packet>();
            std::vector<slsfs::pack::unit_t> headerbuf(slsfs::pack::packet_header::bytesize);
            boost::asio::read(*selected, boost::asio::buffer(headerbuf.data(), headerbuf.size()));

            resp->header.parse(headerbuf.data());

            std::string data(resp->header.datasize, '\0');
            boost::asio::read(*selected, boost::asio::buffer(data.data(), data.size()));

            BOOST_LOG_TRIVIAL(trace) << "request finished " << pack->header;
            return data;
        }
        catch (boost::exception& e)
        {
            using host_endpoint = boost::error_info<struct proxy_endpoint, boost::asio::ip::tcp::endpoint>;
            e << host_endpoint{selected->remote_endpoint()};
            throw;
        }
    }


public:
    client(boost::asio::io_context& io, std::string const& zkhost):
        io_context_{io}, zoo_client_{std::make_shared<zookeeper>(io, zkhost)}
    {
        zoo_client_->bind_reconfigure(
            [this](std::vector<uuid::uuid> &list) { reconfigure(list); });
        zoo_client_->reconfigure();
        zoo_client_->start_watch();
    }

    auto send(slsfs::pack::packet_pointer pack) -> std::string
    {
        int retry_count = 0;
        do
        {
            try {
                return send_request(pack);
            } catch (boost::exception const& e) {
                if (retry_count++ > 3)
                {
                    BOOST_LOG_TRIVIAL(error) << "no more retry " << boost::diagnostic_information(e);
                    throw;
                }

                BOOST_LOG_TRIVIAL(error) << "getting system error when sending request. retry. " << boost::diagnostic_information(e);
            }
        } while (true);
        return "";
    }
};

} // namespace client

#endif // CLIENT_CLIENTLIB_HPP__
