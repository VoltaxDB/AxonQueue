#include <iostream>
#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include "core/ThreadPool.hpp"
#include "persistence/Storage.hpp"
#include "network/TCPServer.hpp"

namespace po = boost::program_options;

int main(int argc, char* argv[]) {
    try {
        po::options_description desc("Allowed options");
        desc.add_options()
            ("help", "produce help message")
            ("host", po::value<std::string>()->default_value("0.0.0.0"), "bind address")
            ("port", po::value<uint16_t>()->default_value(8080), "bind port")
            ("threads", po::value<size_t>()->default_value(std::thread::hardware_concurrency()),
             "number of worker threads")
            ("db-path", po::value<std::string>()->default_value("taskqueue.db"),
             "path to RocksDB database")
            ("tls-cert", po::value<std::string>(), "path to TLS certificate")
            ("tls-key", po::value<std::string>(), "path to TLS private key");
            
        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
        
        if (vm.count("help")) {
            std::cout << desc << "\n";
            return 1;
        }
        
        auto storage = std::make_shared<taskqueuex::Storage>(
            vm["db-path"].as<std::string>()
        );
        
        if (!storage->initialize()) {
            std::cerr << "Failed to initialize storage\n";
            return 1;
        }
        
        auto thread_pool = std::make_shared<taskqueuex::ThreadPool>(
            vm["threads"].as<size_t>()
        );
        thread_pool->start();
        
        boost::asio::io_context io_context;
        
        auto server = std::make_shared<taskqueuex::TCPServer>(
            io_context,
            vm["host"].as<std::string>(),
            vm["port"].as<uint16_t>(),
            thread_pool->getQueueManager()
        );
        
        if (vm.count("tls-cert") && vm.count("tls-key")) {
            server->enableTLS(
                vm["tls-cert"].as<std::string>(),
                vm["tls-key"].as<std::string>()
            );
        }
        
        server->start();
        
        std::cout << "TaskQueueX server started on "
                  << vm["host"].as<std::string>() << ":"
                  << vm["port"].as<uint16_t>() << "\n";
                  
        boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
        signals.async_wait(
            [&](const boost::system::error_code& /*error*/, int /*signal*/) {
                std::cout << "\nShutting down...\n";
                server->stop();
                thread_pool->stop();
                storage->shutdown();
                io_context.stop();
            }
        );
        
        io_context.run();
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
} 