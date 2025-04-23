#pragma once

#include <string>
#include <memory>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <nlohmann/json.hpp>
#include "../core/QueueManager.hpp"
#include "CLICommands.hpp"

namespace taskqueuex {

using boost::asio::ip::tcp;
using json = nlohmann::json;

class TCPServer {
public:

    using CommandHandler = std::function<json(const json&)>;
    
    class Session : public std::enable_shared_from_this<Session> {
    public:
        Session(tcp::socket socket, TCPServer& server);
        ~Session();
        
        void start();
        void stop();
        bool isActive() const;
        
    private:
        void readHeader();
        void readBody(size_t content_length);
        void handleMessage(const std::string& message);
        void writeResponse(const json& response);
        
        tcp::socket socket_;
        TCPServer& server_;
        std::string read_buffer_;
        std::atomic<bool> active_{false};
        std::atomic<uint64_t> request_count_{0};
        std::chrono::steady_clock::time_point last_activity_;
        std::mutex write_mutex_;
    };
    
    TCPServer(boost::asio::io_context& io_context, 
              const std::string& address, 
              unsigned short port,
              std::shared_ptr<QueueManager> queue_manager,
              std::shared_ptr<Storage> storage);
    
    ~TCPServer();
    
    void start();
    void stop();
    bool isRunning() const;
    
    void configureTLS(const std::string& cert_file, const std::string& key_file);
    bool isTLSEnabled() const;
    
    void registerCommand(const std::string& name, CommandHandler handler);
    json executeCommand(const std::string& name, const json& params);
    
    size_t getActiveConnections() const;
    void cleanupInactiveSessions();
    
    std::unordered_map<std::string, double> getMetrics() const;
    
private:
    std::string address_;
    unsigned short port_;
    std::atomic<bool> running_{false};
    std::atomic<bool> tls_enabled_{false};
    
    boost::asio::io_context& io_context_;
    tcp::acceptor acceptor_;
    std::unique_ptr<boost::asio::ssl::context> ssl_context_;
    
    mutable std::shared_mutex sessions_mutex_;
    mutable std::shared_mutex commands_mutex_;
    mutable std::shared_mutex metrics_mutex_;
    
    std::unordered_map<std::shared_ptr<Session>, std::weak_ptr<Session>> sessions_;
    
    std::unordered_map<std::string, CommandHandler> command_handlers_;
    std::shared_ptr<CLICommands> cli_commands_;
    
    std::shared_ptr<QueueManager> queue_manager_;
    std::shared_ptr<Storage> storage_;
    
    std::atomic<uint64_t> total_connections_{0};
    std::atomic<uint64_t> active_connections_{0};
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> request_errors_{0};
    std::atomic<uint64_t> total_response_time_ms_{0};
    
    void acceptConnection();
    void handleAccept(const boost::system::error_code& error, tcp::socket socket);
    void registerDefaultHandlers();
    void updateRequestMetrics(uint64_t response_time_ms);
    void monitorConnections();
    
    static const size_t WORKER_THREADS = 32;
    std::vector<std::thread> worker_threads_;
    std::mutex work_queue_mutex_;
    std::condition_variable work_condition_;
    std::vector<std::function<void()>> work_queue_;
    std::atomic<bool> workers_running_{false};
    
    void startWorkerThreads();
    void stopWorkerThreads();
    void workerThreadFunction();
    void queueWork(std::function<void()> work);
};

} // namespace taskqueuex 