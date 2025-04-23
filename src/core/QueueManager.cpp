#include "core/QueueManager.hpp"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <stdexcept>

namespace taskqueuex
{

    QueueManager::QueueManager()
    {
        mode_ = QueueMode::FIFO;
    }

    QueueManager::~QueueManager()
    {
        clear();
    }

    void QueueManager::enqueue(std::shared_ptr<Task> task, const std::string &group)
    {
        if (!task)
            return;

        task->enqueued_at_ = std::chrono::system_clock::now();

        auto start_time = std::chrono::high_resolution_clock::now();

        {
            std::unique_lock<std::shared_mutex> task_lock(task_mutex_);
            updateTaskInMap(task);
        }

        {
            std::unique_lock<std::shared_mutex> queue_lock(queue_mutex_);

            if (std::find(group_names_.begin(), group_names_.end(), group) == group_names_.end())
            {
                group_names_.push_back(group);
            }

            auto node = new TaskNode(task);

            switch (mode_)
            {
                case QueueMode::FIFO:
                    fifo_queues_[group].enqueue(node);
                    break;

                case QueueMode::LIFO:
                    lifo_queues_[group].push(node);
                    break;

                case QueueMode::PRIORITY:
                    priority_queues_[group].enqueue(node, static_cast<int>(task->getPriority()));
                    break;

                case QueueMode::ROUND_ROBIN:
                    fifo_queues_[group].enqueue(node);
                    break;
            }
        }

        enqueue_count_++;
        total_tasks_++;
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    }

    std::shared_ptr<Task> QueueManager::dequeue()
    {
        auto                  start_time = std::chrono::high_resolution_clock::now();
        std::shared_ptr<Task> task       = nullptr;

        {
            std::unique_lock<std::shared_mutex> queue_lock(queue_mutex_);

            if (empty())
            {
                return nullptr;
            }

            switch (mode_)
            {
                case QueueMode::FIFO:
                {
                    for (const auto &[group, _] : fifo_queues_)
                    {
                        task = getTaskFromFIFO(group);
                        if (task)
                            break;
                    }
                    break;
                }

                case QueueMode::LIFO:
                {
                    for (const auto &[group, _] : lifo_queues_)
                    {
                        task = getTaskFromLIFO(group);
                        if (task)
                            break;
                    }
                    break;
                }

                case QueueMode::ROUND_ROBIN:
                {
                    task = getTaskRoundRobin();
                    break;
                }

                case QueueMode::PRIORITY:
                {
                    int         highest_priority = std::numeric_limits<int>::min();
                    std::string selected_group;

                    for (const auto &[group, queue] : priority_queues_)
                    {
                        if (!queue.empty())
                        {
                            auto node = queue.peek();
                            if (node && node->task)
                            {
                                int priority = static_cast<int>(node->task->getPriority());
                                if (priority > highest_priority)
                                {
                                    highest_priority = priority;
                                    selected_group   = group;
                                }
                            }
                        }
                    }

                    if (!selected_group.empty())
                    {
                        task = getTaskFromPriority(selected_group);
                    }
                    break;
                }
            }
        }

        if (task)
        {
            task->dequeued_at_ = std::chrono::system_clock::now();

            dequeue_count_++;
            total_tasks_--;

            auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 task->dequeued_at_ - task->enqueued_at_)
                                 .count();
            total_wait_time_ms_ += wait_time;
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

        return task;
    }

    std::shared_ptr<Task> QueueManager::peek()
    {
        std::shared_lock<std::shared_mutex> queue_lock(queue_mutex_);

        peek_count_++;

        switch (mode_)
        {
            case QueueMode::FIFO:
            case QueueMode::ROUND_ROBIN:
            {
                for (const auto &[group, queue] : fifo_queues_)
                {
                    auto node = queue.peek();
                    if (node)
                        return node->task;
                }
                return nullptr;
            }

            case QueueMode::LIFO:
            {
                for (const auto &[group, stack] : lifo_queues_)
                {
                    auto node = stack.peek();
                    if (node)
                        return node->task;
                }
                return nullptr;
            }

            case QueueMode::PRIORITY:
            {
                int                   highest_priority = std::numeric_limits<int>::min();
                std::shared_ptr<Task> highest_task     = nullptr;

                for (const auto &[group, queue] : priority_queues_)
                {
                    auto node = queue.peek();
                    if (node && node->task)
                    {
                        int priority = static_cast<int>(node->task->getPriority());
                        if (priority > highest_priority)
                        {
                            highest_priority = priority;
                            highest_task     = node->task;
                        }
                    }
                }

                return highest_task;
            }
        }

        return nullptr;
    }

    size_t QueueManager::size() const
    {
        return total_tasks_.load(std::memory_order_relaxed);
    }

    bool QueueManager::empty() const
    {
        return size() == 0;
    }

