#include "network/CLICommands.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <regex>
#include <thread>

namespace taskqueuex {

CLICommands::CLICommands(std::shared_ptr<QueueManager> queue_manager,
                        std::shared_ptr<Storage> storage)
    : queue_manager_(queue_manager)
    , storage_(storage) {
    initializeStoragePool();
}

void CLICommands::initializeStoragePool() {
    storage_pool_.reserve(STORAGE_POOL_SIZE);
    for (size_t i = 0; i < STORAGE_POOL_SIZE; ++i) {
        storage_pool_.push_back(std::make_shared<Storage>(storage_->getConfig()));
    }
}

std::shared_ptr<Storage> CLICommands::getStorageFromPool() {
    size_t index = storage_pool_index_.fetch_add(1, std::memory_order_relaxed) % STORAGE_POOL_SIZE;
    return storage_pool_[index];
}

template<typename T>
std::vector<T> CLICommands::batchProcess(const std::vector<T>& items,
                                        std::function<void(T)> processor,
                                        size_t batch_size) {
    std::vector<T> results;
    results.reserve(items.size());
    
    std::vector<std::thread> threads;
    std::mutex results_mutex;
    
    for (size_t i = 0; i < items.size(); i += batch_size) {
        size_t end = std::min(i + batch_size, items.size());
        threads.emplace_back([&, i, end]() {
            std::vector<T> batch_results;
            for (size_t j = i; j < end; ++j) {
                processor(items[j]);
                batch_results.push_back(items[j]);
            }
            
            std::lock_guard<std::mutex> lock(results_mutex);
            results.insert(results.end(), batch_results.begin(), batch_results.end());
        });
    }
    
    for (auto& thread : threads) {
        thread.join();
    }
    
    return results;
}

// Queue Operations
nlohmann::json CLICommands::enqueue(const nlohmann::json& params) {
    try {
        std::shared_lock<std::shared_mutex> config_lock(config_mutex_);
        std::unique_lock<std::shared_mutex> task_lock(task_mutex_);
        
        auto task = std::make_shared<Task>(
            params["name"].get<std::string>(),
            static_cast<TaskPriority>(params.value("priority", 50))
        );
        
        if (params.contains("delay")) {
            auto delay = std::chrono::milliseconds(params["delay"].get<int64_t>());
            task->scheduled_for_ = std::chrono::system_clock::now() + delay;
        }
        
        std::string group = params.value("group", "default");
        queue_manager_->enqueue(task, group);
        
        auto storage = getStorageFromPool();
        storage->saveTask(task);
        
        task_lock.unlock();
        logEvent(task->getId(), "created", "Task enqueued");
        
        return {
            {"status", "success"},
            {"task_id", task->getId()}
        };
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::dequeue(const nlohmann::json& /*params*/) {
    try {
        std::shared_lock<std::shared_mutex> task_lock(task_mutex_);
        
        auto task = queue_manager_->dequeue();
        if (!task) {
            return {{"status", "error"}, {"message", "No tasks available"}};
        }
        
        return {
            {"status", "success"},
            {"task", task->toJson()}
        };
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::run(const nlohmann::json& params) {
    try {
        std::unique_lock<std::shared_mutex> task_lock(task_mutex_);
        
        auto task_id = params["task_id"].get<Task::TaskId>();
        auto task = queue_manager_->findTask(task_id);
        
        if (!task) {
            return {{"status", "error"}, {"message", "Task not found"}};
        }
        
        task->markStarted();
        task_lock.unlock();  // Release lock during long-running execution
        
        task->execute();
        
        task_lock.lock();
        task->markCompleted();
        auto storage = getStorageFromPool();
        storage->updateTaskStatus(task_id, task->getStatus());
        
        task_lock.unlock();
        logEvent(task_id, "executed", "Task manually executed");
        
        return {
            {"status", "success"},
            {"task", task->toJson()}
        };
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::cancel(const nlohmann::json& params) {
    try {
        auto task_id = params["task_id"].get<Task::TaskId>();
        bool success = queue_manager_->cancelTask(task_id);
        
        if (success) {
            auto storage = getStorageFromPool();
            storage->updateTaskStatus(task_id, TaskStatus::CANCELLED);
            logEvent(task_id, "cancelled", "Task cancelled");
        }
        
        return {
            {"status", success ? "success" : "error"},
            {"message", success ? "Task cancelled" : "Failed to cancel task"}
        };
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::kill(const nlohmann::json& params) {
    try {
        auto task_id = params["task_id"].get<Task::TaskId>();
        auto task = queue_manager_->findTask(task_id);
        
        if (!task || task->getStatus() != TaskStatus::RUNNING) {
            return {{"status", "error"}, {"message", "Task not running"}};
        }
        
        task->markFailed("Task killed by user");
        auto storage = getStorageFromPool();
        storage->updateTaskStatus(task_id, TaskStatus::FAILED);
        logEvent(task_id, "killed", "Task forcefully terminated");
        
        return {{"status", "success"}};
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::status(const nlohmann::json& params) {
    try {
        auto task_id = params["task_id"].get<Task::TaskId>();
        auto task = queue_manager_->findTask(task_id);
        
        if (!task) {
            return {{"status", "error"}, {"message", "Task not found"}};
        }
        
        return {
            {"status", "success"},
            {"task", task->toJson()}
        };
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::list(const nlohmann::json& params) {
    try {
        std::shared_lock<std::shared_mutex> task_lock(task_mutex_);
        
        std::string state = params.value("state", "all");
        auto storage = getStorageFromPool();
        auto tasks = storage->loadAllTasks();
        
        // Release lock before processing
        task_lock.unlock();
        
        // Process tasks in parallel batches
        auto filtered_tasks = batchProcess<std::shared_ptr<Task>>(
            tasks,
            [&](std::shared_ptr<Task> task) {
                if (state == "all" ||
                    (state == "queued" && task->getStatus() == TaskStatus::QUEUED) ||
                    (state == "running" && task->getStatus() == TaskStatus::RUNNING) ||
                    (state == "completed" && task->getStatus() == TaskStatus::COMPLETED) ||
                    (state == "failed" && task->getStatus() == TaskStatus::FAILED)) {
                    return task->toJson();
                }
                return nlohmann::json(nullptr);
            },
            100  // Process in batches of 100
        );
        
        return {
            {"status", "success"},
            {"tasks", filtered_tasks}
        };
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::clear(const nlohmann::json& params) {
    try {
        std::string scope = params.value("scope", "all");
        
        if (scope == "all") {
            queue_manager_->clear();
        }
        
        return {{"status", "success"}};
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::setMode(const nlohmann::json& params) {
    try {
        std::unique_lock<std::shared_mutex> config_lock(config_mutex_);
        
        std::string mode_str = params["mode"].get<std::string>();
        QueueMode mode;
        
        if (mode_str == "FIFO") mode = QueueMode::FIFO;
        else if (mode_str == "LIFO") mode = QueueMode::LIFO;
        else if (mode_str == "ROUND_ROBIN") mode = QueueMode::ROUND_ROBIN;
        else if (mode_str == "PRIORITY") mode = QueueMode::PRIORITY;
        else throw std::invalid_argument("Invalid queue mode");
        
        queue_manager_->setMode(mode);
        return {{"status", "success"}};
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::setConcurrency(const nlohmann::json& params) {
    try {
        size_t threads = params["threads"].get<size_t>();
        return {{"status", "error"}, {"message", "Not implemented"}};
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::setRetry(const nlohmann::json& params) {
    try {
        auto task_id = params["task_id"].get<Task::TaskId>();
        auto retries = params["count"].get<uint32_t>();
        
        auto task = queue_manager_->findTask(task_id);
        if (!task) {
            return {{"status", "error"}, {"message", "Task not found"}};
        }
        
        task->setMaxRetries(retries);
        auto storage = getStorageFromPool();
        storage->saveTask(task);
        
        return {{"status", "success"}};
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::setDelay(const nlohmann::json& params) {
    try {
        auto task_id = params["task_id"].get<Task::TaskId>();
        auto delay_ms = params["ms"].get<int64_t>();
        
        auto task = queue_manager_->findTask(task_id);
        if (!task) {
            return {{"status", "error"}, {"message", "Task not found"}};
        }
        
        task->scheduled_for_ = std::chrono::system_clock::now() +
                              std::chrono::milliseconds(delay_ms);
        auto storage = getStorageFromPool();
        storage->saveTask(task);
        
        return {{"status", "success"}};
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::setSchedule(const nlohmann::json& params) {
    try {
        auto task_id = params["task_id"].get<Task::TaskId>();
        auto schedule = params["cron"].get<std::string>();
        
        if (!validateCronExpression(schedule)) {
            return {{"status", "error"}, {"message", "Invalid cron expression"}};
        }
        
        auto task = queue_manager_->findTask(task_id);
        if (!task) {
            return {{"status", "error"}, {"message", "Task not found"}};
        }
        
        auto json = task->toJson();
        json["schedule"] = schedule;
        task->fromJson(json);
        auto storage = getStorageFromPool();
        storage->saveTask(task);
        
        return {{"status", "success"}};
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::depend(const nlohmann::json& params) {
    try {
        std::unique_lock<std::shared_mutex> graph_lock(graph_mutex_);
        
        auto task_id = params["task_id"].get<Task::TaskId>();
        auto dep_id = params["on_task_id"].get<Task::TaskId>();
        
        auto task = queue_manager_->findTask(task_id);
        auto dep_task = queue_manager_->findTask(dep_id);
        
        if (!task || !dep_task) {
            return {{"status", "error"}, {"message", "Task not found"}};
        }
        
        task->addDependency(dep_task);
        auto storage = getStorageFromPool();
        storage->saveTask(task);
        
        return {{"status", "success"}};
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::graph(const nlohmann::json& params) {
    try {
        auto task_id = params["task_id"].get<Task::TaskId>();
        std::string dot = generateDOTGraph(task_id);
        
        return {
            {"status", "success"},
            {"graph", dot}
        };
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::ready(const nlohmann::json& /*params*/) {
    try {
        auto tasks = storage_->loadAllTasks();
        std::vector<nlohmann::json> ready_tasks;
        
        for (const auto& task : tasks) {
            if (task->getStatus() == TaskStatus::QUEUED &&
                task->areDependenciesMet() &&
                task->getScheduledFor() <= std::chrono::system_clock::now()) {
                ready_tasks.push_back(task->toJson());
            }
        }
        
        return {
            {"status", "success"},
            {"tasks", ready_tasks}
        };
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::connect(const nlohmann::json& /*params*/) {
    return {{"status", "error"}, {"message", "Command should be handled by client"}};
}

nlohmann::json CLICommands::submit(const nlohmann::json& params) {
    try {
        std::string filename = params["file"].get<std::string>();
        std::ifstream file(filename);
        if (!file) {
            return {{"status", "error"}, {"message", "Cannot open file"}};
        }
        
        nlohmann::json task_def = nlohmann::json::parse(file);
        return enqueue(task_def);
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::exportTask(const nlohmann::json& params) {
    try {
        auto task_id = params["task_id"].get<Task::TaskId>();
        auto task = queue_manager_->findTask(task_id);
        
        if (!task) {
            return {{"status", "error"}, {"message", "Task not found"}};
        }
        
        std::string filename = "task_" + std::to_string(task_id) + ".json";
        std::ofstream file(filename);
        file << task->toJson().dump(2);
        
        return {
            {"status", "success"},
            {"file", filename}
        };
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

nlohmann::json CLICommands::importTask(const nlohmann::json& params) {
    try {
        std::string filename = params["file"].get<std::string>();
        std::ifstream file(filename);
        if (!file) {
            return {{"status", "error"}, {"message", "Cannot open file"}};
        }
        
        nlohmann::json task_def = nlohmann::json::parse(file);
        auto task = std::make_shared<Task>("", TaskPriority::MEDIUM);
        task->fromJson(task_def);
        
        auto storage = getStorageFromPool();
        storage->saveTask(task);
        queue_manager_->enqueue(task);
        
        return {
            {"status", "success"},
            {"task_id", task->getId()}
        };
    } catch (const std::exception& e) {
        return {{"status", "error"}, {"message", e.what()}};
    }
}

std::string CLICommands::generateDOTGraph(Task::TaskId root_id) {
    std::stringstream dot;
    dot << "digraph TaskDependencies {\n";
    
    std::function<void(Task::TaskId)> addNode = [&](Task::TaskId id) {
        if (auto task = queue_manager_->findTask(id)) {
            std::string color;
            switch (task->getStatus()) {
                case TaskStatus::QUEUED: color = "yellow"; break;
                case TaskStatus::RUNNING: color = "blue"; break;
                case TaskStatus::COMPLETED: color = "green"; break;
                case TaskStatus::FAILED: color = "red"; break;
                default: color = "gray";
            }
            
            dot << "  task_" << id << " [label=\"" << task->getName()
                << "\\nID: " << id << "\", color=" << color << "];\n";
                
            for (const auto& dep : task->dependencies_) {
                if (auto dep_task = dep.lock()) {
                    dot << "  task_" << id << " -> task_" << dep_task->getId() << ";\n";
                    addNode(dep_task->getId());
                }
            }
        }
    };
    
    addNode(root_id);
    dot << "}\n";
    return dot.str();
}

bool CLICommands::validateCronExpression(const std::string& cron) {
    std::regex cron_regex(R"(^(\S+\s+){4}\S+$)");
    return std::regex_match(cron, cron_regex);
}

void CLICommands::logEvent(Task::TaskId id, const std::string& event,
                          const std::string& details) {
    nlohmann::json log_entry = {
        {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()},
        {"task_id", id},
        {"event", event},
        {"details", details}
    };
    
    auto storage = getStorageFromPool();
    storage->appendToWAL("event_log", log_entry);
}

} // namespace taskqueuex 