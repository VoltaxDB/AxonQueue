#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../core/Task.hpp"

namespace taskqueuex
{
    namespace utils
    {

        template <typename T>
        class LockFreeQueue;
        template <typename T>
        class IntrusiveList;
        template <typename T>
        class TaskGraph;

        template <typename T>
        class IntrusiveListNode
        {
            friend class IntrusiveList<T>;

           public:
            IntrusiveListNode()          = default;
            virtual ~IntrusiveListNode() = default;

            IntrusiveListNode(const IntrusiveListNode &)            = delete;
            IntrusiveListNode &operator=(const IntrusiveListNode &) = delete;

           private:
            std::atomic<T *> next_{nullptr};
        };

        template <typename T>
        class IntrusiveList
        {
           public:
            IntrusiveList() : head_(nullptr), tail_(nullptr), size_(0) {}
            ~IntrusiveList() = default;

            IntrusiveList(const IntrusiveList &)            = delete;
            IntrusiveList &operator=(const IntrusiveList &) = delete;

            void pushFront(T *node)
            {
                node->next_ = head_.load(std::memory_order_relaxed);
                while (!head_.compare_exchange_weak(
                    node->next_, node, std::memory_order_release, std::memory_order_relaxed))
                {
                }

                if (node->next_ == nullptr)
                {
                    tail_.store(node, std::memory_order_release);
                }

                size_.fetch_add(1, std::memory_order_relaxed);
            }

            void pushBack(T *node)
            {
                node->next_ = nullptr;

                T *prev_tail = tail_.exchange(node, std::memory_order_acq_rel);
                if (prev_tail)
                {
                    prev_tail->next_.store(node, std::memory_order_release);
                }
                else
                {
                    head_.store(node, std::memory_order_release);
                }

                size_.fetch_add(1, std::memory_order_relaxed);
            }

            T *popFront()
            {
                T *node = head_.load(std::memory_order_relaxed);
                if (!node)
                    return nullptr;

                T *next = node->next_.load(std::memory_order_relaxed);
                while (!head_.compare_exchange_weak(
                    node, next, std::memory_order_release, std::memory_order_relaxed))
                {
                    if (!node)
                        return nullptr;  // List became empty
                    next = node->next_.load(std::memory_order_relaxed);
                }

                if (next == nullptr)
                {
                    tail_.store(nullptr, std::memory_order_release);
                }

                size_.fetch_sub(1, std::memory_order_relaxed);
                return node;
            }

            bool empty() const
            {
                return head_.load(std::memory_order_relaxed) == nullptr;
            }

            size_t size() const
            {
                return size_.load(std::memory_order_relaxed);
            }

           private:
            std::atomic<T *>    head_;
            std::atomic<T *>    tail_;
            std::atomic<size_t> size_;
        };

        template <typename T>
        class LockFreeQueue
        {
           public:
            LockFreeQueue()  = default;
            ~LockFreeQueue() = default;

            LockFreeQueue(const LockFreeQueue &)            = delete;
            LockFreeQueue &operator=(const LockFreeQueue &) = delete;

            void enqueue(T *item)
            {
                list_.pushBack(item);
            }

            T *dequeue()
            {
                return list_.popFront();
            }

            T *peek() const
            {
                return list_.head_.load(std::memory_order_acquire);
            }

            bool empty() const
            {
                return list_.empty();
            }

            size_t size() const
            {
                return list_.size();
            }

           private:
            IntrusiveList<T> list_;
        };

        template <typename T>
        class LockFreeStack
        {
           public:
            LockFreeStack() : head_(nullptr), size_(0) {}
            ~LockFreeStack() = default;

            LockFreeStack(const LockFreeStack &)            = delete;
            LockFreeStack &operator=(const LockFreeStack &) = delete;

            void push(T *item)
            {
                item->next_ = head_.load(std::memory_order_relaxed);
                while (!head_.compare_exchange_weak(
                    item->next_, item, std::memory_order_release, std::memory_order_relaxed))
                {
                }

                size_.fetch_add(1, std::memory_order_relaxed);
            }

            T *pop()
            {
                T *item = head_.load(std::memory_order_relaxed);
                if (!item)
                    return nullptr;

                while (!head_.compare_exchange_weak(item,
                                                    item->next_.load(std::memory_order_relaxed),
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed))
                {
                    if (!item)
                        return nullptr;
                }

                size_.fetch_sub(1, std::memory_order_relaxed);
                return item;
            }

