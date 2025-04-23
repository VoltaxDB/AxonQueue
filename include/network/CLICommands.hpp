#pragma once

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <string>

#include "../core/QueueManager.hpp"
#include "../persistence/Storage.hpp"

namespace taskqueuex
{

    class CLICommands
    {
       public:
        CLICommands(std::shared_ptr<QueueManager> queue_manager, std::shared_ptr<Storage> storage);

        nlohmann::json enqueue(const nlohmann::json &params);
        nlohmann::json dequeue(const nlohmann::json &params);
        nlohmann::json run(const nlohmann::json &params);
        nlohmann::json cancel(const nlohmann::json &params);
        nlohmann::json kill(const nlohmann::json &params);
        nlohmann::json status(const nlohmann::json &params);
        nlohmann::json list(const nlohmann::json &params);
        nlohmann::json clear(const nlohmann::json &params);

        nlohmann::json setMode(const nlohmann::json &params);
        nlohmann::json setConcurrency(const nlohmann::json &params);
        nlohmann::json setRetry(const nlohmann::json &params);
        nlohmann::json setDelay(const nlohmann::json &params);
        nlohmann::json setSchedule(const nlohmann::json &params);

        nlohmann::json depend(const nlohmann::json &params);
        nlohmann::json graph(const nlohmann::json &params);
        nlohmann::json ready(const nlohmann::json &params);

        nlohmann::json connect(const nlohmann::json &params);
        nlohmann::json submit(const nlohmann::json &params);
        nlohmann::json exportTask(const nlohmann::json &params);
        nlohmann::json importTask(const nlohmann::json &params);

        nlohmann::json logs(const nlohmann::json &params);
        nlohmann::json events(const nlohmann::json &params);
        nlohmann::json metrics(const nlohmann::json &params);
        nlohmann::json watch(const nlohmann::json &params);

        nlohmann::json flush(const nlohmann::json &params);
        nlohmann::json replay(const nlohmann::json &params);
        nlohmann::json backup(const nlohmann::json &params);
        nlohmann::json restore(const nlohmann::json &params);
        nlohmann::json version(const nlohmann::json &params);

        nlohmann::json mock(const nlohmann::json &params);
        nlohmann::json simulate(const nlohmann::json &params);
        nlohmann::json benchmark(const nlohmann::json &params);

       private:
        std::shared_ptr<QueueManager> queue_manager_;
        std::shared_ptr<Storage>      storage_;

        mutable std::shared_mutex config_mutex_;   // For configuration changes
        mutable std::shared_mutex task_mutex_;     // For task operations
        mutable std::shared_mutex graph_mutex_;    // For dependency graph operations
        mutable std::shared_mutex metrics_mutex_;  // For metrics collection

        static constexpr size_t               STORAGE_POOL_SIZE = 32;
        std::vector<std::shared_ptr<Storage>> storage_pool_;
        std::atomic<size_t>                   storage_pool_index_{0};

        std::string                           generateDOTGraph(Task::TaskId root_id);
        bool                                  validateCronExpression(const std::string &cron);
        std::chrono::system_clock::time_point parseScheduleTime(const std::string &schedule);
        void logEvent(Task::TaskId id, const std::string &event, const std::string &details);

        std::shared_ptr<Storage> getStorageFromPool();
        void                     initializeStoragePool();

        template <typename T>
        std::vector<T> batchProcess(const std::vector<T>  &items,
                                    std::function<void(T)> processor,
                                    size_t                 batch_size = 100);
    };

}  // namespace taskqueuex