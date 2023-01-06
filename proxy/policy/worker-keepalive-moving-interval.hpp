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

/* Ring buffer implementation of a simple moving average */
template <std::uint16_t N, class input_t = std::uint16_t, class sum_t = std::uint32_t>
class SMA {
    private:
        std::uint16_t index       = 0;
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

        input_t get_sma() {
            return (sum + (N / 2)) / N;
        }
};

/* Keeps track of the interval between requests for each worker and adapts the keep alive accordingly */
class keepalive_moving_interval : public worker_keepalive
{
    private:
        pack::waittime_type default_wait_time_;
        std::map<df::worker_id, SMA<20>> wait_times_;
        // std::map<df::worker*, long> average_count_;
        std::map<df::worker_id, std::chrono::high_resolution_clock::time_point> last_request_;

        // Simple moving average: this will keep track of all time intervals between requests since
        // the first request recorded
        // pack::waittime_type simple_rolling_average(
        //     pack::waittime_type current_avg,
        //     std::size_t new_value,
        //     long count)
        // {
        //     auto accumulator = ((current_avg * count) + new_value) / (count + 1);
        //     return accumulator;
        // }

        // Exponential moving average: this will keep track of recent requests with a higher weight
        // adjusting alpha adjusts the weight by which the moving average incorporates the new value
        // Example: an alpha of 1/1000 is comparable to the effect of considering the last 1000 samples
        // pack::waittime_type exponential_rolling_average(
        //     pack::waittime_type current_avg,
        //     std::size_t new_value,
        //     double alpha)
        // {
        //     auto accumulator = (alpha * new_value) + (1.0 - alpha) * current_avg;
        //     return accumulator;
        // }

    public:
        keepalive_moving_interval (pack::waittime_type ms): default_wait_time_{ms} {}

    // updated the interface here: df::worker_ptr have high overhead than df::worker*
    // to convert between these two:
    //   df::worker_ptr => df::worker*    use .get()
    //   df::worker*    => df::worker_ptr use ->shared_from_this()
        // setting the worker keep alive
        void set_worker_keepalive(df::worker_ptr worker_ptr_shared) override
        {
            df::worker_id worker_ptr = worker_ptr_shared->worker_id_;

            pack::waittime_type keep_alive;

            if (wait_times_[worker_ptr].get_sma() < 1) {
                keep_alive = default_wait_time_;
            }
            else {
                if (wait_times_[worker_ptr].get_sma() > 100) {
                    keep_alive = wait_times_[worker_ptr].get_sma() +
                    (0.5 * wait_times_[worker_ptr].get_sma());
                }
                else {
                    keep_alive = wait_times_[worker_ptr].get_sma() + 
                    (100 / wait_times_[worker_ptr].get_sma());
                }
            }

            BOOST_LOG_TRIVIAL(info) << "SMA sent = " << keep_alive;
            send_worker_keepalive(worker_ptr_shared, keep_alive);
        }

        // Updating the policy
        void started_a_new_job(df::worker* worker_ptr) override
        {
            df::worker_id worker_id = worker_ptr->worker_id_;
            if (last_request_.count(worker_id) == 0)
            {
                wait_times_[worker_id] = SMA<20>();
                wait_times_[worker_id](default_wait_time_);
                last_request_[worker_id] = std::chrono::high_resolution_clock::now();
                return;
            }

            auto const current_request_time = std::chrono::high_resolution_clock::now();
            auto const last_request_time = last_request_[worker_id];
            last_request_[worker_id] = current_request_time;

            auto request_interval = std::chrono::duration_cast<std::chrono::milliseconds>(
                current_request_time - last_request_time).count();


            if (request_interval > 10){
                BOOST_LOG_TRIVIAL(info) << "request interval = " << request_interval;
                BOOST_LOG_TRIVIAL(info) << "SMA update = " << wait_times_[worker_id](request_interval);
            }

            // pack::waittime_type new_average;
            // if (average_count_[worker_id] == 1){
            //     new_average = request_interval;
            // }
            // else
            // {
            //     new_average = exponential_rolling_average(
            //         wait_times_[worker_id],
            //         request_interval,
            //         1/1000);
            // }

            // wait_times_[worker_id] = new_average;
            // average_count_[worker_id] = average_count_[worker_id] + 1;


            // BOOST_LOG_TRIVIAL(info) << "EMA AVERAGE FOR CURRENT WORKER: " << new_average * 10;
            // BOOST_LOG_TRIVIAL(info) << "SMA AVERAGE FOR CURRENT WORKER: " << sma * 10;
            // 3/4 of the average added as a buffer to allow for potentially late requests.
            // Make it so the buffer is larger for smaller values and smaller for bigger
            // send_worker_keepalive(worker_id,  * error_multiplier);
        }
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL_HPP__
