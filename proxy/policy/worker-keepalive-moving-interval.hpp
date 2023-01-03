#pragma once
#ifndef POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL_HPP__
#define POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL_HPP__

#include "base-types.hpp"
#include "worker-keepalive.hpp"

#include <map>
#include <chrono>

namespace slsfs::launcher::policy
{

/* Keeps track of the interval between requests for each worker and adapts the keep alive accordingly */
class keepalive_moving_interval : public worker_keepalive
{
private:
    pack::waittime_type default_wait_time_ = 0;
    std::map<df::worker_ptr, pack::waittime_type> wait_times_;
    std::map<df::worker_ptr, long> average_count_;
    std::map<df::worker_ptr, std::chrono::high_resolution_clock::time_point> last_request_;

    // Adjust this to not strictly follow average and give a buffer
    pack::waittime_type rolling_average(pack::waittime_type current_avg, std::size_t new_value, long count) {
        return ((current_avg * count) + new_value) / (count + 1);
    }

public:
    keepalive_moving_interval (pack::waittime_type ms): default_wait_time_{ms} {}

    void set_worker_keepalive(df::worker_ptr worker_ptr) override
    {
        if (average_count_.count(worker_ptr) == 0)
        {
            wait_times_[worker_ptr] = default_wait_time_;
            average_count_[worker_ptr] = 0;
            last_request_[worker_ptr] = std::chrono::high_resolution_clock::now();
            send_worker_keepalive(worker_ptr, wait_times_[worker_ptr]);
            return;
        }

        auto const current_request_time = std::chrono::high_resolution_clock::now();
        auto const last_request_time = last_request_[worker_ptr];

        auto request_interval = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_request_time - last_request_time).count();

        pack::waittime_type new_average;
        if (average_count_[worker_ptr] == 1)
            new_average = request_interval;
        else
        {
            new_average = rolling_average(
                wait_times_[worker_ptr],
                request_interval,
                average_count_[worker_ptr]);
        }

        wait_times_[worker_ptr] = new_average;
        average_count_[worker_ptr] = average_count_[worker_ptr] + 1;
        last_request_[worker_ptr] = current_request_time;

        send_worker_keepalive(worker_ptr, wait_times_[worker_ptr]);
    }
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL_HPP__
