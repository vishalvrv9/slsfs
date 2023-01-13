#pragma once
#ifndef POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL_GLOBAL_HPP__
#define POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL_GLOBAL_HPP__


#include "worker-keepalive.hpp"

#include <boost/log/trivial.hpp>

#include <map>
#include <chrono>
#include <vector>
#include <cstdint>

namespace slsfs::launcher::policy
{

/* Ring buffer implementation of a simple moving average */
class SMA_GLOBAL {
    private:
        std::uint32_t N = 1;
        std::uint32_t index = 0;
        std::vector<std::uint32_t> previousInputs;
        std::uint32_t sum = 0;
        long iter_counter = 1;
        double CV = 0.25;

    public:
    SMA_GLOBAL(std::uint32_t buffer_size) {
            if (buffer_size != 0)
                N = buffer_size;
            previousInputs.resize(N, 0);
        }

        std::uint32_t record(std::uint32_t input) {
            iter_counter += 1;
            sum -= previousInputs[index];
            sum += input;
            previousInputs[index] = input;

            if (++index == N)
                index = 0;

            if (iter_counter < N + 1)
                return (sum + (N / 2)) / iter_counter;
            else
                return (sum + (N / 2)) / N;
        }

        std::uint32_t get_sma() {
            if (iter_counter < N + 1)
                return (sum + (N / 2)) / iter_counter;
            else
                return (sum + (N / 2)) / N;
        }

        template<typename Iterator>
        auto stats(Iterator start, Iterator end) -> int
        {
            int const size = std::distance(start, end);

            double sum = std::accumulate(start, end, 0.0);
            double mean = sum / size, var = 0;

            for (; start != end; start++)
                var += std::pow((*start) - mean, 2);
            var /= size;

            return std::sqrt(var);
        }

        bool is_within_2_stdev() {
            int mean = sum / N;
            int standardDeviation = 0;

            for(std::uint32_t i = 0; i < N; ++i) {
                standardDeviation += pow(previousInputs[i] - mean, 2);
            }

            double stdev = stats(previousInputs.begin(), previousInputs.end());
            BOOST_LOG_TRIVIAL(debug) << "STDEV = " << stdev << ", Mean = " << mean << ", CV = " << stdev / mean;
            if (stdev / mean  < CV)
                return true;
            return false;
        }
};

/* Keeps track of the simple moving average of time intervals between incoming requests
   requests for each worker and adapts the keep alive accordingly */
class keepalive_moving_interval_global : public worker_keepalive
{
    private:
        SMA_GLOBAL sma;
        double error_margin;
        bool first_req = true;
        pack::waittime_type default_wait_time_;
        pack::waittime_type concurrency_threshold;
        std::chrono::high_resolution_clock::time_point last_request_;


    public:
        keepalive_moving_interval_global (
            std::uint32_t buffer_size,
            pack::waittime_type default_ms,
            pack::waittime_type threshold,
            double error_ms):
            sma{buffer_size}, error_margin{error_ms},
            default_wait_time_{default_ms}, concurrency_threshold{threshold} {}

        void set_worker_keepalive(df::worker_ptr worker_ptr_shared) override
        {
            pack::waittime_type keep_alive;
            bool use_sma = sma.is_within_2_stdev();
            BOOST_LOG_TRIVIAL(debug) << "USE SMA: " << use_sma;

            if (sma.get_sma() < concurrency_threshold or !use_sma) {
                keep_alive = default_wait_time_;
            }
            else {
                if (sma.get_sma() > 100) {
                    keep_alive = sma.get_sma() +
                    (error_margin * sma.get_sma()) + 300;
                }
                else {
                    keep_alive = sma.get_sma() + (100 / sma.get_sma()) + 300;
                }
            }

            BOOST_LOG_TRIVIAL(debug) << "SMA sent = " << keep_alive;
            send_worker_keepalive(worker_ptr_shared, keep_alive);
        }

        // Updating the policy
        void started_a_new_job(df::worker* , job_ptr) override
        {
            if (first_req)
            {
                first_req = false;
                last_request_ = std::chrono::high_resolution_clock::now();
                return;
            }

            auto const current_request_time = std::chrono::high_resolution_clock::now();
            auto const last_request_time = last_request_;
            last_request_ = current_request_time;

            auto request_interval = std::chrono::duration_cast<std::chrono::milliseconds>(
                current_request_time - last_request_time).count();

            if (request_interval > concurrency_threshold){
                sma.record(request_interval);
                BOOST_LOG_TRIVIAL(debug) << "recording request interval = " << request_interval
                                         << ", SMA = " << sma.get_sma();
            }
            else{
                BOOST_LOG_TRIVIAL(debug) << "request interval =" << request_interval << ", current SMA =" << sma.get_sma();
            }
        }
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL_GLOBAL_HPP__
