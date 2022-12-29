#pragma once
#ifndef LAUNCHER_HPP__
#define LAUNCHER_HPP__

#include "basic.hpp"
#include "serializer.hpp"
#include "launcher-job.hpp"
#include "launcher-policy.hpp"

#include <oneapi/tbb/concurrent_unordered_set.h>
#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/concurrent_hash_map.h>

#include <iterator>
#include <atomic>

namespace slsfs::launcher
{

template<typename T>
concept CanResolveZookeeper = requires(T t)
{
    { t.get_uuid(std::declval<std::string>()) }
        -> std::convertible_to<net::ip::tcp::endpoint>;
};

// Component responsible for launching jobs and workers
class launcher
{
    net::io_context& io_context_;

    worker_set worker_set_;
    fileid_map fileid_to_worker_;

    job_queue pending_jobs_;
    using jobmap =
        oneapi::tbb::concurrent_unordered_map<
            pack::packet_header,
            job_ptr,
            pack::packet_header_key_hash,
            pack::packet_header_key_compare>;
    jobmap started_jobs_;
    net::io_context::strand started_jobs_strand_, job_launch_strand_;

    uuid::uuid const& id_;
    std::string const announce_host_;
    net::ip::port_type const announce_port_;
    launcher_policy launcher_policy_;

public:
    launcher(net::io_context& io, uuid::uuid const& id,
             std::string const& announce, net::ip::port_type port):
        io_context_{io},
        started_jobs_strand_{io},
        job_launch_strand_{io},
        id_{id},
        announce_host_{announce},
        announce_port_{port},
        launcher_policy_{worker_set_, fileid_to_worker_, pending_jobs_,
                         announce_host_, announce_port_}
        {}

    template<typename PolicyType, typename ... Args>
    void set_policy_filetoworker(Args&& ... args) {
        launcher_policy_.filetoworker_policy_ = std::make_unique<PolicyType>(std::forward<Args>(args)...);
    }

    template<typename PolicyType, typename ... Args>
    void set_policy_launch(Args&& ... args) {
        launcher_policy_.launch_policy_       = std::make_unique<PolicyType>(std::forward<Args>(args)...);
    }

    template<typename PolicyType, typename ... Args>
    void set_policy_keepalive(Args&& ... args) {
        launcher_policy_.keepalive_policy_    = std::make_unique<PolicyType>(std::forward<Args>(args)...);
    }

    void set_worker_config(std::string const& config) {
        launcher_policy_.worker_config_ = config;
    }

    void add_worker(tcp::socket socket, pack::packet_pointer)
    {
        auto worker_ptr = std::make_shared<df::worker>(io_context_, std::move(socket), *this);
        bool ok = worker_set_.emplace(worker_ptr, 0);

        if (not ok)
            BOOST_LOG_TRIVIAL(error) << "Emplace worker not success";

        worker_ptr->start_read_header();
        start_jobs();
    }

    void on_worker_response(pack::packet_pointer pack)
    {
        net::post(
            net::bind_executor(
                started_jobs_strand_,
                [this, pack] () {
                    job_ptr j = started_jobs_[pack->header];
                    j->on_completion_(pack);
                    j->state_ = job::state::finished;
                    BOOST_LOG_TRIVIAL(debug) << "job " << j->pack_->header << " complete";
                }));
    }

    void on_worker_ack(pack::packet_pointer pack)
    {
        net::post(
            net::bind_executor(
                started_jobs_strand_,
                [this, pack] () {
                    BOOST_LOG_TRIVIAL(debug) << "job " << pack->header << " get ack. cancel job timer";
                    if (pack->empty())
                        return;
                    job_ptr j = started_jobs_[pack->header];
                    //BOOST_LOG_TRIVIAL(debug) << "job " << j->pack_->header << " get ack. cancel job timer";
                    if (!j)
                    {
                        BOOST_LOG_TRIVIAL(error) << "get an unknown job: " << pack->header << ". Skip this request";
                        return;
                    }

                    j->state_ = job::state::started;
                    j->timer_.cancel();
                }));
    }

    void on_worker_close(std::shared_ptr<df::worker> worker) {
        worker_set_.erase(worker);
    }

