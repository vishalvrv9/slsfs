#pragma once
#ifndef LAUNCHER_HPP__
#define LAUNCHER_HPP__

#include "basic.hpp"
#include "serializer.hpp"
#include "launcher-job.hpp"
#include "launcher-policy.hpp"
#include "uuid.hpp"

#include <oneapi/tbb/concurrent_queue.h>
#include <oneapi/tbb/concurrent_hash_map.h>

#include <iterator>
#include <atomic>
#include <mutex>

namespace slsfs::launcher
{

template<typename T>
concept CanResolveZookeeper = requires(T t)
{
    { t.get_uuid(std::declval<std::string>()) }
        -> std::convertible_to<net::ip::tcp::endpoint>;
};


struct transfer_request
{
    std::vector<slsfs::pack::packet_header> file_bindings;
    slsfs::pack::packet_pointer cache_tables;
};

using transfer_queue = oneapi::tbb::concurrent_queue<transfer_request>;

// Component responsible for launching jobs and workers
class launcher
{
    net::io_context& io_context_;
    net::io_context::strand launcher_strand_;
    uuid::uuid const& id_;
    bool const enable_direct_datafunction_;
    std::string const announce_host_;
    net::ip::port_type const announce_port_;

    worker_set worker_set_;
    launcher_policy launcher_policy_;
    transfer_queue transfer_requests_;

    void start_execute_policy()
    {
        using namespace std::chrono_literals;
        auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
        timer->expires_from_now(1s);
        timer->async_wait(
            [this, timer] (boost::system::error_code error) {
                switch (error.value())
                {
                case boost::system::errc::success: // timer timeout
                    launcher_policy_.execute();
                    start_create_worker_with_policy();
                    start_execute_policy();
                    break;

                case boost::system::errc::operation_canceled: // timer canceled
                    BOOST_LOG_TRIVIAL(error) << "getting an operation_aborted error on launcher start_execute_policy()";
                    break;
                default:
                    BOOST_LOG_TRIVIAL(error) << "getting error: " << error.message() << " on launcher start_execute_policy()";
                }
            });
    }

    void start_create_worker_with_policy()
    {
        net::post(
            launcher_strand_,
            [this] {
                int const should_start = launcher_policy_.get_ideal_worker_count_delta();

                if (should_start != 0)
                    BOOST_LOG_TRIVIAL(trace) << "Starting worker of size: " << should_start;
                for (int i = 0; i < should_start; i++)
                {
                    create_worker (
                        launcher_policy_.get_worker_config().post_string,
                        launcher_policy_.get_worker_config().max_func_count);
                    launcher_policy_.start_transfer();
                }
            });
    }

    void create_worker (std::string const& body, int const max_func_count = 0)
    {
        launcher_policy_.starting_a_new_worker();

        trigger::make_trigger(io_context_, max_func_count)
            ->register_on_read(
                [](std::shared_ptr<slsfs::http::response<slsfs::http::string_body>> res) {
                    BOOST_LOG_TRIVIAL(info) << "openwhisk response: " << res->body();
                })
            .start_post(body);
    }

public:
    launcher(net::io_context& io, uuid::uuid const& id, bool enable_direct_datafunction,
             std::string const& announce, net::ip::port_type port,
             std::string const& save_report):
        io_context_{io},
        launcher_strand_{io},
        id_{id},
        enable_direct_datafunction_{enable_direct_datafunction},
        announce_host_{announce},
        announce_port_{port},
        launcher_policy_{io, worker_set_,
                         announce_host_, announce_port_,
                         save_report} {
        start_execute_policy();
    }

    template<typename PolicyType, typename ... Args>
    void set_policy_filetoworker (Args&& ... args) {
        launcher_policy_.filetoworker_policy_ = std::make_unique<PolicyType>(std::forward<Args>(args)...);
    }

    template<typename PolicyType, typename ... Args>
    void set_policy_launch (Args&& ... args) {
        launcher_policy_.launch_policy_       = std::make_unique<PolicyType>(std::forward<Args>(args)...);
    }

    template<typename PolicyType, typename ... Args>
    void set_policy_keepalive (Args&& ... args) {
        launcher_policy_.keepalive_policy_    = std::make_unique<PolicyType>(std::forward<Args>(args)...);
    }

    template<typename ... Args>
    void set_worker_config (Args && ... args) {
        launcher_policy_.worker_config_ = worker_config(std::forward<Args>(args)...);
    }

    auto get_fileids_from_transfer(slsfs::pack::packet_pointer cache_table)
        -> std::vector<pack::packet_header>
    {
        std::vector<pack::packet_header> headers;

        for (auto it = cache_table->data.buf.begin(); it < cache_table->data.buf.end(); it++)
        {
            pack::packet_header ph;
            std::copy_n(it, 32, ph.key.begin());
            std::advance(it, 32);
            it = std::find (it, cache_table->data.buf.end(), ' ');
            headers.push_back(ph);
        }
        return headers;
    }

