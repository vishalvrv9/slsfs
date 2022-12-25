#pragma once
#ifndef POLICY_WORKER_KEEPALIVE_CONST_TIME
#define POLICY_WORKER_KEEPALIVE_CONST_TIME

#include "base-types.hpp"
#include "worker-keepalive.hpp"

namespace slsfs::launcher::policy
{

/* Resource provisioning policy interface responsible for deciding a keep alive time for workers after
they have processed their last request */
class keepalive_const_time : public worker_keepalive
{
    pack::waittime_type waittime_;

public:
    keepalive_const_time (pack::waittime_type ms): waittime_{ms} {}

    void set_worker_keepalive(df::worker_ptr worker) override {
        send_worker_keepalive(worker, waittime_);
    }
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_KEEPALIVE_CONST_TIME
