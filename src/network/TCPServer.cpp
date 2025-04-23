#include "network/TCPServer.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <boost/bind.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace taskqueuex {

// Session implementation
TCPServer::Session::Session(tcp::socket socket, TCPServer& server)
    : socket_(std::move(socket))
    , server_(server)
    , last_activity_(std::chrono::steady_clock::now()) {
}

TCPServer::Session::~Session() {
    stop();
}

void TCPServer::Session::start() {
    active_ = true;
    readHeader();
}

void TCPServer::Session::stop() {
    if (active_.exchange(false)) {
        boost::system::error_code ec;
        socket_.close(ec);
    }
}

bool TCPServer::Session::isActive() const {
    return active_ && 
           (std::chrono::steady_clock::now() - last_activity_) < std::chrono::minutes(5);
}

void TCPServer::Session::readHeader() {
    if (!active_) return;
    
    auto self = shared_from_this();
    boost::asio::async_read_until(
        socket_, 
        boost::asio::dynamic_buffer(read_buffer_), 
        "\r\n\r\n",
        [this, self](boost::system::error_code ec, std::size_t length) {
            if (!ec) {
                // Parse headers
                std::string header = read_buffer_.substr(0, length);
                read_buffer_.erase(0, length);
                
                // Extract content length
                size_t content_length = 0;
                size_t pos = header.find("Content-Length:");
                if (pos != std::string::npos) {
                    std::string len_str = header.substr(pos + 15);
                    content_length = std::stoul(len_str);
                }
                
                if (content_length > 0) {
                    readBody(content_length);
                } else {
                    handleMessage("{}");
                }
            } else {
                stop();
            }
        });
}

void TCPServer::Session::readBody(size_t content_length) {
    if (!active_) return;
    
    auto self = shared_from_this();
    
    if (read_buffer_.size() >= content_length) {
        // Already have enough data
        std::string body = read_buffer_.substr(0, content_length);
        read_buffer_.erase(0, content_length);
        handleMessage(body);
        return;
    }
    
    // Need to read more
    boost::asio::async_read(
        socket_,
        boost::asio::dynamic_buffer(read_buffer_),
        boost::asio::transfer_at_least(content_length - read_buffer_.size()),
        [this, self, content_length](boost::system::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                std::string body = read_buffer_.substr(0, content_length);
                read_buffer_.erase(0, content_length);
                handleMessage(body);
            } else {
                stop();
            }
        });
}

void TCPServer::Session::handleMessage(const std::string& message) {
    if (!active_) return;
    
    last_activity_ = std::chrono::steady_clock::now();
    auto start_time = std::chrono::high_resolution_clock::now();
    request_count_++;
    
    json response;
    try {
        json request = json::parse(message);
        
        if (request.contains("command") && request["command"].is_string()) {
            std::string command = request["command"];
            json params = request.value("params", json::object());
            
            // Queue the command execution to the thread pool
            server_.queueWork([this, command, params, start_time]() {
                json cmd_response = server_.executeCommand(command, params);
                
                auto end_time = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end_time - start_time).count();
                
                server_.updateRequestMetrics(duration);
                
                // Send response back
                writeResponse(cmd_response);
            });
            return;
        } else {
            response = {
                {"status", "error"},
                {"message", "Invalid request format - missing command"}
            };
        }
    } catch (const std::exception& e) {
        response = {
            {"status", "error"},
            {"message", std::string("Error parsing request: ") + e.what()}
        };
    }
    
    // Handle errors immediately rather than queueing
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();
    
    server_.updateRequestMetrics(duration);
    writeResponse(response);
}

void TCPServer::Session::writeResponse(const json& response) {
    if (!active_) return;
    
    auto self = shared_from_this();
    std::string response_str = response.dump();
    
    std::unique_lock<std::mutex> lock(write_mutex_);
    std::string message = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(response_str.size()) + "\r\n"
        "Connection: keep-alive\r\n"
        "\r\n" + 
        response_str;
    
    boost::asio::async_write(
        socket_,
        boost::asio::buffer(message),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                readHeader(); // Ready for next message
            } else {
                stop();
            }
        });
}

