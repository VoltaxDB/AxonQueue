#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include "../core/Task.hpp"
#include "../utils/DataStructures.hpp"

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

        void                      addDependency(Task::TaskId dependent, Task::TaskId dependency);
        void                      removeDependency(Task::TaskId dependent, Task::TaskId dependency);
        bool                      areDependenciesMet(Task::TaskId id) const;
        std::string               generateDependencyGraph(Task::TaskId rootId) const;
        std::vector<Task::TaskId> getCriticalPath(Task::TaskId rootId) const;

        void      setMode(QueueMode mode);
        QueueMode getMode() const;

        std::unordered_map<std::string, size_t> getQueueSizes() const;
        std::unordered_map<std::string, double> getMetrics() const;

       private:
        mutable std::shared_mutex queue_mutex_;
        mutable std::shared_mutex task_mutex_;
        mutable std::shared_mutex config_mutex_;

        QueueMode mode_{QueueMode::FIFO};

        std::unordered_map<Task::TaskId, std::shared_ptr<Task>> task_map_;

        std::atomic<size_t> total_tasks_{0};

        std::vector<std::string> group_names_;
        std::atomic<size_t>      current_group_index_{0};

        struct TaskNode : public utils::IntrusiveListNode<TaskNode>
        {
            std::shared_ptr<Task> task;
            explicit TaskNode(std::shared_ptr<Task> t) : task(t) {}
        };

        std::unordered_map<std::string, utils::LockFreeQueue<TaskNode>> fifo_queues_;

        std::unordered_map<std::string, utils::LockFreeStack<TaskNode>> lifo_queues_;

        std::unordered_map<std::string, utils::LockFreePriorityQueue<TaskNode>> priority_queues_;

        utils::TaskGraph dependency_graph_;

        std::atomic<uint64_t> enqueue_count_{0};
        std::atomic<uint64_t> dequeue_count_{0};
        std::atomic<uint64_t> peek_count_{0};
        std::atomic<uint64_t> cancel_count_{0};
        std::atomic<uint64_t> total_wait_time_ms_{0};
        std::atomic<uint64_t> total_execution_time_ms_{0};

        std::string           getNextRoundRobinGroup() const;
        void                  updateTaskInMap(std::shared_ptr<Task> task);
        void                  removeTaskFromMap(Task::TaskId id);
        std::shared_ptr<Task> getTaskFromFIFO(const std::string &group);
        std::shared_ptr<Task> getTaskFromLIFO(const std::string &group);
        std::shared_ptr<Task> getTaskFromPriority(const std::string &group);
        std::shared_ptr<Task> getTaskRoundRobin();
    };

}  // namespace taskqueuex