#include "../basic.hpp"
#include "../serializer.hpp"
#include "../json-replacement.hpp"
#include "../uuid.hpp"
#include "clientlib.hpp"

#include <fmt/core.h>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <absl/random/random.h>
#include <absl/random/zipf_distribution.h>
#include <zk/client.hpp>
#include <zk/results.hpp>
#include <zookeeper/zookeeper.h>

#include <algorithm>
#include <iostream>
#include <array>
#include <memory>
#include <array>
#include <list>
#include <thread>
#include <vector>
#include <random>
#include <chrono>

auto get_uuid (boost::asio::io_context &io_context, std::string const& child) -> boost::asio::ip::tcp::resolver::results_type
{
    using namespace std::string_literals;

    zk::client client = zk::client::connect("zk://zookeeper-1:2181").get();

    zk::future<zk::get_result> resp = client.get("/slsfs/proxy/"s + child);
    zk::buffer const buf = resp.get().data();

    auto const colon = std::find(buf.begin(), buf.end(), ':');

    std::string host(std::distance(buf.begin(), colon), '\0');
    std::copy(buf.begin(), colon, host.begin());

    std::string port(std::distance(std::next(colon), buf.end()), '\0');
    std::copy(std::next(colon), buf.end(), port.begin());

    boost::asio::ip::tcp::resolver resolver(io_context);
    BOOST_LOG_TRIVIAL(debug) << "resolving: " << host << ":" << port ;
    return resolver.resolve(host, port);
}

auto setup_slsfs(boost::asio::io_context &io_context) -> std::pair<std::vector<slsfs::uuid::uuid>, std::vector<boost::asio::ip::tcp::resolver::results_type>>
{
    zk::client client = zk::client::connect("zk://zookeeper-1:2181").get();

    std::vector<slsfs::uuid::uuid> new_proxy_list;
    std::vector<boost::asio::ip::tcp::resolver::results_type> proxys;
    zk::future<zk::get_children_result> children = client.get_children("/slsfs/proxy");

    std::vector<std::string> list = children.get().children();

    std::transform (list.begin(), list.end(),
                    std::back_inserter(new_proxy_list),
                    slsfs::uuid::decode_base64);

    std::sort(new_proxy_list.begin(), new_proxy_list.end());

    for (auto proxy : new_proxy_list)
        proxys.push_back(get_uuid(io_context, proxy.encode_base64()));

    BOOST_LOG_TRIVIAL(debug) << "start with " << proxys.size()  << " proxies";
    return {new_proxy_list, proxys};
}

int pick_proxy(std::vector<slsfs::uuid::uuid> const& proxy_uuid, slsfs::pack::key_t& fileid)
{
    auto it = std::upper_bound (proxy_uuid.begin(), proxy_uuid.end(), fileid);
    if (it == proxy_uuid.end())
        it = proxy_uuid.begin();

    return std::distance(proxy_uuid.begin(), it);
}

void send_cmd(slsfs::tcp::socket& s, slsfs::pack::packet_pointer ptr)
{
    auto pbuf = ptr->serialize();
    boost::asio::write(s, boost::asio::buffer(pbuf->data(), pbuf->size()));

    slsfs::pack::packet_pointer resp = std::make_shared<slsfs::pack::packet>();
    std::vector<slsfs::pack::unit_t> headerbuf(slsfs::pack::packet_header::bytesize);
    boost::asio::read(s, boost::asio::buffer(headerbuf.data(), headerbuf.size()));

    resp->header.parse(headerbuf.data());
    std::string data(resp->header.datasize, '\0');
    boost::asio::read(s, boost::asio::buffer(data.data(), data.size()));
    std::cout << data << "\n";
}

void run()
{
    boost::asio::io_context io_context;
    auto&& [proxy_uuid, proxy_endpoint] = setup_slsfs(io_context);
    std::vector<slsfs::tcp::socket> proxy_sockets;

    for (unsigned int i = 0; i < proxy_endpoint.size(); i++)
    {
        proxy_sockets.emplace_back(io_context);
        boost::asio::connect(proxy_sockets.back(), proxy_endpoint.at(i));
    }

    while (true)
    {
        std::string cmd, arg1, arg2;
        std::cout << "cmd> ";
        std::cin >> cmd >> arg1;

        slsfs::pack::packet_pointer ptr = nullptr;

        switch (slsfs::basic::sswitcher::hash(cmd))
        {
            using namespace slsfs::basic::sswitcher;
        case "ls"_:
            ptr = slsfs::client::ls(arg1);
            break;
        case "mkdir"_:
            ptr = slsfs::client::mkdir(arg1);
            break;
        case "addfile"_:
            std::cin >> arg2;
            ptr = slsfs::client::addfile(arg1, arg2);
            break;
        case "write"_:
            std::cin >> arg2;
            ptr = slsfs::client::write(arg1, arg2);
            break;
        case "read"_:
            std::cin >> arg2;
            ptr = slsfs::client::read(arg1, std::stoi(arg2));
            break;
        default:
            BOOST_LOG_TRIVIAL(error) << "unknown cmd '" << cmd << "'";
            return;
        }

        int index = pick_proxy(proxy_uuid, ptr->header.key);
        slsfs::tcp::socket& s = proxy_sockets.at(index);

        send_cmd(s, ptr);
    }
}

int main()
{
    slsfs::basic::init_log();
#ifdef NDEBUG
    boost::log::core::get()->set_filter(
        boost::log::trivial::severity >= boost::log::trivial::info);
    constexpr static ZooLogLevel loglevel = ZOO_LOG_LEVEL_ERROR;
#else
    boost::log::core::get()->set_filter(
        boost::log::trivial::severity >= boost::log::trivial::trace);
    constexpr static ZooLogLevel loglevel = ZOO_LOG_LEVEL_INFO;
#endif // NDEBUG
    ::zoo_set_debug_level(loglevel);

    run();
    return EXIT_SUCCESS;
}
