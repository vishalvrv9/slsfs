#pragma once
#ifndef LAUNCHER_POLICY_HPP__
#define LAUNCHER_POLICY_HPP__

#include "basic.hpp"
#include "serializer.hpp"
#include "worker.hpp"
#include "uuid.hpp"
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
    net::io_context& io_context_;
    worker_set& worker_set_;
    std::string const announce_host_;
    net::ip::port_type const announce_port_;
    reporter reporter_;

public:
    std::string worker_config_;
    std::unique_ptr<policy::worker_launch>       launch_policy_       = nullptr;
    std::unique_ptr<policy::worker_filetoworker> filetoworker_policy_ = nullptr;
    std::unique_ptr<policy::worker_keepalive>    keepalive_policy_    = nullptr;

public:
    launcher_policy(net::io_context& ioc, worker_set& ws,
                    std::string const& host, net::ip::port_type const port,
                    std::string const& save_report):
        io_context_{ioc}, worker_set_{ws},
        announce_host_{host}, announce_port_{port},
        reporter_{save_report} {}

    auto get_available_worker(pack::packet_pointer ptr) -> df::worker_ptr {
        return filetoworker_policy_->get_available_worker(ptr, worker_set_);
    }

    int get_ideal_worker_count_delta()
    {
        int want_start = launch_policy_->get_ideal_worker_count_delta(worker_set_);
        return want_start;
    }

    auto get_worker_config() -> std::string& {
        return worker_config_;
    }

    void set_worker_keepalive()
    {
        for (auto [worker_ptr, _unused] : worker_set_)
            if (worker_ptr->is_valid())
                keepalive_policy_->set_worker_keepalive(worker_ptr);
    }

    void start_transfer() {
        filetoworker_policy_->start_transfer();
    }

    auto get_assigned_worker(pack::packet_pointer packet_ptr) -> df::worker_ptr {
        return filetoworker_policy_->get_assigned_worker(packet_ptr);
    }

    // methods for updates; defined in base_types.hpp
    void execute()
    {
        net::post(
            io_context_,
            [this](){
                keepalive_policy_   ->execute();
                set_worker_keepalive();
                launch_policy_      ->execute();
                filetoworker_policy_->execute();
                reporter_.execute();
            });
    }

    void started_a_new_job(df::worker* worker_ptr, job_ptr job)
    {
        net::post(
            io_context_,
            [this, worker_ptr, job] () {
                keepalive_policy_   ->started_a_new_job(worker_ptr, job);
                launch_policy_      ->started_a_new_job(worker_ptr, job);
                filetoworker_policy_->started_a_new_job(worker_ptr, job);
                reporter_.started_a_new_job(worker_ptr, job);
            });
    }

    void finished_a_job(df::worker* worker_ptr, job_ptr job)
    {
        net::post(
            io_context_,
            [this, worker_ptr, job] () {
                keepalive_policy_   ->finished_a_job(worker_ptr, job);
                launch_policy_      ->finished_a_job(worker_ptr, job);
                filetoworker_policy_->finished_a_job(worker_ptr, job);
                reporter_.finished_a_job(worker_ptr, job);
            });
    }

    void starting_a_new_worker()
    {
        net::post(
            io_context_,
            [this] () {
                keepalive_policy_   ->starting_a_new_worker();
                launch_policy_      ->starting_a_new_worker();
                filetoworker_policy_->starting_a_new_worker();
                reporter_.starting_a_new_worker();
            });
    }

    void registered_a_new_worker(df::worker* worker_ptr)
    {
        net::post(
            io_context_,
            [this, worker_ptr] () {
                keepalive_policy_   ->registered_a_new_worker(worker_ptr);
                launch_policy_      ->registered_a_new_worker(worker_ptr);
                filetoworker_policy_->registered_a_new_worker(worker_ptr);
                reporter_.registered_a_new_worker(worker_ptr);
            });
    }

    void deregistered_a_worker(df::worker* worker_ptr)
    {
        net::post(
            io_context_,
            [this, worker_ptr] () {
                keepalive_policy_   ->deregistered_a_worker(worker_ptr);
                launch_policy_      ->deregistered_a_worker(worker_ptr);
                filetoworker_policy_->deregistered_a_worker(worker_ptr);
                reporter_.deregistered_a_worker(worker_ptr);
            });
    }
};

template<typename Policy>
concept LauncherPolicy = std::is_base_of<Policy, launcher_policy>::value;

} // namespace slsfs::launcher

#endif // LAUNCHER_POLICY_HPP__
