#include "core/Task.hpp"

#include <chrono>

namespace taskqueuex
{

    std::atomic<Task::TaskId> Task::next_id_{0};

    Task::Task(std::string name, TaskPriority priority)
        : id_(generateId()), name_(std::move(name)), priority_(priority),
          status_(TaskStatus::CREATED), created_at_(std::chrono::system_clock::now()),
          scheduled_for_(created_at_)
    {
    }

    Task::TaskId Task::generateId()
    {
        return next_id_++;
    }

    void Task::markQueued()
    {
        TaskStatus expected = TaskStatus::CREATED;
        if (status_.compare_exchange_strong(expected, TaskStatus::QUEUED))
        {
            scheduled_for_ = std::chrono::system_clock::now();
        }
    }

    void Task::markStarted()
    {
        TaskStatus expected = TaskStatus::QUEUED;
        if (status_.compare_exchange_strong(expected, TaskStatus::RUNNING))
        {
            started_at_ = std::chrono::system_clock::now();
        }
    }

    void Task::markCompleted()
    {
        TaskStatus expected = TaskStatus::RUNNING;
        if (status_.compare_exchange_strong(expected, TaskStatus::COMPLETED))
        {
            completed_at_ = std::chrono::system_clock::now();
        }
    }

    void Task::markFailed(const std::string &error)
    {
        error_message_      = error;
        TaskStatus expected = TaskStatus::RUNNING;
        if (status_.compare_exchange_strong(expected, TaskStatus::FAILED))
        {
            completed_at_ = std::chrono::system_clock::now();
            if (canRetry())
            {
                retry_count_++;
                status_.store(TaskStatus::CREATED);
            }
        }
    }

    void Task::cancel()
    {
        TaskStatus current = status_.load();
        if (current != TaskStatus::COMPLETED && current != TaskStatus::FAILED &&
            current != TaskStatus::CANCELLED)
        {
            status_.store(TaskStatus::CANCELLED);
            completed_at_ = std::chrono::system_clock::now();
        }
    }

    void Task::addDependency(std::shared_ptr<Task> task)
    {
        dependencies_.push_back(task);
    }

    bool Task::areDependenciesMet() const
    {
        for (const auto &dep_weak : dependencies_)
        {
            if (auto dep = dep_weak.lock())
            {
                if (dep->getStatus() != TaskStatus::COMPLETED)
                {
                    return false;
                }
            }
        }
        return true;
    }

    nlohmann::json Task::toJson() const
    {
        nlohmann::json j;
        j["id"]            = id_;
        j["name"]          = name_;
        j["priority"]      = static_cast<int>(priority_);
        j["status"]        = static_cast<int>(status_.load());
        j["created_at"]    = std::chrono::system_clock::to_time_t(created_at_);
        j["scheduled_for"] = std::chrono::system_clock::to_time_t(scheduled_for_);
        j["started_at"]    = std::chrono::system_clock::to_time_t(started_at_);
        j["completed_at"]  = std::chrono::system_clock::to_time_t(completed_at_);
        j["error_message"] = error_message_;
        j["retry_count"]   = retry_count_;
        j["max_retries"]   = max_retries_;

        std::vector<Task::TaskId> dep_ids;
        for (const auto &dep_weak : dependencies_)
        {
            if (auto dep = dep_weak.lock())
            {
                dep_ids.push_back(dep->getId());
            }
        }
        j["dependencies"] = dep_ids;

        return j;
    }

    void Task::fromJson(const nlohmann::json &j)
    {
        id_       = j["id"].get<TaskId>();
        name_     = j["name"].get<std::string>();
        priority_ = static_cast<TaskPriority>(j["priority"].get<int>());
        status_.store(static_cast<TaskStatus>(j["status"].get<int>()));

        created_at_    = std::chrono::system_clock::from_time_t(j["created_at"].get<time_t>());
        scheduled_for_ = std::chrono::system_clock::from_time_t(j["scheduled_for"].get<time_t>());
        started_at_    = std::chrono::system_clock::from_time_t(j["started_at"].get<time_t>());
        completed_at_  = std::chrono::system_clock::from_time_t(j["completed_at"].get<time_t>());

        error_message_ = j["error_message"].get<std::string>();
        retry_count_   = j["retry_count"].get<uint32_t>();
        max_retries_   = j["max_retries"].get<uint32_t>();
    }

}  // namespace taskqueuex