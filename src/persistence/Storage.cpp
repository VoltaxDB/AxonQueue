#include "persistence/Storage.hpp"

#include <rocksdb/utilities/backup_engine.h>

#include <filesystem>

namespace taskqueuex
{

    Storage::Storage(const std::string &db_path) : db_path_(db_path)
    {
        write_options_       = std::make_unique<rocksdb::WriteOptions>();
        write_options_->sync = true;  // Ensure durability

        read_options_                   = std::make_unique<rocksdb::ReadOptions>();
        read_options_->verify_checksums = true;
    }

    Storage::~Storage()
    {
        shutdown();
    }

    bool Storage::initialize()
    {
        rocksdb::Options options;
        options.create_if_missing = true;
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();

        options.manual_wal_flush = true;

        rocksdb::Status status = rocksdb::DB::Open(options, db_path_, &db_);
        if (!status.ok())
        {
            return false;
        }

        std::string value;
        status = db_->Get(*read_options_, makeWALKey(0), &value);
        if (status.ok())
        {
            wal_sequence_ = std::stoull(value);
        }

        return true;
    }

    bool Storage::shutdown()
    {
        if (db_)
        {
            db_->SyncWAL();
            delete db_;
            db_ = nullptr;
        }
        return true;
    }

    bool Storage::saveTask(const std::shared_ptr<Task> &task)
    {
        if (!task)
            return false;

        std::unique_lock lock(mutex_);

        try
        {
            auto json   = task->toJson();
            auto status = db_->Put(*write_options_, makeTaskKey(task->getId()), json.dump());

            if (status.ok())
            {
                appendToWAL("save_task", json);
                return true;
            }
        }
        catch (const std::exception &)
        {
        }

        return false;
    }

    bool Storage::updateTaskStatus(Task::TaskId id, TaskStatus status)
    {
        std::unique_lock lock(mutex_);

        std::string value;
        auto        get_status = db_->Get(*read_options_, makeTaskKey(id), &value);

        if (!get_status.ok())
        {
            return false;
        }

        try
        {
            auto json      = nlohmann::json::parse(value);
            json["status"] = static_cast<int>(status);

            auto put_status = db_->Put(*write_options_, makeTaskKey(id), json.dump());

            if (put_status.ok())
            {
                appendToWAL("update_status",
                            {{"task_id", id}, {"status", static_cast<int>(status)}});
                return true;
            }
        }
        catch (const std::exception &)
        {
        }

        return false;
    }

    std::shared_ptr<Task> Storage::loadTask(Task::TaskId id)
    {
        std::shared_lock lock(mutex_);

        std::string value;
        auto        status = db_->Get(*read_options_, makeTaskKey(id), &value);

        if (!status.ok())
        {
            return nullptr;
        }

        try
        {
            auto json = nlohmann::json::parse(value);
            auto task = std::make_shared<Task>("", TaskPriority::MEDIUM);
            task->fromJson(json);
            return task;
        }
        catch (const std::exception &)
        {
        }

        return nullptr;
    }

    std::vector<std::shared_ptr<Task>> Storage::loadAllTasks()
    {
        std::vector<std::shared_ptr<Task>> tasks;
        std::shared_lock                   lock(mutex_);

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(*read_options_));

        for (it->Seek(TASK_PREFIX); it->Valid(); it->Next())
        {
            std::string key = it->key().ToString();
            if (key.find(TASK_PREFIX) != 0)
                break;

            try
            {
                auto json = nlohmann::json::parse(it->value().ToString());
                auto task = std::make_shared<Task>("", TaskPriority::MEDIUM);
                task->fromJson(json);
                tasks.push_back(task);
            }
            catch (const std::exception &)
            {
            }
        }

