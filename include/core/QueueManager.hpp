#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <queue>
#include <shared_mutex>
#include <stack>
#include <string>
#include <unordered_map>

#include "Task.hpp"

namespace taskqueuex
{

    enum class QueueMode
    {
        FIFO,
        LIFO,
        ROUND_ROBIN,
        PRIORITY
    };

    class QueueManager
    {
       public:
        QueueManager();
        ~QueueManager();

        void enqueue(std::shared_ptr<Task> task, const std::string &group = "default");
        std::shared_ptr<Task> dequeue();
        std::shared_ptr<Task> peek();
        size_t                size() const;
        bool                  empty() const;
        void                  clear();

        std::shared_ptr<Task> findTask(Task::TaskId id) const;
        bool                  cancelTask(Task::TaskId id);

        void      setMode(QueueMode mode);
        QueueMode getMode() const;

        std::unordered_map<std::string, size_t> getQueueSizes() const;
        std::unordered_map<std::string, double> getMetrics() const;

       private:
        std::atomic<size_t> total_size_{0};

        mutable std::shared_mutex queue_mutex_;
        mutable std::shared_mutex task_mutex_;
        mutable std::shared_mutex config_mutex_;

        std::unordered_map<std::string, std::queue<std::shared_ptr<Task>>> fifo_queues_;
        std::unordered_map<std::string, std::stack<std::shared_ptr<Task>>> lifo_queues_;
        std::unordered_map<std::string,
                           std::priority_queue<std::shared_ptr<Task>,
                                               std::vector<std::shared_ptr<Task>>,
                                               std::function<bool(const std::shared_ptr<Task> &,
                                                                  const std::shared_ptr<Task> &)>>>
            priority_queues_;

        std::unordered_map<Task::TaskId, std::shared_ptr<Task>> task_map_;

        QueueMode mode_{QueueMode::FIFO};

        mutable std::vector<std::string> group_names_;
        mutable size_t                   current_group_index_{0};

        std::atomic<uint64_t>                          enqueue_count_{0};
        std::atomic<uint64_t>                          dequeue_count_{0};
        std::atomic<uint64_t>                          peek_count_{0};
        std::atomic<uint64_t>                          cancel_count_{0};
        std::chrono::high_resolution_clock::time_point last_operation_time_;

        std::string getNextRoundRobinGroup() const;
        bool        taskExists(Task::TaskId id) const;
        void        updateTaskMap(std::shared_ptr<Task> task);
        void        removeTaskFromMap(Task::TaskId id);
    };

}  // namespace taskqueuex