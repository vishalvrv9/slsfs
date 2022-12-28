#include "../basic.hpp"
#include "../serializer.hpp"
#include "../json-replacement.hpp"

#include <fmt/core.h>
#include <boost/asio.hpp>
#include <absl/random/random.h>
#include <absl/random/zipf_distribution.h>

#include <algorithm>
#include <iostream>

#include <memory>
#include <array>
#include <list>
#include <thread>
#include <vector>
#include <random>
#include <chrono>

using boost::asio::ip::tcp;

template<typename Function>
auto record(Function &&f) -> long int
{
    //std::chrono::high_resolution_clock::time_point;
    auto const start = std::chrono::high_resolution_clock::now();
    std::invoke(f);
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
    BOOST_LOG_TRIVIAL(info) << fmt::format("{0} avg={1:.3f} sd={2:.3f}", memo, mean, std::sqrt(var));
    for (auto && [time, count] : dist)
        BOOST_LOG_TRIVIAL(info) << fmt::format("{0} {1}: {2}", memo, time, count);
}

void readtest (int const times, int const bufsize,
               std::function<slsfs::pack::key_t(void)> genname,
               std::function<int(int)> genpos,
               std::string const memo = "")
{
    boost::asio::io_context io_context;
    tcp::socket s(io_context);
    tcp::resolver resolver(io_context);
    boost::asio::connect(s, resolver.resolve("192.168.0.224", "12001"));

    std::string const buf(bufsize, 'A');
    std::list<double> records;

    for (int i = 0; i < times; i++)
    {
        slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();

        ptr->header.type = slsfs::pack::msg_t::trigger;
        ptr->header.key = genname();
        //std::string const payload = fmt::format("{{\"operation\": \"read\", \"filename\": \"/embl1.txt\", \"type\": \"file\", \"position\": {}, \"size\": {} }}", genpos(i), buf.size());
        slsfs::jsre::request r;
        r.type = slsfs::jsre::type_t::file;
        r.operation = slsfs::jsre::operation_t::read;
        r.uuid = ptr->header.key;
        r.position = genpos(i);
        r.size = buf.size();
        r.to_network_format();

        ptr->data.buf.resize(sizeof (r));
        std::memcpy(ptr->data.buf.data(), &r, sizeof (r));

        //std::copy(payload.begin(), payload.end(), std::back_inserter(ptr->data.buf));

        ptr->header.gen();
        BOOST_LOG_TRIVIAL(debug) << "sending " << ptr->header;
        auto sendbuf = ptr->serialize();
        records.push_back(record([&]() {
            boost::asio::write(s, boost::asio::buffer(sendbuf->data(), sendbuf->size()));

            slsfs::pack::packet_pointer resp = std::make_shared<slsfs::pack::packet>();
            std::vector<slsfs::pack::unit_t> headerbuf(slsfs::pack::packet_header::bytesize);
            boost::asio::read(s, boost::asio::buffer(headerbuf.data(), headerbuf.size()));

            resp->header.parse(headerbuf.data());
            BOOST_LOG_TRIVIAL(debug) << "read resp: " << resp->header;

            std::string data(resp->header.datasize, '\0');
            boost::asio::read(s, boost::asio::buffer(data.data(), data.size()));
//            BOOST_LOG_TRIVIAL(trace) << data ;
        }));
    }

    stats(records.begin(), records.end(), fmt::format("read {}", memo));
}

