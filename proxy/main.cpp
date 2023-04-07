#include "basic.hpp"
#include "serializer.hpp"
#include "trigger.hpp"
#include "launcher.hpp"
#include "zookeeper.hpp"
#include "socket-writer.hpp"
#include "uuid.hpp"
#include "server.hpp"

#include <boost/program_options.hpp>
#include <boost/log/trivial.hpp>
#include <boost/asio.hpp>
#include <boost/signals2.hpp>
#include <boost/format.hpp>

#include <oneapi/tbb/concurrent_unordered_map.h>
#include <oneapi/tbb/concurrent_queue.h>

#include <algorithm>
#include <iostream>

#include <memory>
#include <array>
#include <list>
#include <thread>
#include <vector>
#include <fstream>
#include <string>
#include <regex>

int main(int argc, char* argv[])
{
    using namespace slsfs::server;

    boost::log::trivial::severity_level level = boost::log::trivial::info;

    std::string verbosity_values;
    namespace po = boost::program_options;
    po::options_description desc{"Options"};
    desc.add_options()
        ("help,h", "Print this help messages")
        ("listen,l",  po::value<unsigned short>()->default_value(12000),            "listen on this port")
        ("verbose,v", po::value<std::string>(&verbosity_values)->implicit_value(""),"log verbosity")
        ("init",      po::bool_switch(),                                            "reset all system (clear zookeeper entries)")
        ("initint",   po::value<int>()->default_value(0),                           "reset all system with 0, 1")
        ("thread",    po::value<int>()->default_value(std::thread::hardware_concurrency()), "# of thread")
        ("announce",  po::value<std::string>(),                                     "announce this ip address for other proxy to connect")
        ("report",    po::value<std::string>()->default_value("/dev/null"),         "path to save report every seconds")
        ("policy-filetoworker",      po::value<std::string>(),                      "file to worker policy name")
        ("policy-filetoworker-args", po::value<std::string>()->default_value(""),   "file to worker policy name extra args")
        ("policy-launch",            po::value<std::string>(),                      "launch policy name")
        ("policy-launch-args",       po::value<std::string>()->default_value(""),   "launch policy name extra args")
        ("policy-keepalive",         po::value<std::string>(),                      "keepalive policy name")
        ("policy-keepalive-args",    po::value<std::string>()->default_value(""),   "keepalive policy name extra args")
        ("worker-config",            po::value<std::string>(),                      "worker config json file path to use")
        ("max-function-count",       po::value<int>()->default_value(0),            "marks the max random function name to use")
        ("blocksize",                po::value<int>()->default_value(4096),         "worker config blocksize");
    po::positional_options_description pos_po;
    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv)
              .options(desc)
              .positional(pos_po).run(), vm);
    po::notify(vm);

    if (vm.count("help"))
    {
        BOOST_LOG_TRIVIAL(info) << desc;
        return EXIT_FAILURE;
    }

    if (vm.count("verbosity"))
        verbosity_values += "v";

    int const verbosity = verbosity_values.size();

    slsfs::basic::init_log(static_cast<boost::log::trivial::severity_level>(level - static_cast<boost::log::trivial::severity_level>(verbosity)));
    BOOST_LOG_TRIVIAL(trace) << "set verbosity=" << verbosity;

    int const worker  = vm["thread"].as<int>();
    net::io_context ioc {worker};

    unsigned short const port  = vm["listen"].as<unsigned short>();
    std::string const announce = vm["announce"].as<std::string>();
    std::string const save_report = vm["report"].as<std::string>();
    int  const blocksize       = vm["blocksize"].as<int>();
    bool const init_cluster    = vm["init"].as<bool>();
    //slsfs::uuid::uuid server_id = slsfs::uuid::gen_uuid();
    slsfs::uuid::uuid server_id = slsfs::uuid::gen_uuid_static_seed(announce);

    std::string worker_config;
    {
        std::ifstream worker_configuration(vm["worker-config"].as<std::string>());

        if (!worker_configuration.is_open())
        {
            BOOST_LOG_TRIVIAL(fatal) << "config " <<  vm["worker-config"].as<std::string>() << "not found";
            return 1;
        }

        std::stringstream template_config;
        template_config << worker_configuration.rdbuf();
        worker_config = (boost::format(template_config.str()) % announce % port % blocksize).str();
    }

    tcp_server server{ioc, port, server_id, announce, save_report};

    set_policy_filetoworker(server, vm["policy-filetoworker"].as<std::string>(), vm["policy-filetoworker-args"].as<std::string>());
    set_policy_launch      (server, vm["policy-launch"]      .as<std::string>(), vm["policy-launch-args"]      .as<std::string>());
    set_policy_keepalive   (server, vm["policy-keepalive"]   .as<std::string>(), vm["policy-keepalive-args"]   .as<std::string>());
    server.set_worker_config(worker_config, vm["max-function-count"].as<int>());

    server.start_accept();
    BOOST_LOG_TRIVIAL(info) << server_id << " listen on " << port;

    std::vector<char> announce_buf;
    fmt::format_to(std::back_inserter(announce_buf), "{}:{}", announce, port);

    slsfs::zookeeper::zookeeper zoo {ioc, server.launcher(), server_id, announce_buf};

    if (init_cluster || vm["initint"].as<int>() != 0)
    {
        BOOST_LOG_TRIVIAL(info) << "init cluster + init zookeeper";
        zoo.reset();
    }
    else
        zoo.start_setup();

    net::signal_set listener(ioc, SIGINT, SIGTERM);
    listener.async_wait(
        [&ioc, &zoo] (boost::system::error_code const&, int signal_number) {
            BOOST_LOG_TRIVIAL(info) << "Stopping... sig=" << signal_number;
            zoo.shutdown();
            ioc.stop();
        });

    std::vector<std::thread> worker_threads;
    worker_threads.reserve(worker);
    for(int i = 1; i < worker; i++)
        worker_threads.emplace_back([&ioc] { ioc.run(); });
    ioc.run();

    for (std::thread& th : worker_threads)
        th.join();

    return EXIT_SUCCESS;
}
