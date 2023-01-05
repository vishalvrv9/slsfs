

#include "datafunction.hpp"
//#include "metadatafunction.hpp"
#include "worker.hpp"
#include "storage-conf.hpp"
#include "storage-conf-cass.hpp"
#include "storage-conf-ssbd.hpp"
#include "storage-conf-ssbd-stripe.hpp"
#include "proxy-command.hpp"

#include <slsfs.hpp>

#include <oneapi/tbb/concurrent_hash_map.h>
#include <oneapi/tbb/concurrent_queue.h>

#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/asio.hpp>

#include <iostream>
#include <memory>
#include <thread>
#include <sstream>
#include <map>
#include <ctime>

template<typename Function, typename ... Args>
auto record(Function &&f, Args &&... args) -> long int
{
    auto const start = std::chrono::high_resolution_clock::now();
    std::invoke(f, std::forward<Args>(args)...);
    auto const now = std::chrono::high_resolution_clock::now();
    auto relativetime = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
    return relativetime;
}

template<typename Iterator>
void stats(Iterator start, Iterator end, std::string const memo = "")
{
    int const size = std::distance(start, end);

    double sum = std::accumulate(start, end, 0.0);
    double mean = sum / size, var = 0;

    std::map<int, int> dist;
    for (; start != end; start++)
    {
        dist[(*start)/1000000]++;
        var += std::pow((*start) - mean, 2);
    }

    var /= size;
    slsfs::log::logstring<slsfs::log::level::info>(fmt::format("{0} avg={1:.3f} sd={2:.3f}", memo, mean, std::sqrt(var)));
    for (auto && [time, count] : dist)
        slsfs::log::logstring<slsfs::log::level::info>(fmt::format("{0} {1}: {2}", memo, time, count));
}

namespace slsfsdf
{

using boost::asio::ip::tcp;

int do_datafunction(std::ostream &ow_out)
try
{
    slsfsdf::server::queue_map queue_map;
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);

    slsfsdf::server::proxy_set proxys;

    using json = slsfs::base::json;
    json input;

    std::cin >> input;

#ifdef AS_ACTIONLOOP
    input = input["value"];
#endif

    /* example json config
    {
        "type": "wakeup",
        "proxyhost": "192.168.1.1",
        "proxyport": "12000",
        "storagetype": "ssbd-basic",
        "storageconfig": {
            "hosts": [{
                "host": "192.168.2.1",
                "port": "12000"
            }]
        }
    }
    */

    std::shared_ptr<slsfsdf::storage_conf> conf = nullptr;

    std::string const storagetype = input["storagetype"].get<std::string>();
    switch (slsfs::sswitch::hash(storagetype))
    {
    using namespace slsfs::sswitch;
    case "ssbd-basic"_:
        conf = std::make_shared<slsfsdf::storage_conf_ssbd>(ioc);
        break;
    case "ssbd-stripe"_:
        conf = std::make_shared<slsfsdf::storage_conf_ssbd_stripe>(ioc);
        break;
    case "cassandra"_:
        conf = std::make_shared<slsfsdf::storage_conf_cass>();
        break;
    case "swift"_:
        conf = std::make_shared<slsfsdf::storage_conf_swift>();
        break;
    }
    conf->init(input["storageconfig"]);

    auto proxy_command_ptr = std::make_shared<slsfsdf::server::proxy_command>(ioc, conf, queue_map, proxys);
    proxys.emplace(proxy_command_ptr, 0);

    std::string const proxyhost = input["proxyhost"].get<std::string>();
    std::string const proxyport = input["proxyport"].get<std::string>();

    slsfs::log::logstring(fmt::format("connect to {}:{}", proxyhost, proxyport));

    boost::asio::steady_timer function_timeout{ioc};
    {
        using namespace std::chrono_literals;
        function_timeout.expires_from_now(298s);
///        function_timeout.expires_from_now(20s);
        function_timeout.async_wait(
            [proxy_command_ptr] (boost::system::error_code ec) {
                slsfs::log::logstring(fmt::format("time to die: get code: {}", ec.message()));
                proxy_command_ptr->close();
            });
    }

    proxy_command_ptr->start_connect(resolver.resolve(proxyhost, proxyport));

    std::vector<std::thread> v;
    unsigned int const worker = std::min<unsigned int>(4, std::thread::hardware_concurrency());
    v.reserve(worker);
    for(unsigned int i = 0; i < worker; i++)
        v.emplace_back([&ioc] { ioc.run(); });

    json output;
    output["original-request"] = input;
    output["response"] = json::array();

    for (std::thread& th : v)
        th.join();

    slsfs::log::logstring("data function shutdown");
    ow_out << "{\"finish\": \"job\"}" << std::endl;

    return 0;
}
catch (std::exception const & e)
{
    slsfs::log::logstring(std::string("exception thrown ") + e.what());
    std::cerr << "do function ecxception: " << e.what() << std::endl;
    return -1;
}

} // namespace slsfsdf

int main(int argc, char *argv[])
{
    std::string name = fmt::format("DF:{0:4d}", slsfs::uuid::gen_rand_number());
    char const* name_cstr = name.c_str();
    slsfs::log::init(name_cstr);
    slsfs::log::logstring("data function start");

    //std::uint32_t version = std::time(nullptr);//const auto p1 = std::chrono::system_clock::now();;
    SCOPE_DEFER([] { slsfs::log::push_logs(); });

#ifdef AS_ACTIONLOOP
    namespace io = boost::iostreams;

    io::stream_buffer<io::file_descriptor_sink> fpstream(3, io::close_handle);
    std::ostream ow_out {&fpstream};
    while (true)
    {
        slsfs::log::logstring("starting as action loop");
        //records.push_back(record([&](){ slsfsdf::do_datafunction(ow_out); }));
        int error = slsfsdf::do_datafunction(ow_out);
        if (error != 0)
            return error;
    }
    return 0;
#else
    std::cerr << "starting as normal" << std::endl;
    return slsfsdf::do_datafunction(std::cout);
    //slsfs::log::push_logs();
#endif
}