void writetest (int const times, int const bufsize,
                std::function<slsfs::pack::key_t(void)> genname,
                std::function<int(int)> genpos, std::string const memo = "")
{
    boost::asio::io_context io_context;
    tcp::socket s(io_context);
    tcp::resolver resolver(io_context);
    boost::asio::connect(s, resolver.resolve("192.168.0.224", "12001"));

    std::string const buf(bufsize, 'A');

    std::list<double> records;
    for (int i = 0; i < times; i++)
    {
        slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();

        ptr->header.type = slsfs::pack::msg_t::trigger;
        ptr->header.key = genname();
        //std::string const payload = fmt::format("{{ \"operation\": \"write\", \"filename\": \"/embl1.txt\", \"type\": \"file\", \"position\": {}, \"size\": {}, \"data\": \"{}\" }}", genpos(i), buf.size(), buf);

        slsfs::jsre::request r;
        r.type = slsfs::jsre::type_t::file;
        r.operation = slsfs::jsre::operation_t::write;
        r.uuid = ptr->header.key;
        r.position = genpos(i);
        r.size = buf.size();

        r.to_network_format();

        ptr->data.buf.resize(sizeof (r) + buf.size());
        std::memcpy(ptr->data.buf.data(), &r, sizeof (r));
        std::memcpy(ptr->data.buf.data() + sizeof (r), buf.data(), buf.size());

        //= std::string("{\"operation\": \"write\", \"filename\": \"/helloworld.txt\", \"type\": \"file\", \"position\": 0, \"size\": ") + buf.size() + ", \"data\": \"" + buf + "\"}";
        //std::copy(payload.begin(), payload.end(), std::back_inserter(ptr->data.buf));

        ptr->header.gen();
        BOOST_LOG_TRIVIAL(debug) << "sending " << ptr->header;
        auto buf = ptr->serialize();

        records.push_back(record([&]() {
            boost::asio::write(s, boost::asio::buffer(buf->data(), buf->size()));

            slsfs::pack::packet_pointer resp = std::make_shared<slsfs::pack::packet>();
            std::vector<slsfs::pack::unit_t> headerbuf(slsfs::pack::packet_header::bytesize);
            boost::asio::read(s, boost::asio::buffer(headerbuf.data(), headerbuf.size()));

            resp->header.parse(headerbuf.data());
            BOOST_LOG_TRIVIAL(debug) << "write resp:" << resp->header;

            std::string data(resp->header.datasize, '\0');
            boost::asio::read(s, boost::asio::buffer(data.data(), data.size()));
            //BOOST_LOG_TRIVIAL(debug) << data ;
       }));
    }
    stats(records.begin(), records.end(), fmt::format("write {}", memo));
}

int main(int argc, char *argv[])
{
    slsfs::basic::init_log();

    if (argc < 4)
    {
        BOOST_LOG_TRIVIAL(fatal) << "usage: ./client [times] [# of client] bufsize resultfile";
        return -1;
    }

    int const times   = std::stoi(argv[1]);
    int const clients = std::stoi(argv[2]);
    int const bufsize = std::stoi(argv[3]);
    std::string resultfile = argv[4];

    std::mt19937 engine(19937);

    absl::zipf_distribution namedist(256*256, /*alpha=*/ 1.2);

    auto genname =
        [&engine, &namedist]() {
            int n = namedist(engine);
            slsfs::pack::unit_t n1 = n / 256;
            slsfs::pack::unit_t n2 = n % 256;
            return slsfs::pack::key_t {
                7, 8, 7, 8, 7, 8, 7, 8,
                7, 8, 7, 8, 7, 8, 7, 8,
                7, 8, 7, 8, 7, 8, 7, 8,
                7, 8, 7, 8, 7, 8, n1, n2
            };
        };

    {
        std::vector<std::thread> v;
        for (int i = 0; i < clients; i++)
            v.emplace_back(writetest, times, bufsize,
                           genname,
                           [bufsize](int i) { return i*bufsize; }, "seq");

        for (std::thread& th : v)
            th.join();
    }

    {
        std::uniform_int_distribution<> dist(0, 1000);
        std::vector<std::thread> v;
        for (int i = 0; i < clients; i++)
            v.emplace_back(readtest, times, bufsize,
                           genname,
                           [&engine, &dist](int) { return dist(engine); }, "rand read");

        for (std::thread& th : v)
            th.join();
    }

    return EXIT_SUCCESS;
}
