#include "../basic.hpp"
#include "../serializer.hpp"
#include "../json-replacement.hpp"
#include "../uuid.hpp"
#include "../scope_exit.hpp"
#include "clientlib.hpp"
#include "clientlib-direct-client.hpp"

#include <fmt/core.h>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <absl/random/random.h>
#include <absl/random/zipf_distribution.h>
#include <zk/client.hpp>
#include <zk/results.hpp>
#include <zookeeper/zookeeper.h>

#include <chrono>

void start_test(boost::program_options::variables_map &vm)
{
    int    const total_times     = vm["total-times"].as<int>();
    int    const total_duration  = vm["total-duration"].as<int>();
    int    const bufsize         = vm["bufsize"].as<int>();
    double const zipf_alpha      = vm["zipf-alpha"].as<double>();
    int    const file_range      = vm["file-range"].as<int>();
    int    const worker          = vm["total-clients"].as<int>();
    std::string const test_name  = vm["test-name"].as<std::string>();
    std::string const resultfile = vm["result"].as<std::string>();
    std::string const zookeeper_host = vm["zookeeper"].as<std::string>();

    absl::zipf_distribution namedist(file_range, zipf_alpha);
    std::uniform_int_distribution<int> uniformdist(0, file_range), singledist(0, 255);

    auto start = std::chrono::system_clock::now();
    std::atomic<int> counter = 0;

    BOOST_LOG_TRIVIAL(info) << "starting test (thread=" << worker << "); bufsize=(" << bufsize << ")";
    std::vector<std::jthread> pool;

    for (int i = 0; i < worker; i++)
        pool.emplace_back(
            [total_times, worker, bufsize, start, total_duration, singledist, zookeeper_host, i, &counter] () mutable {
                std::random_device rd;
                int const seed = rd();
                std::mt19937 engine(seed);

                //auto anyname =
                //    [&engine, &singledist] {
                //        slsfs::pack::key_t t{};
                //        for (slsfs::pack::unit_t& n : t)
                //            n = singledist(engine);
                //        return t;
                //    };

                auto anyname =
                    [&engine, &singledist] {
                        slsfs::pack::key_t t{};
                        t[0] = singledist(engine);
                        t[1] = singledist(engine);
                        return t;
                    };

                BOOST_LOG_TRIVIAL(info) << "thread id=" << i << " seed=" << seed;

                try
                {
                    boost::asio::io_context io_context;
                    boost::asio::signal_set listener(io_context, SIGINT, SIGTERM);
                    listener.async_wait(
                        [&io_context] (boost::system::error_code const&, int signal_number) {
                            BOOST_LOG_TRIVIAL(info) << "Stopping... sig=" << signal_number;
                            io_context.stop();
                        });

                    slsfs::client::direct_client slsfs_client{io_context, zookeeper_host};

                    std::string buf(bufsize, 'A');
                    for (int i = 0; i < total_times; i++)
                    {
                        using namespace std::chrono_literals;
                        if (std::chrono::system_clock::now() - start > total_duration * 1s)
                        {
                            BOOST_LOG_TRIVIAL(info) << "Timeout (" << total_duration << "s). Closing client\n";
                            return;
                        }

                        slsfs::pack::packet_pointer request = slsfs::client::packet_create::write(anyname(), buf);
                        std::string response = slsfs_client.send(request);

                        if (response.empty())
                            continue;

                        BOOST_LOG_TRIVIAL(trace) << response << "\n";
                        counter++;
                    }
                    return;
                } catch (boost::exception const& e) {
                    BOOST_LOG_TRIVIAL(error) << "boost exception catched at client " << i << " " << boost::diagnostic_information(e);
                }
            });

    for (std::jthread& th : pool)
        th.join();

    auto end = std::chrono::system_clock::now();
    double const duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    BOOST_LOG_TRIVIAL(info) << "Finish " << counter.load() << " requests in " << duration_us / 1000 << "ms";
    BOOST_LOG_TRIVIAL(info) << "Throughput = "
                            << counter.load() * bufsize / duration_us * 1000000 / 1000 << " KBps";
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
#endif

    namespace po = boost::program_options;
    po::options_description desc{"Options"};
    desc.add_options()
        ("help,h", "Print this help messages")
        ("total-times",   po::value<int>()->default_value(10000),     "each client run # total times")
        ("total-duration",po::value<int>()->default_value(60*60),     "max duration for this client to live")
        ("total-clients", po::value<int>()->default_value(1),         "# of clients")
        ("bufsize",       po::value<int>()->default_value(4096),      "Size of the read/write buffer")
        ("zipf-alpha",    po::value<double>()->default_value(1.2),    "set the alpha value of zipf dist")
        ("file-range",    po::value<int>()->default_value(256*256*4), "set the total different number of files")
        ("test-name",     po::value<std::string>(),                   "oneof [fill, 50-50, 95-5, 100-0, 0-100, samename, samename-read]; format: read-write")
        ("uniform-dist",  po::bool_switch(),                          "use uniform distribution")
        ("zookeeper",     po::value<std::string>()->default_value("zk://zookeeper-1:2181"), "zookeeper host")
        ("result",        po::value<std::string>()->default_value("/dev/null"), "save result to this file");

    po::positional_options_description pos_po;
    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv)
              .options(desc)
              .positional(pos_po).run(), vm);
    po::notify(vm);

    if (vm.count("help"))
    {
        BOOST_LOG_TRIVIAL(info) << desc;
        return EXIT_SUCCESS;
    }

    start_test(vm);
}
