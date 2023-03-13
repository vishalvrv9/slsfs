
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
    std::cout << fmt::format("{0} avg={1:.3f} sd={2:.3f}", memo, mean, std::sqrt(var)) << "\n";
    for (auto && [time, count] : dist)
        std::cout << fmt::format("{0} {1}: {2}", memo, time, count) << "\n";
}

namespace slsfsdf
{

using boost::asio::ip::tcp;

template<typename NextFunc>
void start_send_request_sequence (std::shared_ptr<slsfsdf::storage_conf> conf,
                                  slsfs::pack::packet_pointer ptr,
                                  NextFunc && next)
{
    slsfs::jsre::request_parser<slsfs::base::byte> input {ptr};

    auto const start = std::chrono::high_resolution_clock::now();
    conf->start_perform(
        input,
        [input, start, conf, next]
        (slsfs::base::buf buf) {
            auto const end = std::chrono::high_resolution_clock::now();
            auto relativetime = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

            std::invoke(next, relativetime);
        });
}

void start_send_request(std::shared_ptr<slsfsdf::storage_conf> conf,
                        slsfs::pack::packet_pointer ptr,
                        std::atomic<int> &outstanding_requests,
                        oneapi::tbb::concurrent_vector<std::uint64_t> &result)
{
    slsfs::jsre::request_parser<slsfs::base::byte> input {ptr};

    auto const start = std::chrono::high_resolution_clock::now();
    conf->start_perform(
        input,
        [input, start, conf, &result, &outstanding_requests]
        (slsfs::base::buf buf) {
            auto const end = std::chrono::high_resolution_clock::now();
            auto relativetime = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            result.push_back(relativetime);

            slsfs::log::log<slsfs::log::level::info>("request finished");
            if (--outstanding_requests <= 0)
            {
                slsfs::log::log<slsfs::log::level::info>("conf->close()");
                conf->close();
            }
        });
}

int do_datafunction()
{
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);

    boost::asio::steady_timer timer(ioc);
    timer.expires_from_now(std::chrono::seconds(2));
    timer.async_wait([] (boost::system::error_code) {});

    using json = slsfs::base::json;
    json input;
    std::cin >> input;

    std::vector<std::thread> workers;
    unsigned int const worker = std::min<unsigned int>(4, std::thread::hardware_concurrency());
    workers.reserve(worker);
    for (unsigned int i = 0; i < worker; i++)
        workers.emplace_back([&ioc] { ioc.run(); });

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

    slsfs::log::log<slsfs::log::level::info>("starting");

    //start_send_request_sequence

    int const outstanding_requests = 10000;
    std::atomic<int> counter = outstanding_requests;
    oneapi::tbb::concurrent_vector<std::uint64_t> result_vector;
    for (int i = 0; i < outstanding_requests; i++)
    {
        // static_cast<unsigned char>(rand()%16)
        slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();
        ptr->header.gen();
        ptr->header.key = slsfs::pack::key_t{2, 2, 2, 2, 2, 2, 2, 2,
                                             2, 2, 2, 2, 2, 2, 2, 2,
                                             2, 2, 2, 2, 2, 2, 2, 2,
                                             2, 2, 2, 2, 2, 2, 2, 2};

        std::string buf (4096, 'E');
        slsfs::jsre::request request {
            .type      = slsfs::jsre::type_t::file,
            .operation = slsfs::jsre::operation_t::write,
            .uuid      = ptr->header.key,
            .position  = 0,
            .size      = static_cast<std::uint32_t>(buf.size()),
        };
        request.to_network_format();

        if (request.operation == slsfs::jsre::operation_t::read)
        {
            ptr->data.buf.resize(sizeof (request));
            std::memcpy(ptr->data.buf.data(), &request, sizeof (request));
        }
        else
        {
            ptr->data.buf.resize(sizeof (request) + buf.size());
            std::memcpy(ptr->data.buf.data(), &request, sizeof (request));
            std::memcpy(ptr->data.buf.data() + sizeof (request), buf.data(), buf.size());
        }

        start_send_request(conf, ptr, counter, result_vector);
    }

    for (std::thread& th : workers)
        th.join();

    stats(result_vector.begin(), result_vector.end());

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