    auto fileid_to_worker() -> fileid_map& {
        return launcher_policy_.filetoworker_policy_->fileid_to_worker_;
    }

    void add_worker (tcp::socket socket, pack::packet_pointer worker_info)
    {
        auto worker_ptr = std::make_shared<df::worker>(io_context_, std::move(socket), *this);

        // read first 8 bytes of ip:port and proceed of the table reading

        boost::asio::ip::address_v4::bytes_type host;
        std::memcpy(&host, worker_info->data.buf.data(), sizeof(host));
        boost::asio::ip::address address = boost::asio::ip::make_address_v4(host);

        std::uint16_t port = 0;
        std::memcpy(&port, worker_info->data.buf.data() + sizeof(host), sizeof(port));
        port = pack::ntoh(port);

        boost::asio::ip::tcp::endpoint endpoint(address, port);

        bool ok = worker_set_.emplace(worker_ptr, endpoint);
        if (not ok)
            BOOST_LOG_TRIVIAL(error) << "Emplace worker not success. Current worker count: " << worker_set_.size();
        else
            BOOST_LOG_TRIVIAL(info) << "Get new worker [" << worker_ptr->id_.short_hash() << "] @ " << endpoint <<". Active worker count: " << worker_set_.size();

        // cache table reading

        bool cache_transfer = false;
        transfer_request pending_transfer;
        if (transfer_requests_.try_pop(pending_transfer))
        {
            BOOST_LOG_TRIVIAL(trace) << "(launcher.add_worker) found pending transfer request, attaching to new worker";
            for (slsfs::pack::packet_header const& file_binding : pending_transfer.file_bindings)
            {
                fileid_map::const_accessor acc;
                if (fileid_to_worker().find(acc, file_binding))
                {
                    if (!acc->second->is_valid())
                    {
                        fileid_to_worker().emplace(file_binding, worker_ptr);
                        BOOST_LOG_TRIVIAL(trace) << "(launcher.add_worker) adding a file_binding";
                        continue;
                    }
                    BOOST_LOG_TRIVIAL(trace) << "(launcher.add_worker) file_binding already assigned";
                }
                else
                {
                    BOOST_LOG_TRIVIAL(trace) << "(launcher.add_worker) adding a file_binding";
                    fileid_to_worker().emplace(file_binding, worker_ptr);
                }
            }
            BOOST_LOG_TRIVIAL(trace) << "(launcher.add_worker) sending cache transfer request to new worker";
            pending_transfer.cache_tables->header.type = slsfs::pack::msg_t::cache_transfer;
            worker_ptr->start_write(pending_transfer.cache_tables);
            cache_transfer = true;
        }

        launcher_policy_.registered_a_new_worker(worker_ptr.get(), cache_transfer);
        worker_ptr->start_read_header();
    }

    void on_worker_reschedule (job_ptr job)
    {
        BOOST_LOG_TRIVIAL(trace) << "job " << job->pack_->header << " reschedule due to worker close";
        fileid_to_worker().erase(job->pack_->header);
        reschedule (job);
    }

    void on_worker_close (df::worker_ptr worker, slsfs::pack::packet_pointer to_transfer)
    {
        std::uint32_t cache_hits = 0;
        std::uint32_t cache_evictions = 0;

        if (to_transfer)
        {
            std::memcpy(&cache_hits, to_transfer->data.buf.data() + to_transfer->data.buf.size() - 8, 4);
            std::memcpy(&cache_evictions, to_transfer->data.buf.data() + to_transfer->data.buf.size() - 4, 4);

            to_transfer->data.buf.erase(
                to_transfer->data.buf.begin() + to_transfer->data.buf.size() - 8,
                to_transfer->data.buf.end());

            std::vector<slsfs::pack::packet_header> file_bindings =
                get_fileids_from_transfer(to_transfer);

            BOOST_LOG_TRIVIAL(debug) << "(launcher.on_worker_close) queueing a transfer request total file bindings: "<< file_bindings.size();
            transfer_requests_.push(transfer_request(file_bindings, to_transfer));
        }

        worker_set_.erase(worker);
        launcher_policy_.deregistered_a_worker(worker.get(), cache_hits, cache_evictions);
    }

    void on_worker_finished_a_job (df::worker* worker, job_ptr job) {
        launcher_policy_.finished_a_job(worker, job);
    }

