#pragma once
#ifndef LAUNCHER_HPP__
#define LAUNCHER_HPP__

#include "basic.hpp"
#include "serializer.hpp"
#include "launcher-job.hpp"
#include "launcher-policy.hpp"

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

    uuid::uuid const& id_;
    std::string const announce_host_;
    net::ip::port_type const announce_port_;
    launcher_policy launcher_policy_;

public:
    launcher(net::io_context& io, uuid::uuid const& id,
             std::string const& announce, net::ip::port_type port):
        io_context_{io},
        id_{id},
        announce_host_{announce},
        announce_port_{port},
        launcher_policy_{worker_set_, fileid_to_worker_, pending_jobs_,
                         announce_host_, announce_port_} {}

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

        BOOST_LOG_TRIVIAL(info) << "worker set size: " << worker_set_.size();

        if (not ok)
            BOOST_LOG_TRIVIAL(error) << "Emplace worker not success";

        worker_ptr->start_read_header();
        start_jobs();
    }

    void on_worker_reschedule(job_ptr job)
    {
        BOOST_LOG_TRIVIAL(debug) << "job " << job->pack_->header << " reschedule";
        pending_jobs_.push(job);
        fileid_to_worker_.erase(job->pack_->header);
    }

    void on_worker_close(df::worker_ptr worker)
    {
        BOOST_LOG_TRIVIAL(info) << "on_worker_close: " << worker.get();
        worker_set_.erase(worker);
        start_jobs();
    }

    void start_jobs()
    {
        if (launcher_policy_.should_start_new_worker())
        {
            create_worker(launcher_policy_.get_worker_config());
            launcher_policy_.set_worker_keepalive();
        }

        BOOST_LOG_TRIVIAL(debug) << "pending job count=" << pending_jobs_.unsafe_size();
        job_ptr j;
        while (pending_jobs_.try_pop(j))
        {
            BOOST_LOG_TRIVIAL(trace) << "Starting jobs";

            // execute the assigned policy
            // 1. which data function to pick?
            // 2. determine we start a new data function?
            // 3. how long should this data function idle?

            df::worker_ptr worker_ptr = launcher_policy_.get_assigned_worker(j->pack_);

            if (!worker_ptr || not worker_ptr->is_valid())
            {
                worker_ptr = launcher_policy_.get_available_worker(j->pack_);

                // no avaliable worker. Reassign
                if (!worker_ptr || not worker_ptr->is_valid())
                {
                    BOOST_LOG_TRIVIAL(error) << "no avaliable worker. Reschedule";
                    pending_jobs_.push(j);
                    break;
                }

                fileid_to_worker_.emplace(j->pack_->header, worker_ptr);
            }

            BOOST_LOG_TRIVIAL(trace) << "Starting jobs, Start post. ";
            worker_ptr->start_write(j);

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
    }

    void create_worker(std::string const& body) {
        trigger::make_trigger(io_context_)->start_post(body);
    }

    template<typename Callback>
    void start_trigger_post(std::string const& body, pack::packet_pointer original_pack, Callback next)
    {
        pack::packet_pointer pack = std::make_shared<pack::packet>();
        pack->header      = original_pack->header;
        pack->header.type = pack::msg_t::worker_push_request;
        pack->data.buf    = std::vector<pack::unit_t>(body.size());
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

        // may change to job ptr
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