TCPServer::TCPServer(boost::asio::io_context& io_context,
                    const std::string& address,
                    unsigned short port,
                    std::shared_ptr<QueueManager> queue_manager,
                    std::shared_ptr<Storage> storage)
    : address_(address)
    , port_(port)
    , io_context_(io_context)
    , acceptor_(io_context)
    , queue_manager_(queue_manager)
    , storage_(storage) {
    
    cli_commands_ = std::make_shared<CLICommands>(queue_manager_, storage_);
    
    boost::asio::ip::tcp::endpoint endpoint(
        boost::asio::ip::address::from_string(address), port);
    
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    
    registerDefaultHandlers();
}

TCPServer::~TCPServer() {
    stop();
}

void TCPServer::start() {
    if (running_.exchange(true)) return;
    
    acceptConnection();
    startWorkerThreads();
    
    std::thread([this]() {
        monitorConnections();
    }).detach();
}

void TCPServer::stop() {
    if (!running_.exchange(false)) return;
    
    boost::system::error_code ec;
    acceptor_.close(ec);
    
    std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
    for (auto& session_pair : sessions_) {
        if (auto session = session_pair.first) {
            session->stop();
        }
    }
    sessions_.clear();
    
    stopWorkerThreads();
}

bool TCPServer::isRunning() const {
    return running_;
}

void TCPServer::configureTLS(const std::string& cert_file, const std::string& key_file) {
    try {
        ssl_context_ = std::make_unique<boost::asio::ssl::context>(
            boost::asio::ssl::context::sslv23_server);
        
        ssl_context_->set_options(
            boost::asio::ssl::context::default_workarounds |
            boost::asio::ssl::context::no_sslv2 |
            boost::asio::ssl::context::no_sslv3);
            
        ssl_context_->use_certificate_chain_file(cert_file);
        ssl_context_->use_private_key_file(key_file, boost::asio::ssl::context::pem);
        
        tls_enabled_ = true;
    } catch (const std::exception& e) {
        std::cerr << "TLS configuration error: " << e.what() << std::endl;
        ssl_context_.reset();
        tls_enabled_ = false;
    }
}

bool TCPServer::isTLSEnabled() const {
    return tls_enabled_;
}

void TCPServer::registerCommand(const std::string& name, CommandHandler handler) {
    std::unique_lock<std::shared_mutex> lock(commands_mutex_);
    command_handlers_[name] = handler;
}

json TCPServer::executeCommand(const std::string& name, const json& params) {
    {
        std::shared_lock<std::shared_mutex> lock(commands_mutex_);
        auto it = command_handlers_.find(name);
        if (it != command_handlers_.end()) {
            try {
                return it->second(params);
            } catch (const std::exception& e) {
                return {
                    {"status", "error"},
                    {"message", std::string("Command execution error: ") + e.what()}
                };
            }
        }
    }
    
    try {
        if (name == "enqueue") return cli_commands_->enqueue(params);
        else if (name == "dequeue") return cli_commands_->dequeue(params);
        else if (name == "run") return cli_commands_->run(params);
        else if (name == "cancel") return cli_commands_->cancel(params);
        else if (name == "kill") return cli_commands_->kill(params);
        else if (name == "status") return cli_commands_->status(params);
        else if (name == "list") return cli_commands_->list(params);
        else if (name == "clear") return cli_commands_->clear(params);
        else if (name == "setMode") return cli_commands_->setMode(params);
        else if (name == "setConcurrency") return cli_commands_->setConcurrency(params);
        else if (name == "setRetry") return cli_commands_->setRetry(params);
        else if (name == "setDelay") return cli_commands_->setDelay(params);
        else if (name == "setSchedule") return cli_commands_->setSchedule(params);
        else if (name == "depend") return cli_commands_->depend(params);
        else if (name == "graph") return cli_commands_->graph(params);
        else if (name == "ready") return cli_commands_->ready(params);
        else if (name == "connect") return cli_commands_->connect(params);
        else if (name == "submit") return cli_commands_->submit(params);
        else if (name == "exportTask") return cli_commands_->exportTask(params);
        else if (name == "importTask") return cli_commands_->importTask(params);
    } catch (const std::exception& e) {
        return {
            {"status", "error"},
            {"message", std::string("Command execution error: ") + e.what()}
        };
    }
    
    return {
        {"status", "error"},
        {"message", "Unknown command: " + name}
    };
}

