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
protected:
    int const max_outstanding_starting_request_;
    std::atomic<int> starting_worker_count_ = 0;

public:
    worker_launch(int max_outstanding_starting_request):
        max_outstanding_starting_request_{max_outstanding_starting_request} {}

    void starting_a_new_worker() override {
        starting_worker_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void registered_a_new_worker(df::worker*) override {
        starting_worker_count_.fetch_sub(1, std::memory_order_relaxed);
    }

    int get_ideal_worker_count_delta (worker_set& ws)
    {
//        BOOST_LOG_TRIVIAL(info) << "get_ideal_worker_count(ws): " << get_ideal_worker_count(ws)
//                                << " ws.size(): " << ws.size()
//                                << " starting_worker_count_: " << starting_worker_count_;

        int should_start = std::max(get_ideal_worker_count(ws) - (static_cast<int>(ws.size()) + starting_worker_count_.load()), 0);
        int can_start    = std::max(max_outstanding_starting_request_ - starting_worker_count_.load(), 0);

//        BOOST_LOG_TRIVIAL(info) << "calc: " << std::min(should_start, can_start);
        return std::min(should_start, can_start);
    }

    virtual
    int get_ideal_worker_count (worker_set &) { return 0; }
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_LAUNCH_HPP__
