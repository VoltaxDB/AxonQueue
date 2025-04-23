#include "core/QueueManager.hpp"

#include <algorithm>
#include <stdexcept>

namespace taskqueuex
{

    QueueManager::QueueManager(QueueMode initial_mode) : mode_(initial_mode)
    {
        group_queues_["default"] = TaskQueue();
        group_order_.push_back("default");
    }

    void QueueManager::enqueue(TaskPtr task, const std::string &group)
    {
        std::unique_lock lock(mutex_);

        if (task == nullptr)
        {
            throw std::invalid_argument("Cannot enqueue null task");
        }

        if (group_queues_.find(group) == group_queues_.end())
        {
            group_queues_[group] = TaskQueue();
            group_order_.push_back(group);
        }

        task->markQueued();

        if (mode_ == QueueMode::PRIORITY)
        {
            size_t priority_index = static_cast<size_t>(task->getPriority()) / 100;
            priority_index        = std::min(priority_index, priority_queues_.size() - 1);
            priority_queues_[priority_index].push_back(task);
        }
        else
        {
            group_queues_[group].push_back(task);
        }

        indexTask(task);
        queued_count_++;

        cv_.notify_one();
    }

    QueueManager::TaskPtr QueueManager::dequeue()
    {
        std::unique_lock lock(mutex_);

        while (empty() && !paused_)
        {
            cv_.wait(lock);
        }

        if (paused_)
        {
            return nullptr;
        }

        return dequeueInternal();
    }

    bool QueueManager::tryDequeue(TaskPtr &task)
    {
        std::unique_lock lock(mutex_);

        if (empty() || paused_)
        {
            return false;
        }

        task = dequeueInternal();
        return task != nullptr;
    }

    QueueManager::TaskPtr QueueManager::dequeueInternal()
    {
        switch (mode_)
        {
            case QueueMode::FIFO:
                return dequeueFIFO();
            case QueueMode::LIFO:
                return dequeueLIFO();
            case QueueMode::ROUND_ROBIN:
                return dequeueRoundRobin();
            case QueueMode::PRIORITY:
                return dequeuePriority();
            default:
                return nullptr;
        }
    }

    QueueManager::TaskPtr QueueManager::dequeueFIFO()
    {
        for (const auto &group : group_order_)
        {
            auto &queue = group_queues_[group];
            if (!queue.empty())
            {
                TaskPtr task = queue.front();
                queue.pop_front();
                queued_count_--;
                running_count_++;

                auto wait_time = std::chrono::system_clock::now() - task->getScheduledFor();
                total_wait_time_ +=
                    std::chrono::duration_cast<std::chrono::milliseconds>(wait_time).count();

                return task;
            }
        }
        return nullptr;
    }

    QueueManager::TaskPtr QueueManager::dequeueLIFO()
    {
        for (const auto &group : group_order_)
        {
            auto &queue = group_queues_[group];
            if (!queue.empty())
            {
                TaskPtr task = queue.back();
                queue.pop_back();
                queued_count_--;
                running_count_++;

                auto wait_time = std::chrono::system_clock::now() - task->getScheduledFor();
                total_wait_time_ +=
                    std::chrono::duration_cast<std::chrono::milliseconds>(wait_time).count();

                return task;
            }
        }
        return nullptr;
    }

    QueueManager::TaskPtr QueueManager::dequeueRoundRobin()
    {
        size_t attempts = 0;
        while (attempts < group_order_.size())
        {
            const auto &group = group_order_[current_group_index_];
            auto       &queue = group_queues_[group];

            current_group_index_ = (current_group_index_ + 1) % group_order_.size();

            if (!queue.empty())
            {
                TaskPtr task = queue.front();
                queue.pop_front();
                queued_count_--;
                running_count_++;

                auto wait_time = std::chrono::system_clock::now() - task->getScheduledFor();
                total_wait_time_ +=
                    std::chrono::duration_cast<std::chrono::milliseconds>(wait_time).count();

                return task;
            }
            attempts++;
        }
        return nullptr;
    }

    QueueManager::TaskPtr QueueManager::dequeuePriority()
    {
        for (auto it = priority_queues_.rbegin(); it != priority_queues_.rend(); ++it)
        {
            if (!it->empty())
            {
                TaskPtr task = it->front();
                it->pop_front();
                queued_count_--;
                running_count_++;

                auto wait_time = std::chrono::system_clock::now() - task->getScheduledFor();
                total_wait_time_ +=
                    std::chrono::duration_cast<std::chrono::milliseconds>(wait_time).count();

                return task;
            }
        }
        return nullptr;
    }

    void QueueManager::setMode(QueueMode mode)
    {
        std::unique_lock lock(mutex_);
        if (mode_ == mode)
        {
            return;
        }

        std::vector<TaskPtr> tasks;
        for (auto &[group, queue] : group_queues_)
        {
            while (!queue.empty())
            {
                tasks.push_back(queue.front());
                queue.pop_front();
            }
        }

        mode_ = mode;

        for (const auto &task : tasks)
        {
            if (mode_ == QueueMode::PRIORITY)
            {
                size_t priority_index = static_cast<size_t>(task->getPriority()) / 100;
                priority_index        = std::min(priority_index, priority_queues_.size() - 1);
                priority_queues_[priority_index].push_back(task);
            }
            else
            {
                group_queues_["default"].push_back(task);
            }
        }
    }

    void QueueManager::pause()
    {
        std::unique_lock lock(mutex_);
        paused_ = true;
    }

    void QueueManager::resume()
    {
        std::unique_lock lock(mutex_);
        paused_ = false;
        cv_.notify_all();
    }

    void QueueManager::clear()
    {
        std::unique_lock lock(mutex_);
        for (auto &[group, queue] : group_queues_)
        {
            queue.clear();
        }
        for (auto &queue : priority_queues_)
        {
            queue.clear();
        }
        queued_count_ = 0;
    }

    bool QueueManager::empty() const
    {
        if (mode_ == QueueMode::PRIORITY)
        {
            return std::all_of(priority_queues_.begin(),
                               priority_queues_.end(),
                               [](const TaskQueue &q) { return q.empty(); });
        }
        return std::all_of(group_queues_.begin(),
                           group_queues_.end(),
                           [](const auto &pair) { return pair.second.empty(); });
    }

    size_t QueueManager::size() const
    {
        return queued_count_;
    }

    bool QueueManager::cancelTask(Task::TaskId id)
    {
        std::unique_lock lock(mutex_);
        auto             it = task_index_.find(id);
        if (it != task_index_.end())
        {
            if (auto task = it->second.lock())
            {
                task->cancel();
                removeFromIndex(id);
                return true;
            }
        }
        return false;
    }

    QueueManager::TaskPtr QueueManager::findTask(Task::TaskId id)
    {
        std::shared_lock lock(mutex_);
        auto             it = task_index_.find(id);
        if (it != task_index_.end())
        {
            return it->second.lock();
        }
        return nullptr;
    }

    void QueueManager::indexTask(TaskPtr task)
    {
        task_index_[task->getId()] = task;
    }

    void QueueManager::removeFromIndex(Task::TaskId id)
    {
        task_index_.erase(id);
    }

    size_t QueueManager::getQueuedCount() const
    {
        return queued_count_;
    }

    size_t QueueManager::getRunningCount() const
    {
        return running_count_;
    }

    double QueueManager::getAverageWaitTime() const
    {
        if (completed_count_ == 0)
        {
            return 0.0;
        }
        return total_wait_time_ / static_cast<double>(completed_count_);
    }

}  // namespace taskqueuex