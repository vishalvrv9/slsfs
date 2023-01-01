#pragma once
#ifndef POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL
#define POLICY_WORKER_KEEPALIVE_MOVING_INTERVAL

#include "base-types.hpp"
#include "worker-keepalive.hpp"

#include <map>

namespace slsfs::launcher::policy
{

/* Keeps track of the interval between requests for each worker and adapts the keep alive accordingly */
class keepalive_moving_interval : public worker_keepalive
{
    pack::waittime_type default_waittime_;
    std::map<df::worker_ptr, pack::waittime_type> waittimes_;

public:
    keepalive_moving_interval (pack::waittime_type ms): waittime_{ms} {}

    void set_worker_keepalive(df::worker_ptr worker) override {
        send_worker_keepalive(worker, waittime_);
    }
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_KEEPALIVE_CONST_TIME
