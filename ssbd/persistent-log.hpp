#pragma once

#ifndef PERSISTENT_LOG_HPP__
#define PERSISTENT_LOG_HPP__

#include "leveldb-serializer.hpp"

namespace ssbd
{

class persistent_log
{
    std::unique_ptr<leveldb::DB> db_log_ = nullptr;

public:
    persistent_log (std::string const &dbname)
    {
        leveldb::DB* db = nullptr;
        leveldb::Options options;
        options.create_if_missing = true;
        leveldb::Status status = leveldb::DB::Open(options, std::string(dbname), &db);
        if (not status.ok())
        {
            BOOST_LOG_TRIVIAL(error) << status.ToString() << "\n";
            throw std::runtime_error("cannot open db");
        }

        BOOST_LOG_TRIVIAL(debug) << "open db log ptr: " << db << "\n";
        db_log_.reset(db);
    }

    auto get_committed_version (std::string const& key) -> slsfs::leveldb_pack::versionint_t
    {
        std::string const commit_version_key = key + "-committed-version";
        std::string commit_version_buffer (sizeof(slsfs::leveldb_pack::versionint_t), '\0');
        db_log_->Get(leveldb::ReadOptions(), commit_version_key, &commit_version_buffer);

        slsfs::leveldb_pack::versionint_t version = 0;
        std::memcpy(&version, commit_version_buffer.data(), sizeof(slsfs::leveldb_pack::versionint_t));

        // get from network format
        return slsfs::leveldb_pack::ntoh(version);
    }

    void put_committed_version (std::string const& key, slsfs::leveldb_pack::versionint_t version)
    {
        std::string const commit_version_key = key + "-committed-version";
        std::string commit_version_buffer (sizeof(slsfs::leveldb_pack::versionint_t), '\0');

        // save as network format
        version = slsfs::leveldb_pack::hton(version);
        std::memcpy(commit_version_buffer.data(), &version, sizeof(slsfs::leveldb_pack::versionint_t));
        db_log_->Put(leveldb::WriteOptions(), commit_version_key, commit_version_buffer);
    }

    void put_pending_prepare (std::string const& key, std::string const& value, slsfs::leveldb_pack::versionint_t version)
    {
        { // save version
            std::string version_value (sizeof(slsfs::leveldb_pack::versionint_t), '\0');
            std::string const version_key = key + "-version";

            // save as network format
            version = slsfs::leveldb_pack::hton(version);
            std::memcpy(version_value.data(), &version, sizeof(slsfs::leveldb_pack::versionint_t));
            db_log_->Put(leveldb::WriteOptions(), version_key, version_value);
        }

        {
            std::string const log_key = key + "-data";
            db_log_->Put(leveldb::WriteOptions(), log_key, value);
        }
    }

    auto get_pending_prepare_data (std::string const& key) -> std::string
    {
        std::string const log_key = key + "-data";
        std::string result;
        db_log_->Get(leveldb::ReadOptions(), log_key, &result);
        return result;
    }

    auto get_pending_prepare_version (std::string const& key) -> slsfs::leveldb_pack::versionint_t
    {
        std::string version_value (sizeof(slsfs::leveldb_pack::versionint_t), '\0');
        std::string const version_key = key + "-version";

        db_log_->Get(leveldb::ReadOptions(), key, &version_value);

        // network format => host
        slsfs::leveldb_pack::versionint_t version = 0;
        std::memcpy(version_value.data(), &version, sizeof(slsfs::leveldb_pack::versionint_t));
        return slsfs::leveldb_pack::ntoh(version);
    }

    void commit_pending_prepare (std::string const& key, leveldb::DB& save_dest)
    {
        std::string const value = get_pending_prepare_data(key);
        slsfs::leveldb_pack::versionint_t const version = get_pending_prepare_version(key);

        put_committed_version(key, version);
        save_dest.Put(leveldb::WriteOptions(), key, value);
    }

    bool have_pending_log (std::string const& key)
    {
        return
            (get_pending_prepare_version(key) != get_committed_version(key)) ||
            (get_pending_prepare_version(key) != 0); // 0 == empty log
    }
};

} // namespace ssbd

#endif // PERSISTENT_LOG_HPP__
