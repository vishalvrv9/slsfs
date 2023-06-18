#pragma once

#ifndef CLIENT_CLIENTLIB_ZOOKEEPER_HPP__
#define CLIENT_CLIENTLIB_ZOOKEEPER_HPP__

#include "../uuid.hpp"
#include "../basic.hpp"

#include <zk/client.hpp>
#include <zk/results.hpp>
#include <zookeeper/zookeeper.h>
#include <boost/signals2.hpp>
#include <boost/asio.hpp>


namespace slsfs::client
{

class zookeeper
{
    net::io_context& io_context_;
    zk::client client_;
    oneapi::tbb::concurrent_hash_map<std::string, net::ip::tcp::endpoint> uuid_cache_;
    boost::launch pool_ = boost::launch::async;
    bool closed_ = false;
    std::chrono::system_clock::duration local_zk_diff_ = std::chrono::nanoseconds::zero();
    boost::signals2::signal<void(std::vector<uuid::uuid>&)> on_reconfigure_;

#ifdef NDEBUG
    constexpr static ZooLogLevel loglevel = ZOO_LOG_LEVEL_ERROR;
#else
    //constexpr static ZooLogLevel loglevel = ZOO_LOG_LEVEL_DEBUG;
    constexpr static ZooLogLevel loglevel = ZOO_LOG_LEVEL_ERROR;
#endif // NDEBUG

public:
    zookeeper(net::io_context& io, std::string const& zkhost):
        io_context_{io},
        client_{zk::client::connect(zkhost).get()}
    {
        ::zoo_set_debug_level(loglevel);
        start_watch();
    }

    template<typename OnReconfigure>
    void bind_reconfigure(OnReconfigure reconf) {
        on_reconfigure_.connect(reconf);
    }

    void start_watch()
    {
        BOOST_LOG_TRIVIAL(trace) << "watch /slsfs/proxy start";

        client_.watch_children("/slsfs/proxy").then(
            pool_,
            [this] (zk::future<zk::watch_children_result> children) {
                auto&& res = children.get();
                BOOST_LOG_TRIVIAL(trace) << "set watch ok";

                res.next().then(
                    pool_,
                    [this, children=std::move(children)] (zk::future<zk::event> event) {
                        zk::event const & e = event.get();
                        BOOST_LOG_TRIVIAL(trace) << "watch event get: " << e.type();
                        start_watch();
                        reconfigure();
                    });
            });
    }

    void reconfigure()
    {
        BOOST_LOG_TRIVIAL(trace) << "start_reconfigure";

        std::vector<std::string> list = client_.get_children("/slsfs/proxy").get().children();

        BOOST_LOG_TRIVIAL(trace) << "proxy reconfigured. size: " << list.size();

        std::vector<uuid::uuid> new_proxy_list;
        std::transform (list.begin(), list.end(),
                        std::back_inserter(new_proxy_list),
                        uuid::decode_base64);

        std::sort(new_proxy_list.begin(), new_proxy_list.end());
        on_reconfigure_(new_proxy_list);
    }

    auto get_uuid (std::string const& child) -> net::ip::tcp::endpoint
    {
        using namespace std::string_literals;

        decltype(uuid_cache_)::accessor result;
        if (uuid_cache_.find(result, child))
            return result->second;

        zk::future<zk::get_result> resp = client_.get("/slsfs/proxy/"s + child);
        zk::buffer const buf = resp.get().data();

        auto const colon = std::find(buf.begin(), buf.end(), ':');

        std::string host(std::distance(buf.begin(), colon), '\0');
        std::copy(buf.begin(), colon, host.begin());

        std::string port(std::distance(std::next(colon), buf.end()), '\0');
        std::copy(std::next(colon), buf.end(), port.begin());

        net::ip::tcp::resolver resolver(io_context_);
        for (net::ip::tcp::endpoint resolved : resolver.resolve(host, port))
        {
            uuid_cache_.emplace(child, resolved);
            return resolved;
        }
        return {};
    }
};


} // namespace client

#endif // CLIENT_CLIENTLIB_ZOOKEEPER_HPP__
