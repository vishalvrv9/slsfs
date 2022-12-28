#pragma once
#ifndef LAUNCHER_JOB_HPP__
#define LAUNCHER_JOB_HPP__

#include "basic.hpp"
#include "serializer.hpp"
#include "worker.hpp"
#include "uuid.hpp"

#include <boost/signals2.hpp>
#include <boost/asio.hpp>
#include <iterator>
#include <atomic>

namespace slsfs::launcher
{

// Specifies a READ or WRITE jobs managed by launcher
class job
{
public:
    enum class state : std::uint8_t
    {
        registered,
        started,
        finished
    };
    state state_ = state::registered;

    using on_completion_callable = boost::signals2::signal<void (pack::packet_pointer)>;
    on_completion_callable on_completion_;
    pack::packet_pointer pack_;

    boost::asio::steady_timer timer_;

    template<typename Next>
    job (net::io_context& ioc, pack::packet_pointer p, Next && next):
        pack_{p}, timer_{ioc} { on_completion_.connect(next); }
};

using job_ptr = std::shared_ptr<job>;
using job_queue = oneapi::tbb::concurrent_queue<job_ptr>;

} // namespace slsfs::launcher

#endif // LAUNCHER_JOB_HPP__