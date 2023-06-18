#pragma once

#ifndef CLIENT_CLIENTLIB_HPP__
#define CLIENT_CLIENTLIB_HPP__

#include "clientlib-packet-create.hpp"
#include "clientlib-zookeeper.hpp"
#include "../uuid.hpp"

#include <oneapi/tbb/concurrent_map.h>

#include <shared_mutex>

namespace slsfs::client
{

class client
{
    boost::asio::io_context& io_context_;
    zookeeper zoo_client_;
    oneapi::tbb::concurrent_map<slsfs::uuid::uuid,
                                std::shared_ptr<boost::asio::ip::tcp::socket>> proxys_;
    std::shared_mutex proxy_mutex_;

    void reconfigure(std::vector<uuid::uuid> &new_sorted_proxy_list)
    {
        //BOOST_LOG_TRIVIAL(trace) << "on reconfigure";
        BOOST_LOG_TRIVIAL(info) << "proxy reconfigured. new proxy size = " << new_sorted_proxy_list.size();
        for (uuid::uuid const & id : new_sorted_proxy_list)
        {
            auto acc = proxys_.find(id);

            if (acc != proxys_.end() && acc->second->is_open())
                continue;

            boost::asio::ip::tcp::endpoint endpoint = zoo_client_.get_uuid(id.encode_base64());
            BOOST_LOG_TRIVIAL(trace) << "connecting to " << id << " " << endpoint;

            try
            {
                auto proxy_socket = std::make_shared<boost::asio::ip::tcp::socket> (io_context_);
                proxy_socket->connect(endpoint);

                proxys_.emplace(id, proxy_socket);
            }
            catch (boost::exception & e)
            {
                using host_endpoint = boost::error_info<struct tag_host_endpoint, boost::asio::ip::tcp::endpoint>;
                e << host_endpoint{endpoint};
                throw;
            }
        }

        if (proxys_.size() != new_sorted_proxy_list.size())
        {
            std::sort(new_sorted_proxy_list.begin(), new_sorted_proxy_list.end());

            // remove all id not in the new_sorted_proxy_list
            // only need at unsafe erase
            std::unique_lock lock(proxy_mutex_);
            for (auto it = proxys_.begin(); it != proxys_.end(); )
            {
                if (std::binary_search(new_sorted_proxy_list.begin(),
                                       new_sorted_proxy_list.end(),
                                       it->first))
                    ++it;
                else
                    it = proxys_.unsafe_erase(it);
            }
        }
    }

    auto send_request(slsfs::pack::packet_pointer pack) -> std::string
    {
        auto pbuf = pack->serialize();
        //BOOST_LOG_TRIVIAL(trace) << "send " << pack->header;

        std::shared_ptr<boost::asio::ip::tcp::socket> selected = nullptr;
        {
            std::shared_lock lock(proxy_mutex_);

            auto it = proxys_.upper_bound(slsfs::uuid::up_cast(pack->header.key));
            if (it == proxys_.end())
                it = proxys_.begin();

            selected = it->second;
            BOOST_LOG_TRIVIAL(trace) << "selected proxy " << selected->remote_endpoint() << " from " << proxys_.size() << " proxys";
        }

        BOOST_LOG_TRIVIAL(trace) << "sending packet " << pack->header;
        boost::asio::write(*selected, boost::asio::buffer(pbuf->data(), pbuf->size()));

        slsfs::pack::packet_pointer resp = std::make_shared<slsfs::pack::packet>();
        std::vector<slsfs::pack::unit_t> headerbuf(slsfs::pack::packet_header::bytesize);
        boost::asio::read(*selected, boost::asio::buffer(headerbuf.data(), headerbuf.size()));

        resp->header.parse(headerbuf.data());

        std::string data(resp->header.datasize, '\0');
        boost::asio::read(*selected, boost::asio::buffer(data.data(), data.size()));
        return data;
    }


public:
    client(boost::asio::io_context& io, std::string const& zkhost):
        io_context_{io}, zoo_client_{io, zkhost}
    {
        zoo_client_.bind_reconfigure(
            [this](std::vector<uuid::uuid> &list) { reconfigure(list); });
        zoo_client_.reconfigure();
    }

    auto send(slsfs::pack::packet_pointer pack) -> std::string
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

#endif // CLIENT_CLIENTLIB_HPP__
