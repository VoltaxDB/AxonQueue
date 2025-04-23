#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace taskqueuex
{

    enum class TaskStatus
    {
        CREATED,
        QUEUED,
        RUNNING,
        COMPLETED,
        FAILED,
        CANCELLED
    };

    enum class TaskPriority
    {
        LOW      = 0,
        MEDIUM   = 50,
        HIGH     = 100,
        CRITICAL = 200
    };

    class Task
    {
       public:
        using TimePoint = std::chrono::system_clock::time_point;
        using TaskId    = uint64_t;

        Task(std::string name, TaskPriority priority = TaskPriority::MEDIUM);
        virtual ~Task() = default;

        TaskId getId() const
        {
            return id_;
        }
        const std::string &getName() const
        {
            return name_;
        }
        TaskPriority getPriority() const
        {
            return priority_;
        }
        TaskStatus getStatus() const
        {
            return status_;
        }

        TimePoint getCreatedAt() const
        {
            return created_at_;
        }
        TimePoint getScheduledFor() const
        {
            return scheduled_for_;
        }
        TimePoint getStartedAt() const
        {
            return started_at_;
        }
        TimePoint getCompletedAt() const
        {
            return completed_at_;
        }

        virtual void execute() = 0;
        void         markQueued();
        void         markStarted();
        void         markCompleted();
        void         markFailed(const std::string &error);
        void         cancel();

        virtual nlohmann::json toJson() const;
        virtual void           fromJson(const nlohmann::json &j);

        void addDependency(std::shared_ptr<Task> task);
        bool areDependenciesMet() const;

        void setMaxRetries(uint32_t retries)
        {
            max_retries_ = retries;
        }
        uint32_t getRetryCount() const
        {
            return retry_count_;
        }
        bool canRetry() const
        {
            return retry_count_ < max_retries_;
        }

       protected:
        TaskId                  id_;
        std::string             name_;
        TaskPriority            priority_;
        std::atomic<TaskStatus> status_;

        TimePoint created_at_;
        TimePoint scheduled_for_;
        TimePoint started_at_;
        TimePoint completed_at_;

        std::string error_message_;
        uint32_t    retry_count_{0};
        uint32_t    max_retries_{3};

        std::vector<std::weak_ptr<Task>> dependencies_;

       private:
        static std::atomic<TaskId> next_id_;
        TaskId                     generateId();
    };

}  // namespace taskqueuex