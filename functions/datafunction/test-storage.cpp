
#include "worker.hpp"
#include "storage-conf.hpp"
#include "storage-conf-cass.hpp"
#include "storage-conf-swift.hpp"
#include "storage-conf-ssbd-backend.hpp"
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


namespace slsfsdf
{

using boost::asio::ip::tcp;

void send_request(std::shared_ptr<slsfsdf::storage_conf> conf, slsfs::jsre::operation_t operation)
{
    slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();
    ptr->header.gen();
    ptr->header.key = slsfs::pack::key_t{2, 2, 2, 2, 2, 2, 2, 2,
                                         2, 2, 2, 2, 2, 2, 2, 2,
                                         2, 2, 2, 2, 2, 2, 2, 2,
                                         2, 2, 2, 2, 2, 2, 2, 2};

    operation = slsfs::jsre::operation_t::read;

    std::string buf (10240, 'E');
    slsfs::jsre::request read_request {
        .type      = slsfs::jsre::type_t::file,
        .operation = operation,
        .uuid      = ptr->header.key,
        .position  = 0,
        .size      = 10240,
    };

    read_request.to_network_format();

    if (operation == slsfs::jsre::operation_t::read)
    {
        ptr->data.buf.resize(sizeof (read_request));
        std::memcpy(ptr->data.buf.data(), &read_request, sizeof (read_request));
    }
    else
    {
        ptr->data.buf.resize(sizeof (read_request) + buf.size());
        std::memcpy(ptr->data.buf.data(), &read_request, sizeof (read_request));
        std::memcpy(ptr->data.buf.data() + sizeof (read_request), buf.data(), buf.size());
    }

    slsfs::jsre::request_parser<slsfs::base::byte> input {ptr};

    auto const start = std::chrono::high_resolution_clock::now();
    conf->start_perform(
        input,
        [start] (slsfs::base::buf buf) {
            auto const end = std::chrono::high_resolution_clock::now();
            auto relativetime = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            slsfs::log::log<slsfs::log::level::debug>("req finish in: {}", relativetime);

            std::stringstream ss;
            for (char c : buf)
                ss << c;
            slsfs::log::log("read response: '{}'", ss.str());
        });
}

int do_datafunction()
{
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);

    using json = slsfs::base::json;
    json input;
    std::cin >> input;

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
    case "ssbd"_:
        conf = std::make_shared<slsfsdf::storage_conf_ssbd_backend>(ioc);
        break;
    case "cassandra"_:
        conf = std::make_shared<slsfsdf::storage_conf_cass>();
        break;
    case "swift"_:
        conf = std::make_shared<slsfsdf::storage_conf_swift>();
        break;
    }
    conf->init(input["storageconfig"]);

    //send_request(conf, slsfs::jsre::operation_t::write);
    send_request(conf, slsfs::jsre::operation_t::read);

    std::vector<std::thread> v;
    unsigned int const worker = std::min<unsigned int>(4, std::thread::hardware_concurrency());
    v.reserve(worker);
    for(unsigned int i = 0; i < worker; i++)
        v.emplace_back([&ioc] { ioc.run(); });

    for (std::thread& th : v)
        th.join();

    return 0;
}

} // namespace slsfsdf

int main(int argc, char *argv[])
{
    std::string name = fmt::format("DF:{0:4d}", slsfs::uuid::gen_rand_number());
    char const* name_cstr = name.c_str();
    slsfs::log::init(name_cstr);

    return slsfsdf::do_datafunction();
}
