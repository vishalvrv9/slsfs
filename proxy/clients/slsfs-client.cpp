#include "../basic.hpp"
#include "../serializer.hpp"
#include "../json-replacement.hpp"

#include <fmt/core.h>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
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
auto stats(Iterator start, Iterator end, std::string const memo = "")
    -> std::tuple<std::map<int, int>, int, int>
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

    return {dist, mean, std::sqrt(var)};
}

auto readtest (int const times, int const bufsize,
               std::function<slsfs::pack::key_t(void)> genname,
               std::function<int(int)> genpos,
               std::string const memo = "")
    -> std::pair<std::list<double>, std::chrono::nanoseconds>
{
    boost::asio::io_context io_context;
    tcp::socket s(io_context);
    tcp::resolver resolver(io_context);
    boost::asio::connect(s, resolver.resolve("192.168.0.224", "12001"));

    std::string const buf(bufsize, 'A');
    std::list<double> records;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < times; i++)
    {
        slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();

        ptr->header.type = slsfs::pack::msg_t::trigger;
        ptr->header.key = genname();
        slsfs::jsre::request r;
        r.type = slsfs::jsre::type_t::file;
        r.operation = slsfs::jsre::operation_t::read;
        r.uuid = ptr->header.key;
        r.position = genpos(i);
        r.size = buf.size();
        r.to_network_format();

        ptr->data.buf.resize(sizeof (r));
        std::memcpy(ptr->data.buf.data(), &r, sizeof (r));

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
            BOOST_LOG_TRIVIAL(debug) << data ;
        }));
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats(records.begin(), records.end(), fmt::format("read {}", memo));

    return {records, end - start};
}

auto writetest (int const times, int const bufsize,
                std::function<slsfs::pack::key_t(void)> genname,
                std::function<int(int)> genpos, std::string const memo = "")
    -> std::pair<std::list<double>, std::chrono::nanoseconds>
{
    boost::asio::io_context io_context;
    tcp::socket s(io_context);
    tcp::resolver resolver(io_context);
    boost::asio::connect(s, resolver.resolve("192.168.0.224", "12001"));

    std::string const buf(bufsize, 'A');

    std::list<double> records;

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < times; i++)
    {
        slsfs::pack::packet_pointer ptr = std::make_shared<slsfs::pack::packet>();

        ptr->header.type = slsfs::pack::msg_t::trigger;
        ptr->header.key = genname();

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
            BOOST_LOG_TRIVIAL(debug) << data ;
       }));
    }

    auto end = std::chrono::high_resolution_clock::now();
    stats(records.begin(), records.end(), fmt::format("write {}", memo));
    return {records, end - start};
}

template<typename TestFunc>
void start_test(std::string const testname, boost::program_options::variables_map& vm, TestFunc && testfunc)
{
    int const total_times   = vm["total-times"].as<int>();
    int const total_clients = vm["total-clients"].as<int>();
    int const bufsize       = vm["bufsize"].as<int>();
    double const zipf_alpha = vm["zipf-alpha"].as<double>();
    int const file_range    = vm["file-range"].as<int>();
    std::string const resultfile = vm["result"].as<std::string>();
    std::string const result_filename = resultfile + "-" + testname + ".csv";

    std::vector<std::future<std::pair<std::list<double>, std::chrono::nanoseconds>>> results;
    std::list<double> fullstat;
    for (int i = 0; i < total_clients; i++)
        results.emplace_back(std::invoke(testfunc));

    boost::filesystem::remove(result_filename);
    std::ofstream out_csv {result_filename, std::ios_base::app};

    out_csv << std::fixed << testname;
    out_csv << ",summary,";

    for (int i = 0; i < total_clients; i++)
        out_csv << ",client" << i;

    out_csv << "\n";

    std::vector<std::list<double>> gathered_results;
    std::vector<std::chrono::nanoseconds> gathered_durations;

    for (auto it = results.begin(); it != results.end(); ++it)
    {
        auto && [result, duration] = it->get();
        gathered_results.push_back(result);
        gathered_durations.push_back(duration);

        fullstat.insert(fullstat.end(), std::next(result.begin()), result.end());
    }

    auto [dist, avg, stdev] = stats(fullstat.begin(), fullstat.end());

    for (unsigned int row = 0; row < total_times; row++)
    {
        if (row <= gathered_durations.size() && row != 0)
            out_csv << *std::next(gathered_durations.begin(), row-1);

        switch (row)
        {
        case 0:
            out_csv << ",,";
            break;
        case 1:
            out_csv << ",avg," << avg;
            break;
        case 2:
            out_csv << ",stdev," << stdev;
            break;
        case 3:
            out_csv << ",bufsize," << bufsize;
            break;
        case 4:
            out_csv << ",zipf-alpha," << zipf_alpha;
            break;
        case 5:
            out_csv << ",file-range," << file_range;
            break;

        case 6:
            out_csv << ",,";
            break;

        case 7:
            out_csv << ",dist(ms) bucket,";
            break;

        default:
            if (0 <= row - 8 && row - 8 < dist.size())
                out_csv << "," << std::next(dist.begin(), row - 8)->first
                        << "," << std::next(dist.begin(), row - 8)->second;
            else
                out_csv << ",,";
        }

        for (std::list<double>& list : gathered_results)
        {
            auto it = list.begin();
            out_csv << "," << *std::next(it, row);
        }
        out_csv << "\n";
    }
}

int main(int argc, char *argv[])
{
    slsfs::basic::init_log();

#ifdef NDEBUG
    boost::log::core::get()->set_filter(
        boost::log::trivial::severity >= boost::log::trivial::info);
#else
    boost::log::core::get()->set_filter(
        boost::log::trivial::severity >= boost::log::trivial::trace);
#endif // NDEBUG

    namespace po = boost::program_options;
    po::options_description desc{"Options"};
    desc.add_options()
        ("help,h", "Print this help messages")
        ("total-times",   po::value<int>()->default_value(10000), "each client run # total times")
        ("total-clients", po::value<int>()->default_value(1), "# of clients")
        ("bufsize",       po::value<int>()->default_value(4096), "Size of the read/write buffer")
        ("zipf-alpha",    po::value<double>()->default_value(1.2), "set the alpha value of zipf dist")
        ("file-range",    po::value<int>()->default_value(256*256), "set the total different number of files")
        ("result",        po::value<std::string>(), "save result to this file");

    po::positional_options_description pos_po;
    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv)
              .options(desc)
              .positional(pos_po).run(), vm);
    po::notify(vm);

    std::mt19937 engine(19937);

    int const total_times   = vm["total-times"].as<int>();
    int const bufsize       = vm["bufsize"].as<int>();
    double const zipf_alpha = vm["zipf-alpha"].as<double>();
    int const file_range    = vm["file-range"].as<int>();
    std::string const resultfile = vm["result"].as<std::string>();

    absl::zipf_distribution namedist(file_range, zipf_alpha);

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

    start_test(
        "write",
        vm,
        [&]() {
            return std::async(
                std::launch::async,
                writetest, total_times, bufsize,
                genname,
                [bufsize](int i) { return i*bufsize; },
                "seq");
        });

    {
        std::uniform_int_distribution<> dist(0, 1000);
        start_test(
            "read",
            vm,
            [&]() {
                return std::async(
                    std::launch::async,
                    readtest, total_times, bufsize,
                    genname,
                    [&engine, &dist](int) { return dist(engine); },
                    "rand");
            });
    }

    return EXIT_SUCCESS;
}
