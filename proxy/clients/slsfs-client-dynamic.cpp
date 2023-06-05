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
        ("thread",        po::value<int>()->default_value(std::thread::hardware_concurrency()), "# of thread")
        ("uniform-dist",  po::bool_switch(),                          "use uniform distribution")
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

    std::mt19937 engine(19937);

    int    const total_times     = vm["total-times"].as<int>();
    //int    const total_duration  = vm["total-duration"].as<int>();
    int    const bufsize         = vm["bufsize"].as<int>();
    double const zipf_alpha      = vm["zipf-alpha"].as<double>();
    int    const file_range      = vm["file-range"].as<int>();
    //bool   const use_uniform     = vm["uniform-dist"].as<bool>();
    int    const worker          = vm["thread"].as<int>();
    std::string const test_name  = vm["test-name"].as<std::string>();
    std::string const resultfile = vm["result"].as<std::string>();

    absl::zipf_distribution namedist(file_range, zipf_alpha);
    std::uniform_int_distribution<int> uniformdist(0, file_range), singledist(0, 255);

    auto anyname = [&engine, &singledist] {
        slsfs::pack::key_t t{};
        for (slsfs::pack::unit_t& n : t)
            n = singledist(engine);
        return t;
    };

    std::vector<std::jthread> pool;
    for (int i = 0; i < worker; i++)
        pool.emplace_back(
            [total_times, worker, bufsize, &anyname] {
                boost::asio::io_context io_context;
                slsfs::client::client slsfs_client{io_context, "zk://zookeeper-1:2181"};

                std::string buf(bufsize, 'A');
                for (int i = 0; i < total_times/worker; i++)
                {
                    slsfs::pack::packet_pointer request = slsfs::client::packet_create::write(anyname(), buf);
                    std::string response = slsfs_client.send(request);
                    BOOST_LOG_TRIVIAL(info) << response << "\n";
                    //using namespace std::chrono_literals;
                    //std::this_thread::sleep_for(200ms);
                }
            });
}