        return tasks;
    }

    bool Storage::appendToWAL(const std::string &operation, const nlohmann::json &data)
    {
        uint64_t sequence = ++wal_sequence_;

        nlohmann::json wal_entry = {
            {"sequence", sequence},
            {"timestamp", std::chrono::system_clock::now().time_since_epoch().count()},
            {"operation", operation},
            {"data", data}};

        auto status = db_->Put(*write_options_, makeWALKey(sequence), wal_entry.dump());

        if (status.ok())
        {
            db_->Put(*write_options_, makeWALKey(0), std::to_string(sequence));
            return true;
        }

        return false;
    }

    std::vector<std::pair<std::string, nlohmann::json>> Storage::replayWAL()
    {
        std::vector<std::pair<std::string, nlohmann::json>> operations;
        std::shared_lock                                    lock(mutex_);

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(*read_options_));

        for (it->Seek(WAL_PREFIX); it->Valid(); it->Next())
        {
            std::string key = it->key().ToString();
            if (key.find(WAL_PREFIX) != 0)
                break;

            try
            {
                auto wal_entry = nlohmann::json::parse(it->value().ToString());
                operations.emplace_back(wal_entry["operation"].get<std::string>(),
                                        wal_entry["data"]);
            }
            catch (const std::exception &)
            {
            }
        }

        return operations;
    }

    bool Storage::truncateWAL()
    {
        std::unique_lock lock(mutex_);

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(*read_options_));
        rocksdb::WriteBatch                batch;

        for (it->Seek(WAL_PREFIX); it->Valid(); it->Next())
        {
            std::string key = it->key().ToString();
            if (key.find(WAL_PREFIX) != 0)
                break;

            batch.Delete(key);
        }

        auto status = db_->Write(*write_options_, &batch);
        return status.ok();
    }

    bool Storage::compact()
    {
        std::unique_lock lock(mutex_);
        auto status = db_->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr);
        return status.ok();
    }

    bool Storage::backup(const std::string &backup_dir)
    {
        std::unique_lock lock(mutex_);

        rocksdb::BackupEngine *backup_engine;
        auto                   status = rocksdb::BackupEngine::Open(
            rocksdb::Env::Default(), rocksdb::BackupableDBOptions(backup_dir), &backup_engine);

        if (!status.ok())
        {
            return false;
        }

        status = backup_engine->CreateNewBackup(db_);
        delete backup_engine;

        return status.ok();
    }

    bool Storage::restore(const std::string &backup_dir)
    {
        std::unique_lock lock(mutex_);

        shutdown();

        rocksdb::BackupEngine *backup_engine;
        auto                   status = rocksdb::BackupEngine::Open(
            rocksdb::Env::Default(), rocksdb::BackupableDBOptions(backup_dir), &backup_engine);

        if (!status.ok())
        {
            initialize();  // Reopen the database
            return false;
        }

        status = backup_engine->RestoreDBFromLatestBackup(db_path_, db_path_);
        delete backup_engine;

        if (!status.ok())
        {
            initialize();  // Reopen the database
            return false;
        }

        return initialize();
    }

    std::string Storage::makeTaskKey(Task::TaskId id) const
    {
        return std::string(TASK_PREFIX) + std::to_string(id);
    }

    std::string Storage::makeWALKey(uint64_t sequence) const
    {
        return std::string(WAL_PREFIX) + std::to_string(sequence);
    }

    Task::TaskId Storage::extractTaskId(const std::string &key) const
    {
        if (key.find(TASK_PREFIX) == 0)
        {
            return std::stoull(key.substr(strlen(TASK_PREFIX)));
        }
        return 0;
    }

    size_t Storage::getStorageSize() const
    {
        std::shared_lock lock(mutex_);
        uint64_t         size = 0;

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(*read_options_));
        for (it->SeekToFirst(); it->Valid(); it->Next())
        {
            size += it->key().size() + it->value().size();
        }

        return size;
    }

    size_t Storage::getWALSize() const
    {
        std::shared_lock lock(mutex_);
        uint64_t         size = 0;

        std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(*read_options_));
        for (it->Seek(WAL_PREFIX); it->Valid(); it->Next())
        {
            std::string key = it->key().ToString();
            if (key.find(WAL_PREFIX) != 0)
                break;
            size += it->key().size() + it->value().size();
        }

        return size;
    }

    Storage::BatchOperation::BatchOperation(Storage &storage)
        : storage_(storage), batch_(std::make_unique<rocksdb::WriteBatch>())
    {
    }

    Storage::BatchOperation::~BatchOperation()
    {
        if (!committed_)
        {
            rollback();
        }
    }

    bool Storage::BatchOperation::saveTask(const std::shared_ptr<Task> &task)
    {
        if (!task)
            return false;

        try
        {
            auto json = task->toJson();
            batch_->Put(storage_.makeTaskKey(task->getId()), json.dump());
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    bool Storage::BatchOperation::updateTaskStatus(Task::TaskId id, TaskStatus status)
    {
        std::string value;
        auto        get_status = storage_.db_->Get(
            *storage_.read_options_, storage_.makeTaskKey(id), &value);

        if (!get_status.ok())
        {
            return false;
        }

        try
        {
            auto json      = nlohmann::json::parse(value);
            json["status"] = static_cast<int>(status);
            batch_->Put(storage_.makeTaskKey(id), json.dump());
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    bool Storage::BatchOperation::commit()
    {
        auto status = storage_.db_->Write(*storage_.write_options_, batch_.get());
        committed_  = status.ok();
        return committed_;
    }

    void Storage::BatchOperation::rollback()
    {
        batch_->Clear();
    }

}  // namespace taskqueuex