    void QueueManager::clear()
    {
        std::unique_lock<std::shared_mutex> queue_lock(queue_mutex_);
        std::unique_lock<std::shared_mutex> task_lock(task_mutex_);

        fifo_queues_.clear();
        lifo_queues_.clear();
        priority_queues_.clear();

        task_map_.clear();

        total_tasks_ = 0;
    }

    std::shared_ptr<Task> QueueManager::findTask(Task::TaskId id) const
    {
        std::shared_lock<std::shared_mutex> task_lock(task_mutex_);

        auto it = task_map_.find(id);
        if (it != task_map_.end())
        {
            return it->second;
        }

        return nullptr;
    }

    bool QueueManager::cancelTask(Task::TaskId id)
    {
        std::unique_lock<std::shared_mutex> task_lock(task_mutex_);

        auto it = task_map_.find(id);
        if (it != task_map_.end() && it->second->getStatus() == TaskStatus::QUEUED)
        {
            it->second->markCancelled();
            removeTaskFromMap(id);
            cancel_count_++;
            total_tasks_--;
            return true;
        }

        return false;
    }

    void QueueManager::addDependency(Task::TaskId dependent, Task::TaskId dependency)
    {
        dependency_graph_.addDependency(dependent, dependency);

        std::shared_lock<std::shared_mutex> task_lock(task_mutex_);

        auto dep_task = findTask(dependent);
        auto req_task = findTask(dependency);

        if (dep_task && req_task)
        {
            dep_task->addDependency(req_task);
        }
    }

    void QueueManager::removeDependency(Task::TaskId dependent, Task::TaskId dependency)
    {
        dependency_graph_.removeDependency(dependent, dependency);
    }

    bool QueueManager::areDependenciesMet(Task::TaskId id) const
    {
        auto deps = dependency_graph_.getDependencies(id);
        if (deps.empty())
        {
            return true;
        }

        std::shared_lock<std::shared_mutex> task_lock(task_mutex_);

        for (auto dep_id : deps)
        {
            auto task = findTask(dep_id);
            if (!task || task->getStatus() != TaskStatus::COMPLETED)
            {
                return false;
            }
        }

        return true;
    }

    std::string QueueManager::generateDependencyGraph(Task::TaskId rootId) const
    {
        std::shared_lock<std::shared_mutex> task_lock(task_mutex_);

        auto subgraph = dependency_graph_.getSubgraph(rootId);

        std::unordered_map<Task::TaskId, std::string> names;
        std::unordered_map<Task::TaskId, TaskStatus>  statuses;

        for (const auto &[id, _] : subgraph)
        {
            auto task = findTask(id);
            if (task)
            {
                names[id]    = task->getName();
                statuses[id] = task->getStatus();
            }
        }

        return utils::generateDOTGraph(subgraph, names, statuses);
    }

    std::vector<Task::TaskId> QueueManager::getCriticalPath(Task::TaskId rootId) const
    {
        std::shared_lock<std::shared_mutex> task_lock(task_mutex_);

        auto subgraph = dependency_graph_.getSubgraph(rootId);

        return utils::findCriticalPath(subgraph);
    }

    void QueueManager::setMode(QueueMode mode)
    {
        std::unique_lock<std::shared_mutex> config_lock(config_mutex_);

        if (mode_ == mode)
        {
            return;
        }

        std::unique_lock<std::shared_mutex> queue_lock(queue_mutex_);

        std::vector<std::pair<std::string, std::shared_ptr<Task>>> all_tasks;

        switch (mode_)
        {
            case QueueMode::FIFO:
            case QueueMode::ROUND_ROBIN:
            {
                for (auto &[group, queue] : fifo_queues_)
                {
                    while (!queue.empty())
                    {
                        auto node = queue.dequeue();
                        if (node && node->task)
                        {
                            all_tasks.emplace_back(group, node->task);
                            delete node;
                        }
                    }
                }
                break;
            }

            case QueueMode::LIFO:
            {
                for (auto &[group, stack] : lifo_queues_)
                {
                    while (!stack.empty())
                    {
                        auto node = stack.pop();
                        if (node && node->task)
                        {
                            all_tasks.emplace_back(group, node->task);
                            delete node;
                        }
                    }
                }
                break;
            }

            case QueueMode::PRIORITY:
            {
                for (auto &[group, queue] : priority_queues_)
                {
                    while (!queue.empty())
                    {
                        auto node = queue.dequeue();
                        if (node && node->task)
                        {
                            all_tasks.emplace_back(group, node->task);
                            // Node is deleted inside dequeue
                        }
                    }
                }
                break;
            }
        }

        fifo_queues_.clear();
        lifo_queues_.clear();
        priority_queues_.clear();

        mode_ = mode;

        for (const auto &[group, task] : all_tasks)
        {
            auto node = new TaskNode(task);

            switch (mode_)
            {
                case QueueMode::FIFO:
                case QueueMode::ROUND_ROBIN:
                    fifo_queues_[group].enqueue(node);
                    break;

                case QueueMode::LIFO:
                    lifo_queues_[group].push(node);
                    break;

                case QueueMode::PRIORITY:
                    priority_queues_[group].enqueue(node, static_cast<int>(task->getPriority()));
                    break;
            }
        }
    }

