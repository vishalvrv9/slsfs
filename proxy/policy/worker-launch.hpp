#pragma once
#ifndef POLICY_WORKER_LAUNCH_HPP__
#define POLICY_WORKER_LAUNCH_HPP__

#include "../launcher-base-types.hpp"
#include <atomic>

namespace slsfs::launcher::policy
{

/* Resource provisioning policy interface responsible for starting and managing the pool of workers */
class worker_launch : public info
{
    unsigned int const max_outstanding_starting_request_ = 7;
    std::atomic<unsigned int> counter_ = 0;

public:
    void starting_a_new_worker() override {
        counter_.fetch_add(1, std::memory_order_relaxed);
    }

    void registered_a_new_worker(df::worker*) override {
        counter_.fetch_sub(1, std::memory_order_relaxed);
    }

    bool reaches_max_pending_start_worker_requests() {
        return counter_.load() > max_outstanding_starting_request_;
    }

    virtual
    bool should_start_new_worker(launcher::job_queue& pending_jobs, worker_set& current_workers)
    {
        using namespace std::chrono_literals;
        pending_jobs.unsafe_size();
        if (reaches_max_pending_start_worker_requests())
            return false;

        return current_workers.empty();
    }
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_LAUNCH_HPP__