    void process_job (job_ptr job)
    {
        df::worker_ptr worker_ptr = launcher_policy_.get_assigned_worker(job->pack_);
        if (!worker_ptr || not worker_ptr->is_valid())
        {
            worker_ptr = launcher_policy_.get_available_worker(job->pack_);

            // no available worker. Re-scheduling request
            if (!worker_ptr || not worker_ptr->is_valid())
            {
                //BOOST_LOG_TRIVIAL(trace) << "No available worker. Re-scheduling request";
                //start_create_worker_with_policy();
                reschedule(job);
                return;
            }

            //worker_ptr->soft_close();
            fileid_to_worker().emplace(job->pack_->header, worker_ptr);
        }

        BOOST_LOG_TRIVIAL(trace) << "Starting jobs, Start post.";

        // async launch stat calculator
        launcher_policy_.started_a_new_job(worker_ptr.get(), job);

        worker_ptr->start_write(job);

        using namespace std::chrono_literals;
        job->timer_.expires_from_now(2s);
        job->timer_.async_wait(
            [this, job] (boost::system::error_code ec) {
                if (ec && ec != net::error::operation_aborted)
                {
                    BOOST_LOG_TRIVIAL(error) << "error: " << ec << "repush job " << job->pack_->header;
                    reschedule(job);
                }
            });
        BOOST_LOG_TRIVIAL(trace) << "job started" << job->pack_->header;
    }

    template<typename Next>
    void process_job_with_worker (pack::packet_pointer pack, Next next)
    {
        df::worker_ptr worker_ptr = launcher_policy_.get_assigned_worker(pack);
        if (!worker_ptr || not worker_ptr->is_valid())
        {
            worker_ptr = launcher_policy_.get_available_worker(pack);

            // no available worker. Re-scheduling request
            if (!worker_ptr || not worker_ptr->is_valid())
            {
                //BOOST_LOG_TRIVIAL(trace) << "No available worker. Re-scheduling request";
                //start_create_worker_with_policy();

                // need to post in io context to avoid segmentation fault (stack overflow)
                net::post(io_context_,
                          [this, pack, next] {
                              process_job_with_worker (pack, next);
                          });
                return;
            }

            //worker_ptr->soft_close();
            fileid_to_worker().emplace(pack->header, worker_ptr);
        }

        BOOST_LOG_TRIVIAL(trace) << "Starting job worker";

        std::invoke(next, worker_ptr);
    }

    template<typename Callback>
    void start_trigger_post (std::vector<slsfs::pack::unit_t> & body, pack::packet_pointer original_pack, Callback next)
    {
        pack::packet_pointer pack = std::make_shared<pack::packet>();
        pack->header      = original_pack->header;
        pack->header.type = pack::msg_t::worker_push_request;
        std::swap(pack->data.buf, body);

        if (enable_direct_datafunction_)
            process_job_with_worker(
                pack,
                [this, pack, original_pack, next=std::move(next)] (df::worker_ptr worker_ptr) {
                    boost::asio::ip::tcp::endpoint endpoint;
                    {
                        worker_set_accessor acc;
                        if (worker_set_.find (acc, worker_ptr))
                            endpoint = acc->second;
                    }

                    boost::asio::ip::address_v4::bytes_type bytes = endpoint.address().to_v4().to_bytes();
                    std::uint16_t port = pack::hton(endpoint.port());

                    pack->data.buf.resize(sizeof(bytes) + sizeof(port));
                    std::memcpy(pack->data.buf.data(), &bytes, sizeof(bytes));
                    std::memcpy(pack->data.buf.data() + sizeof(bytes),
                                std::addressof(port), sizeof(port));

                    std::invoke(next, pack);
                });
        else
            schedule(std::make_shared<job>(io_context_, pack, next));
    }

    void schedule (job_ptr job)
    {
        launcher_policy_.schedule_a_new_job(job);
        net::post(io_context_, [this, job] { process_job(job); });
    }

    void reschedule (job_ptr job)
    {
        launcher_policy_.reschedule_a_job(job);
        net::post(io_context_, [this, job] { process_job(job); });
    }

    // assumes begin -> end are sorted
    template<std::forward_iterator ForwardIterator, CanResolveZookeeper Zookeeper>
    void reconfigure (ForwardIterator begin, ForwardIterator end, Zookeeper&& zoo)
    {
        BOOST_LOG_TRIVIAL(trace) << "launcher reconfigure";
        for (auto&& pair : fileid_to_worker())
        {
            auto it = std::upper_bound (begin, end, pair.first.key);
            if (it == end)
                it = begin;

            if (*it != id_)
                start_send_reconfigure_message(pair, zoo.get_uuid(it->encode_base64()));
        }
    }

    void start_send_reconfigure_message (fileid_worker_pair & pair, net::ip::tcp::endpoint new_proxy)
    {
        BOOST_LOG_TRIVIAL(trace) << "start_send_reconfigure_message: " << new_proxy;

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
