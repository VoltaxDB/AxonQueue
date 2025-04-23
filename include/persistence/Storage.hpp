#pragma once

#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "../core/Task.hpp"

namespace taskqueuex
{

    class Storage
    {
       public:
        explicit Storage(const std::string &db_path);
        ~Storage();

        bool initialize();
        bool shutdown();

        bool                               saveTask(const std::shared_ptr<Task> &task);
        bool                               updateTaskStatus(Task::TaskId id, TaskStatus status);
        std::shared_ptr<Task>              loadTask(Task::TaskId id);
        std::vector<std::shared_ptr<Task>> loadAllTasks();

        bool appendToWAL(const std::string &operation, const nlohmann::json &data);
        std::vector<std::pair<std::string, nlohmann::json>> replayWAL();
        bool                                                truncateWAL();

        class BatchOperation
        {
           public:
            explicit BatchOperation(Storage &storage);
            ~BatchOperation();

            bool saveTask(const std::shared_ptr<Task> &task);
            bool updateTaskStatus(Task::TaskId id, TaskStatus status);
            bool commit();
            void rollback();

           private:
            Storage                             &storage_;
            std::unique_ptr<rocksdb::WriteBatch> batch_;
            bool                                 committed_{false};
        };

        bool compact();
        bool backup(const std::string &backup_dir);
        bool restore(const std::string &backup_dir);

        size_t getStorageSize() const;
        size_t getWALSize() const;

       private:
        std::string                            db_path_;
        std::unique_ptr<rocksdb::DB>           db_;
        std::unique_ptr<rocksdb::WriteOptions> write_options_;
        std::unique_ptr<rocksdb::ReadOptions>  read_options_;

        static constexpr const char *TASK_PREFIX = "task:";
        static constexpr const char *WAL_PREFIX  = "wal:";
        static constexpr const char *META_PREFIX = "meta:";

        std::string  makeTaskKey(Task::TaskId id) const;
        std::string  makeWALKey(uint64_t sequence) const;
        Task::TaskId extractTaskId(const std::string &key) const;

        std::atomic<uint64_t>     wal_sequence_{0};
        mutable std::shared_mutex mutex_;

        friend class BatchOperation;
    };

}  // namespace taskqueuex