            T *peek() const
            {
                return head_.load(std::memory_order_acquire);
            }

            bool empty() const
            {
                return head_.load(std::memory_order_relaxed) == nullptr;
            }

            size_t size() const
            {
                return size_.load(std::memory_order_relaxed);
            }

           private:
            std::atomic<T *>    head_;
            std::atomic<size_t> size_;
        };

        template <typename T>
        class LockFreePriorityQueue
        {
           private:
            static constexpr int MAX_LEVEL = 16;

            struct Node
            {
                T                  *value;
                int                 priority;
                std::atomic<Node *> next[MAX_LEVEL];
                int                 level;

                Node(T *val, int prio, int lvl) : value(val), priority(prio), level(lvl)
                {
                    for (int i = 0; i < level; i++)
                    {
                        next[i].store(nullptr, std::memory_order_relaxed);
                    }
                }
            };

           public:
            LockFreePriorityQueue() : size_(0)
            {
                head_ = new Node(nullptr, std::numeric_limits<int>::min(), MAX_LEVEL);
            }

            ~LockFreePriorityQueue()
            {
                clear();
                delete head_;
            }

            LockFreePriorityQueue(const LockFreePriorityQueue &)            = delete;
            LockFreePriorityQueue &operator=(const LockFreePriorityQueue &) = delete;

            void enqueue(T *item, int priority)
            {
                const int randomLevel = getRandomLevel();
                Node     *newNode     = new Node(item, priority, randomLevel);

                Node *update[MAX_LEVEL];
                Node *current = head_;

                Node *next = current->next[i].load(std::memory_order_acquire);
                while (next && next->priority > priority)
                {
                    current = next;
                    next    = current->next[i].load(std::memory_order_acquire);
                }
                update[i] = current;
            }

            for (int i = 0; i < randomLevel; i++)
            {
                Node *expectedNext = update[i]->next[i].load(std::memory_order_relaxed);
                newNode->next[i].store(expectedNext, std::memory_order_relaxed);
                while (!update[i]->next[i].compare_exchange_weak(
                    expectedNext, newNode, std::memory_order_release, std::memory_order_relaxed))
                {
                    expectedNext = update[i]->next[i].load(std::memory_order_relaxed);
                    newNode->next[i].store(expectedNext, std::memory_order_relaxed);
                }
            }

            size_.fetch_add(1, std::memory_order_relaxed);
        }

        T *
        dequeue()
        {
            Node *current = head_;
            Node *next    = current->next[0].load(std::memory_order_acquire);

            if (!next)
                return nullptr;  // Empty queue

            for (int i = next->level - 1; i >= 0; i--)
            {
                Node *expectedNext = next;
                while (!current->next[i].compare_exchange_weak(
                    expectedNext,
                    next->next[i].load(std::memory_order_relaxed),
                    std::memory_order_release,
                    std::memory_order_relaxed))
                {
                    next = current->next[0].load(std::memory_order_acquire);
                    if (!next)
                        return nullptr;  // Queue became empty
                    expectedNext = next;
                }
            }

            T *result = next->value;
            delete next;

            size_.fetch_sub(1, std::memory_order_relaxed);
            return result;
        }

        T *peek() const
        {
            Node *next = head_->next[0].load(std::memory_order_acquire);
            return next ? next->value : nullptr;
        }

        bool empty() const
        {
            return head_->next[0].load(std::memory_order_relaxed) == nullptr;
        }

        size_t size() const
        {
            return size_.load(std::memory_order_relaxed);
        }

        void clear()
        {
            Node *current = head_->next[0].load(std::memory_order_acquire);
            while (current)
            {
                Node *next = current->next[0].load(std::memory_order_acquire);
                delete current;
                current = next;
            }

            for (int i = 0; i < MAX_LEVEL; i++)
            {
                head_->next[i].store(nullptr, std::memory_order_relaxed);
            }

            size_.store(0, std::memory_order_relaxed);
        }

       private:
        Node               *head_;
        std::atomic<size_t> size_;

        int getRandomLevel() const
        {
            static thread_local std::mt19937 gen(std::random_device{}());
            std::uniform_int_distribution<>  dis(1, MAX_LEVEL);
            return dis(gen);
        }
    };  // namespace utils

    class TaskGraph
    {
       public:
        TaskGraph()  = default;
        ~TaskGraph() = default;

        void addDependency(Task::TaskId dependent, Task::TaskId dependency)
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);

