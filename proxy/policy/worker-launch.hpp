#pragma once
#ifndef POLICY_WORKER_LAUNCH_HPP__
#define POLICY_WORKER_LAUNCH_HPP__

#include "base-types.hpp"

namespace slsfs::launcher::policy
{

/* Resource provisioning policy interface responsible for starting and managing the pool of workers */
class worker_launch
{
public:
    virtual
    bool should_start_new_worker(launcher::job_queue& pending_jobs,
                                 worker_set& current_workers) = 0;
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_LAUNCH_HPP__
