#pragma once
#ifndef POLICY_WORKER_FILETOWORKER_LOWEST_LOAD_HPP__
#define POLICY_WORKER_FILETOWORKER_LOWEST_LOAD_HPP__

#include "base-types.hpp"

#include "worker-filetoworker.hpp"

#include <limits>

namespace slsfs::launcher::policy
{

/* Resource provisioning interface policy responsible for assigning files to workers */
class lowest_load : public worker_filetoworker
{
public:
    auto get_available_worker(pack::packet_pointer /*packet_ptr*/,
                              worker_set& current_workers) -> df::worker_ptr override
    {
        df::worker_ptr best = nullptr;

        int job = std::numeric_limits<int>::max();

        for (auto&& [worker_ptr, _notused] : current_workers)
            if (worker_ptr->is_valid() && worker_ptr->pending_jobs() < job)
            {
                best = worker_ptr;
                job  = worker_ptr->pending_jobs();
            }

        return best;
    }
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_FILETOWORKER_LOWEST_LOAD_HPP__
