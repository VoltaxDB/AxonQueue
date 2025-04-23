#include "core/ThreadPool.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace taskqueuex
{

    ThreadPool::ThreadPool(size_t num_threads) : queue_manager_(std::make_shared<QueueManager>())
    {
        worker_health_.resize(num_threads);
        adjustThreadPool(num_threads);
    }

    ThreadPool::~ThreadPool()
    {
        stop();
    }

    void ThreadPool::start()
    {
        should_stop_ = false;
        paused_      = false;

        for (size_t i = 0; i < threads_.size(); ++i)
        {
            if (!threads_[i].joinable())
            {
                threads_[i] = std::thread(&ThreadPool::workerFunction, this, i);
            }
        }
    }

    void ThreadPool::stop()
    {
        should_stop_ = true;
        queue_manager_->resume();  // Wake up any waiting threads

        for (auto &thread : threads_)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }

        threads_.clear();
        active_threads_ = 0;
    }

    void ThreadPool::pause()
    {
        paused_ = true;
        queue_manager_->pause();
    }

    void ThreadPool::resume()
    {
        paused_ = false;
        queue_manager_->resume();
    }

    void ThreadPool::setNumThreads(size_t num_threads)
    {
        if (num_threads == threads_.size())
        {
            return;
        }

        if (num_threads < threads_.size())
        {
            stop();
        }

        adjustThreadPool(num_threads);

        if (!should_stop_)
        {
            start();
        }
    }

    void ThreadPool::adjustThreadPool(size_t target_size)
    {
        worker_health_.resize(target_size);

        if (target_size > threads_.size())
        {
            size_t old_size = threads_.size();
            threads_.resize(target_size);

            for (size_t i = old_size; i < target_size; ++i)
            {
                if (!should_stop_)
                {
                    threads_[i] = std::thread(&ThreadPool::workerFunction, this, i);
                }
            }
        }
    }

    void ThreadPool::workerFunction(size_t worker_id)
    {
        auto &health = worker_health_[worker_id];

        while (!should_stop_)
        {
            health.last_heartbeat.store(std::chrono::system_clock::now());
            health.is_busy = false;

            QueueManager::TaskPtr task;
            if (queue_manager_->tryDequeue(task))
            {
                health.is_busy = true;
                active_threads_++;

                auto start_time = std::chrono::high_resolution_clock::now();

                try
                {
                    task->markStarted();
                    task->execute();
                    task->markCompleted();
                }
                catch (const std::exception &e)
                {
                    task->markFailed(e.what());
                }

                auto end_time = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time -
                                                                                     start_time);

                updateMetrics(duration);
                health.tasks_processed++;
                active_threads_--;
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    void ThreadPool::updateMetrics(std::chrono::nanoseconds task_duration)
    {
        tasks_processed_++;
        total_task_latency_ += task_duration.count();
    }

    double ThreadPool::getAverageTaskLatency() const
    {
        if (tasks_processed_ == 0)
        {
            return 0.0;
        }
        return total_task_latency_ / static_cast<double>(tasks_processed_);
    }

    size_t ThreadPool::getTasksQueued() const
    {
        return queue_manager_->getQueuedCount();
    }

    bool ThreadPool::isHealthy() const
    {
        auto now = std::chrono::system_clock::now();

        for (const auto &health : worker_health_)
        {
            auto last_heartbeat = health.last_heartbeat.load();
            auto duration =
                std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count();

            if (duration > 5)
            {
                return false;
            }
        }

        return true;
    }

    std::vector<std::string> ThreadPool::getWorkerStatus() const
    {
        std::vector<std::string> status;
        auto                     now = std::chrono::system_clock::now();

        for (size_t i = 0; i < worker_health_.size(); ++i)
        {
            const auto &health         = worker_health_[i];
            auto        last_heartbeat = health.last_heartbeat.load();
            auto        duration =
                std::chrono::duration_cast<std::chrono::seconds>(now - last_heartbeat).count();

            std::stringstream ss;
            ss << "Worker " << i << ": " << (health.is_busy.load() ? "Busy" : "Idle")
               << ", Tasks: " << health.tasks_processed.load() << ", Last heartbeat: " << duration
               << "s ago";

            status.push_back(ss.str());
        }

        return status;
    }

}  // namespace taskqueuex