            if (dependencies_.find(dependent) == dependencies_.end())
            {
                dependencies_[dependent] = std::unordered_set<Task::TaskId>();
            }

            dependencies_[dependent].insert(dependency);

            if (dependents_.find(dependency) == dependents_.end())
            {
                dependents_[dependency] = std::unordered_set<Task::TaskId>();
            }

            dependents_[dependency].insert(dependent);
        }

        void removeDependency(Task::TaskId dependent, Task::TaskId dependency)
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);

            auto it = dependencies_.find(dependent);
            if (it != dependencies_.end())
            {
                it->second.erase(dependency);
            }

            auto rev_it = dependents_.find(dependency);
            if (rev_it != dependents_.end())
            {
                rev_it->second.erase(dependent);
            }
        }

        std::unordered_set<Task::TaskId> getDependencies(Task::TaskId taskId) const
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);

            auto it = dependencies_.find(taskId);
            if (it != dependencies_.end())
            {
                return it->second;
            }

            return std::unordered_set<Task::TaskId>();
        }

        std::unordered_set<Task::TaskId> getDependents(Task::TaskId taskId) const
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);

            auto it = dependents_.find(taskId);
            if (it != dependents_.end())
            {
                return it->second;
            }

            return std::unordered_set<Task::TaskId>();
        }

        bool hasDependencies(Task::TaskId taskId) const
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);

            auto it = dependencies_.find(taskId);
            return it != dependencies_.end() && !it->second.empty();
        }

        bool hasDependents(Task::TaskId taskId) const
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);

            auto it = dependents_.find(taskId);
            return it != dependents_.end() && !it->second.empty();
        }

        std::vector<Task::TaskId> getReadyTasks() const
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);

            std::vector<Task::TaskId> readyTasks;
            for (const auto &[taskId, deps] : dependencies_)
            {
                if (deps.empty())
                {
                    readyTasks.push_back(taskId);
                }
            }

            return readyTasks;
        }

        void removeTask(Task::TaskId taskId)
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);

            dependencies_.erase(taskId);

            for (auto &[_, deps] : dependencies_)
            {
                deps.erase(taskId);
            }

            auto dep_it = dependents_.find(taskId);
            if (dep_it != dependents_.end())
            {
                for (Task::TaskId dependent : dep_it->second)
                {
                    auto it = dependencies_.find(dependent);
                    if (it != dependencies_.end())
                    {
                        it->second.erase(taskId);
                    }
                }

                dependents_.erase(dep_it);
            }
        }

        std::unordered_map<Task::TaskId, std::unordered_set<Task::TaskId>> getSubgraph(
            Task::TaskId rootId) const
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);

            std::unordered_map<Task::TaskId, std::unordered_set<Task::TaskId>> subgraph;
            std::unordered_set<Task::TaskId>                                   visited;

            std::function<void(Task::TaskId)> buildSubgraph = [&](Task::TaskId id)
            {
                if (visited.find(id) != visited.end())
                    return;
                visited.insert(id);

                auto it = dependencies_.find(id);
                if (it != dependencies_.end())
                {
                    subgraph[id] = it->second;
                    for (Task::TaskId depId : it->second)
                    {
                        buildSubgraph(depId);
                    }
                }
                else
                {
                    subgraph[id] = {};
                }
            };

            buildSubgraph(rootId);
            return subgraph;
        }

        std::string toDOT(const std::unordered_map<Task::TaskId, std::string> &taskNames = {}) const
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);

            std::stringstream dot;
            dot << "digraph TaskDependencies {\n";

            for (const auto &[taskId, _] : dependencies_)
            {
                std::string name = "Task " + std::to_string(taskId);
                auto        it   = taskNames.find(taskId);
                if (it != taskNames.end())
                {
                    name = it->second;
                }

                dot << "  task_" << taskId << " [label=\"" << name << "\"];\n";
            }

            for (const auto &[taskId, deps] : dependencies_)
            {
                for (Task::TaskId depId : deps)
                {
                    dot << "  task_" << taskId << " -> task_" << depId << ";\n";
                }
            }

            dot << "}\n";
            return dot.str();
        }

       private:
        std::unordered_map<Task::TaskId, std::unordered_set<Task::TaskId>> dependencies_;

        std::unordered_map<Task::TaskId, std::unordered_set<Task::TaskId>> dependents_;

        mutable std::shared_mutex mutex_;
    };

}  // namespace taskqueuex
   // namespace taskqueuex