    QueueMode QueueManager::getMode() const
    {
        std::shared_lock<std::shared_mutex> config_lock(config_mutex_);
        return mode_;
    }

    std::unordered_map<std::string, size_t> QueueManager::getQueueSizes() const
    {
        std::shared_lock<std::shared_mutex> queue_lock(queue_mutex_);

        std::unordered_map<std::string, size_t> sizes;

        switch (mode_)
        {
            case QueueMode::FIFO:
            case QueueMode::ROUND_ROBIN:
                for (const auto &[group, queue] : fifo_queues_)
                {
                    sizes[group] = queue.size();
                }
                break;

            case QueueMode::LIFO:
                for (const auto &[group, stack] : lifo_queues_)
                {
                    sizes[group] = stack.size();
                }
                break;

            case QueueMode::PRIORITY:
                for (const auto &[group, queue] : priority_queues_)
                {
                    sizes[group] = queue.size();
                }
                break;
        }

        return sizes;
    }

    std::unordered_map<std::string, double> QueueManager::getMetrics() const
    {
        std::unordered_map<std::string, double> metrics;

        metrics["total_tasks"]   = total_tasks_.load(std::memory_order_relaxed);
        metrics["enqueue_count"] = enqueue_count_.load(std::memory_order_relaxed);
        metrics["dequeue_count"] = dequeue_count_.load(std::memory_order_relaxed);
        metrics["peek_count"]    = peek_count_.load(std::memory_order_relaxed);
        metrics["cancel_count"]  = cancel_count_.load(std::memory_order_relaxed);

        uint64_t completed = dequeue_count_.load(std::memory_order_relaxed);
        if (completed > 0)
        {
            metrics["avg_wait_time_ms"] = static_cast<double>(
                                              total_wait_time_ms_.load(std::memory_order_relaxed)) /
                                          completed;
            metrics["avg_execution_time_ms"] =
                static_cast<double>(total_execution_time_ms_.load(std::memory_order_relaxed)) /
                completed;
        }
        else
        {
            metrics["avg_wait_time_ms"]      = 0.0;
            metrics["avg_execution_time_ms"] = 0.0;
        }

        return metrics;
    }

    std::string QueueManager::getNextRoundRobinGroup() const
    {
        if (group_names_.empty())
        {
            return "";
        }

        size_t index = current_group_index_.fetch_add(1, std::memory_order_relaxed) %
                       group_names_.size();
        return group_names_[index];
    }

    void QueueManager::updateTaskInMap(std::shared_ptr<Task> task)
    {
        if (!task)
            return;

        task_map_[task->getId()] = task;
    }

    void QueueManager::removeTaskFromMap(Task::TaskId id)
    {
        task_map_.erase(id);
    }

    std::shared_ptr<Task> QueueManager::getTaskFromFIFO(const std::string &group)
    {
        auto it = fifo_queues_.find(group);
        if (it == fifo_queues_.end() || it->second.empty())
        {
            return nullptr;
        }

        auto node = it->second.dequeue();
        if (!node)
        {
            return nullptr;
        }

        auto task = node->task;
        delete node;
        return task;
    }

    std::shared_ptr<Task> QueueManager::getTaskFromLIFO(const std::string &group)
    {
        auto it = lifo_queues_.find(group);
        if (it == lifo_queues_.end() || it->second.empty())
        {
            return nullptr;
        }

        auto node = it->second.pop();
        if (!node)
        {
            return nullptr;
        }

        auto task = node->task;
        delete node;
        return task;
    }

    std::shared_ptr<Task> QueueManager::getTaskFromPriority(const std::string &group)
    {
        auto it = priority_queues_.find(group);
        if (it == priority_queues_.end() || it->second.empty())
        {
            return nullptr;
        }

        auto node = it->second.dequeue();
        if (!node)
        {
            return nullptr;
        }

        return node->task;  // Node is deleted inside dequeue
    }

    std::shared_ptr<Task> QueueManager::getTaskRoundRobin()
    {
        size_t num_groups = group_names_.size();
        for (size_t i = 0; i < num_groups; ++i)
        {
            std::string group = getNextRoundRobinGroup();
            auto        task  = getTaskFromFIFO(group);
            if (task)
            {
                return task;
            }
        }

        return nullptr;
    }

}  // namespace taskqueuex