    void start_jobs()
    {
        job_ptr j;
        while (pending_jobs_.try_pop(j))
        {
            BOOST_LOG_TRIVIAL(trace) << "Starting jobs";

            // execute the assigned policy
            // 1. which data function to pick?
            // 2. determine we start a new data function?
            // 3. how long should this data function idle?

            df::worker_ptr worker_ptr = launcher_policy_.get_assigned_worker(j->pack_);

            if (!worker_ptr)
            {
                worker_ptr = launcher_policy_.get_available_worker(j->pack_);

                // emergency start
                if (!worker_ptr)
                {
                    launcher_policy_.set_worker_keepalive();
                    create_worker(launcher_policy_.get_worker_config());
                    pending_jobs_.push(j);
                    break;
                }

                fileid_to_worker_.emplace(j->pack_->header, worker_ptr);
            }

            BOOST_LOG_TRIVIAL(trace) << "Starting jobs, Start post. ";
            started_jobs_.emplace(j->pack_->header, j);
            worker_ptr->start_write(j->pack_);

            using namespace std::chrono_literals;
            j->timer_.expires_from_now(2s);
            j->timer_.async_wait(
                [this, j] (boost::system::error_code ec) {
                    if (ec && ec != boost::asio::error::operation_aborted)
                    {
                        BOOST_LOG_TRIVIAL(debug) << "error: " << ec << "repush job " << j->pack_->header;
                        pending_jobs_.push(j);
                    }
                });
            BOOST_LOG_TRIVIAL(debug) << "start job " << j->pack_->header;
        }

        if (launcher_policy_.should_start_new_worker())
        {
            create_worker(launcher_policy_.get_worker_config());
            launcher_policy_.set_worker_keepalive();
        }
    }

    void create_worker(std::string const& body)
    {
        net::post(
            net::bind_executor(
                job_launch_strand_,
                [this, body] {
                    trigger::make_trigger(io_context_)->start_post(body);
                }));
    }

    template<typename Callback>
    void start_trigger_post(std::string const& body, pack::packet_pointer original_pack, Callback next)
    {
        pack::packet_pointer pack = std::make_shared<pack::packet>();
        pack->header = original_pack->header;
//        pack->header.gen();
        pack->header.type = pack::msg_t::worker_push_request;
        pack->data.buf = std::vector<pack::unit_t>(body.size());
        std::memcpy(pack->data.buf.data(), body.data(), body.size());

        auto j = std::make_shared<job>(io_context_, pack, next);
        pending_jobs_.push(j);
        start_jobs();
    }

    // assumes begin -> end are sorted
    template<std::forward_iterator ForwardIterator, CanResolveZookeeper Zookeeper>
    void reconfigure(ForwardIterator begin, ForwardIterator end, Zookeeper&& zoo)
    {
        BOOST_LOG_TRIVIAL(trace) << "launcher reconfigure";
        for (auto&& pair : fileid_to_worker_)
        {
            auto it = std::upper_bound (begin, end, pair.first.key);
            if (it == end)
                it = begin;

            if (*it != id_)
                start_send_reconfigure_message(pair, zoo.get_uuid(it->encode_base64()));
        }
    }

    void start_send_reconfigure_message(fileid_worker_pair & pair, net::ip::tcp::endpoint new_proxy)
    {
        BOOST_LOG_TRIVIAL(debug) << "start_send_reconfigure_message: " << new_proxy;

        pack::packet_pointer p = std::make_shared<pack::packet>();
        p->header = pair.first;
        p->header.type = pack::msg_t::proxyjoin;

        auto ip = new_proxy.address().to_v4().to_bytes();
        auto port = pack::hton(new_proxy.port());
        p->data.buf = std::vector<pack::unit_t>(sizeof(ip) + sizeof(port));
        std::copy(ip.begin(), ip.end(), p->data.buf.begin());
        std::memcpy(p->data.buf.data() + ip.size(), &port, sizeof(port));

        pair.second->start_write(p);
    }
};

} // namespace launcher


#endif // LAUNCHER_HPP__

//        if (get_available_worker() == nullptr)
//        {
//            BOOST_LOG_TRIVIAL(debug) << "starting function as new worker";
//            create_worker(body);
//            trigger_->register_on_read(
//                [next](std::shared_ptr<http::response<http::string_body>> /*resp*/) {});
//        }
