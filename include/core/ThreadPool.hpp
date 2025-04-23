#pragma once

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <queue>
#include <thread>
#include <vector>

#include "QueueManager.hpp"

namespace taskqueuex
{

    class ThreadPool
    {
       public:
        explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency());
        ~ThreadPool();

        void start();
        void stop();
        void pause();
        void resume();

        void   setNumThreads(size_t num_threads);
        size_t getNumThreads() const
        {
            return threads_.size();
        }
        size_t getActiveThreads() const
        {
            return active_threads_;
        }

        template <typename F, typename... Args>
        auto submit(F &&f, Args &&...args)
            -> std::future<typename std::invoke_result_t<F, Args...>>;

        double getAverageTaskLatency() const;
        size_t getTasksProcessed() const
        {
            return tasks_processed_;
        }
        size_t getTasksQueued() const;

        bool                     isHealthy() const;
        std::vector<std::string> getWorkerStatus() const;

       private:
        std::vector<std::thread>      threads_;
        std::shared_ptr<QueueManager> queue_manager_;

        std::atomic<bool>   should_stop_{false};
        std::atomic<bool>   paused_{false};
        std::atomic<size_t> active_threads_{0};

        void workerFunction(size_t worker_id);

        std::atomic<size_t> tasks_processed_{0};
        std::atomic<double> total_task_latency_{0};

        struct WorkerHealth
        {
            std::atomic<std::chrono::system_clock::time_point> last_heartbeat;
            std::atomic<size_t>                                tasks_processed{0};
            std::atomic<bool>                                  is_busy{false};
        };
        std::vector<WorkerHealth> worker_health_;

        void adjustThreadPool(size_t target_size);
        void monitorWorkerHealth();
        void updateMetrics(std::chrono::nanoseconds task_duration);
    };

    template <typename F, typename... Args>
    auto ThreadPool::submit(F &&f, Args &&...args)
        -> std::future<typename std::invoke_result_t<F, Args...>>
    {
        using return_type = typename std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> result = task->get_future();

        auto wrapped_task = std::make_shared<Task>("async_task", TaskPriority::MEDIUM);

        wrapped_task->execute = [task]() { (*task)(); };

        queue_manager_->enqueue(wrapped_task);

        return result;
    }

}  // namespace taskqueuex