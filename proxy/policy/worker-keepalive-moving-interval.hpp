#pragma once
#ifndef POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL_HPP__
#define POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL_HPP__


#include "worker-keepalive.hpp"

#include <boost/log/trivial.hpp>

#include <map>
#include <chrono>
#include <cstdint>

namespace slsfs::launcher::policy
{

template <std::uint16_t N, class input_t = std::uint16_t, class sum_t = std::uint32_t>
class SMA {
    private:
        std::uint16_t index            = 0;
        input_t previousInputs[N] = {};
        sum_t sum                 = 0;

    public:
        input_t operator() (input_t input) {
            sum -= previousInputs[index];
            sum += input;
            previousInputs[index] = input;
            if (++index == N)
                index = 0;
            return (sum + (N / 2)) / N;
        }
};

/* Keeps track of the interval between requests for each worker and adapts the keep alive accordingly */
class keepalive_moving_interval : public worker_keepalive
{
    private:
        pack::waittime_type default_wait_time_;
        std::map<df::worker_ptr, SMA<500>> wait_times_;
        std::map<df::worker_ptr, long> average_count_;
        std::map<df::worker_ptr, std::chrono::high_resolution_clock::time_point> last_request_;

        // Simple moving average: this will keep track of all time intervals between requests since
        // the first request recorded
        pack::waittime_type simple_rolling_average(
            pack::waittime_type current_avg,
            std::size_t new_value,
            long count)
        {
            auto accumulator = ((current_avg * count) + new_value) / (count + 1);
            return accumulator;
        }

        // Exponential moving average: this will keep track of recent requests with a higher weight
        // adjusting alpha adjusts the weight by which the moving average incorporates the new value
        // Example: an alpha of 1/1000 is comparable to the effect of considering the last 1000 samples
        pack::waittime_type exponential_rolling_average(
            pack::waittime_type current_avg,
            std::size_t new_value,
            double alpha)
        {
            auto accumulator = (alpha * new_value) + (1.0 - alpha) * current_avg;
            return accumulator;
        }

    public:
        keepalive_moving_interval (pack::waittime_type ms): default_wait_time_{ms} {}

        void set_worker_keepalive(df::worker_ptr worker_ptr) override
        {
            send_worker_keepalive(worker_ptr, 10000);
        }

    // Ryan: I added new method here;
    // every time launcher starts a job, it calls this function
    // define in base-types.hpp
    // for the set_worker_keepalive() function, it will be called every 1 seconds.
    // can you override the set_worker_keepalive() function to set the timeout value?
        void started_a_new_job(df::worker_ptr worker_ptr) override
        {
            set_worker_keepalive_souf(worker_ptr);
        }

        void set_worker_keepalive_souf(df::worker_ptr worker_ptr)
        {
            if (average_count_.count(worker_ptr) == 0)
            {
                wait_times_[worker_ptr] = SMA<500>();
                // average_count_[worker_ptr] = 0;
                // last_request_[worker_ptr] = std::chrono::high_resolution_clock::now();
                send_worker_keepalive(worker_ptr, default_wait_time_);
                return;
            }

            auto const current_request_time = std::chrono::high_resolution_clock::now();
            auto const last_request_time = last_request_[worker_ptr];

            [[maybe_unused]]
            auto request_interval = std::chrono::duration_cast<std::chrono::milliseconds>(
                current_request_time - last_request_time).count();


            // pack::waittime_type new_average;
            // if (average_count_[worker_ptr] == 1){
            //     new_average = request_interval;
            // }
            // else
            // {
            //     new_average = exponential_rolling_average(
            //         wait_times_[worker_ptr],
            //         request_interval,
            //         1/1000);
            // }

            // wait_times_[worker_ptr] = new_average;
            // average_count_[worker_ptr] = average_count_[worker_ptr] + 1;
            // last_request_[worker_ptr] = current_request_time;

            [[maybe_unused]]
            pack::waittime_type error_multiplier = 10;

            // BOOST_LOG_TRIVIAL(info) << "EMA AVERAGE FOR CURRENT WORKER: " << new_average * 10;
            // BOOST_LOG_TRIVIAL(info) << "SMA AVERAGE FOR CURRENT WORKER: " << sma * 10;
            // 3/4 of the average added as a buffer to allow for potentially late requests.
            // Make it so the buffer is larger for smaller values and smaller for bigger
//            send_worker_keepalive(worker_ptr, wait_times_[worker_ptr](request_interval) * error_multiplier);
        }
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL_HPP__
