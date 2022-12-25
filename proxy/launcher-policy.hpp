#pragma once
#ifndef LAUNCHER_POLICY_HPP__
#define LAUNCHER_POLICY_HPP__

#include "basic.hpp"
#include "serializer.hpp"
#include "worker.hpp"
#include "uuid.hpp"
#include "worker-config.hpp"
#include "policy/all.hpp"

#include <oneapi/tbb/concurrent_unordered_set.h>
#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/concurrent_hash_map.h>

#include <type_traits>
#include <concepts>

namespace slsfs::launcher
{

class launcher_policy
{
    worker_set& worker_set_;
    fileid_map& fileid_to_worker_;
    oneapi::tbb::concurrent_queue<job_ptr>& pending_jobs_;
    std::string const announce_host_;
    boost::asio::ip::port_type const announce_port_;

public:
    std::string worker_config_;
    std::unique_ptr<policy::worker_launch>       launch_policy_       = nullptr;
    std::unique_ptr<policy::worker_filetoworker> filetoworker_policy_ = nullptr;
    std::unique_ptr<policy::worker_keepalive>    keepalive_policy_    = nullptr;

public:
    launcher_policy(worker_set& ws, fileid_map& fileid, oneapi::tbb::concurrent_queue<job_ptr>& pending_jobs,
                    std::string const& host, boost::asio::ip::port_type const port):
        worker_set_{ws}, fileid_to_worker_{fileid}, pending_jobs_{pending_jobs},
        announce_host_{host}, announce_port_{port} {}

    auto get_available_worker(pack::packet_pointer packet_ptr) -> df::worker_ptr {
        return filetoworker_policy_->get_available_worker(packet_ptr, worker_set_);
    }

    bool should_start_new_worker() {
        return launch_policy_->should_start_new_worker(pending_jobs_, worker_set_);
    }

    auto get_worker_config() -> std::string& {
        return worker_config_;
    }

    void set_worker_keepalive()
    {
       for (auto&& [worker_ptr, _notused] : worker_set_)
           keepalive_policy_->set_worker_keepalive(worker_ptr);
    }

    auto get_assigned_worker(pack::packet_pointer packet_ptr) -> df::worker_ptr
    {
        fileid_to_worker_accessor it;
        if (bool found = fileid_to_worker_.find(it, packet_ptr->header); found)
            return it->second;
        return nullptr;
    }
};

template<typename Policy>
concept LauncherPolicy = std::is_base_of<Policy, launcher_policy>::value;

} // namespace slsfs::launcher

#endif // LAUNCHER_POLICY_HPP__
