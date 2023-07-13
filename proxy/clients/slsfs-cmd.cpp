#include "../basic.hpp"
#include "../serializer.hpp"
#include "../json-replacement.hpp"
#include "../uuid.hpp"
#include "clientlib.hpp"
#include "clientlib-client-pool.hpp"

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

void run (slsfs::client::client& slsfs_client)
{
    while (true)
    {
        std::string cmd, arg1, arg2;
        std::cout << "cmd> ";
        std::cin >> cmd >> arg1;

        slsfs::pack::packet_pointer request = nullptr;

        switch (slsfs::basic::sswitcher::hash(cmd))
        {
            using namespace slsfs::basic::sswitcher;
        case "ls"_:
            request = slsfs::client::packet_create::ls(arg1);
            break;
        case "mkdir"_:
            request = slsfs::client::packet_create::mkdir(arg1);
            break;
        case "addfile"_:
            std::cin >> arg2;
            request = slsfs::client::packet_create::addfile(arg1, arg2);
            break;
        case "write"_:
            std::cin >> arg2;
            request = slsfs::client::packet_create::write(arg1, arg2);
            break;
        case "read"_:
            std::cin >> arg2;
            request = slsfs::client::packet_create::read(arg1, std::stoi(arg2));
            break;
        default:
            BOOST_LOG_TRIVIAL(error) << "unknown cmd '" << cmd << "'";
            return;
        }

        std::cout << slsfs_client.send(request) << "\n";
    }
}

int main(int argc, char *argv[])
{
    slsfs::basic::init_log();
    std::string verbosity_values;

    namespace po = boost::program_options;
    po::options_description desc{"Options"};
    desc.add_options()
        ("help,h", "Print this help messages")
        ("verbose,v",  po::value<std::string>(&verbosity_values)->implicit_value(""),"log verbosity")
        ("zookeeper", po::value<std::string>()->default_value("zk://zookeeper-1:2181"), "zookeeper host");

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
    if (vm.count("verbosity"))
        verbosity_values += "v";

    int const verbosity = verbosity_values.size();
    boost::log::trivial::severity_level const level = boost::log::trivial::info;
    slsfs::basic::init_log(static_cast<boost::log::trivial::severity_level>(level - static_cast<boost::log::trivial::severity_level>(verbosity)));
    BOOST_LOG_TRIVIAL(debug) << "set verbosity=" << verbosity;

    boost::asio::io_context io_context;
    slsfs::client::client slsfs_client{io_context, vm["zookeeper"].as<std::string>()};

    boost::asio::signal_set listener(io_context, SIGINT, SIGTERM);
    listener.async_wait(
        [&io_context] (boost::system::error_code const&, int signal_number) {
            BOOST_LOG_TRIVIAL(debug) << "Stopping... sig=" << signal_number;
            io_context.stop();
        });

    //slsfs::client::client_pool<slsfs::client::client> pool{10, "zk://zookeeper-1:2181"};
    run(slsfs_client);
    return EXIT_SUCCESS;
}