size_t TCPServer::getActiveConnections() const {
    return active_connections_.load();
}

void TCPServer::cleanupInactiveSessions() {
    std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
    auto it = sessions_.begin();
    
    while (it != sessions_.end()) {
        auto session = it->first;
        if (!session || !session->isActive()) {
            it = sessions_.erase(it);
            active_connections_--;
        } else {
            ++it;
        }
    }
}

std::unordered_map<std::string, double> TCPServer::getMetrics() const {
    std::unordered_map<std::string, double> metrics;
    
    metrics["active_connections"] = active_connections_.load();
    metrics["total_connections"] = total_connections_.load();
    metrics["total_requests"] = total_requests_.load();
    metrics["request_errors"] = request_errors_.load();
    
    uint64_t total_reqs = total_requests_.load();
    metrics["avg_response_time_ms"] = (total_reqs > 0) ?
        static_cast<double>(total_response_time_ms_.load()) / total_reqs : 0.0;
    
    auto queue_metrics = queue_manager_->getMetrics();
    for (const auto& [key, value] : queue_metrics) {
        metrics[key] = value;
    }
    
    return metrics;
}

void TCPServer::acceptConnection() {
    if (!running_) return;
    
    acceptor_.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket) {
            handleAccept(ec, std::move(socket));
        });
}

void TCPServer::handleAccept(const boost::system::error_code& error, tcp::socket socket) {
    if (!running_) return;
    
    if (!error) {
        auto session = std::make_shared<Session>(std::move(socket), *this);
        
        {
            std::unique_lock<std::shared_mutex> lock(sessions_mutex_);
            sessions_[session] = session;
            active_connections_++;
            total_connections_++;
        }
        
        session->start();
    }
    
    acceptConnection(); // Continue accepting connections
}

void TCPServer::registerDefaultHandlers() {
    registerCommand("metrics", [this](const json& /*params*/) {
        return json(getMetrics());
    });
    
}

void TCPServer::updateRequestMetrics(uint64_t response_time_ms) {
    total_requests_++;
    total_response_time_ms_ += response_time_ms;
}

void TCPServer::monitorConnections() {
    while (running_) {
        cleanupInactiveSessions();
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}

void TCPServer::startWorkerThreads() {
    workers_running_ = true;
    
    worker_threads_.reserve(WORKER_THREADS);
    for (size_t i = 0; i < WORKER_THREADS; ++i) {
        worker_threads_.emplace_back(&TCPServer::workerThreadFunction, this);
    }
}

void TCPServer::stopWorkerThreads() {
    {
        std::unique_lock<std::mutex> lock(work_queue_mutex_);
        workers_running_ = false;
        work_queue_.clear();
    }
    
    work_condition_.notify_all();
    
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    
    worker_threads_.clear();
}

void TCPServer::workerThreadFunction() {
    while (workers_running_) {
        std::function<void()> work;
        
        {
            std::unique_lock<std::mutex> lock(work_queue_mutex_);
            work_condition_.wait(lock, [this]() {
                return !workers_running_ || !work_queue_.empty();
            });
            
            if (!workers_running_ && work_queue_.empty()) {
                return;
            }
            
            if (!work_queue_.empty()) {
                work = std::move(work_queue_.back());
                work_queue_.pop_back();
            }
        }
        
        if (work) {
            work();
        }
    }
}

void TCPServer::queueWork(std::function<void()> work) {
    {
        std::unique_lock<std::mutex> lock(work_queue_mutex_);
        work_queue_.push_back(std::move(work));
    }
    
    work_condition_.notify_one();
}

} // namespace taskqueuex 