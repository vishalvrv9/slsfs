#pragma once
#ifndef POLICY_WORKER_FILETOWORKER_HPP__
#define POLICY_WORKER_FILETOWORKER_HPP__

#include "../launcher-base-types.hpp"

namespace slsfs::launcher::policy
{

/* Resource provisioning interface policy responsible for assigning files to workers */
class worker_filetoworker : public info
{
public:
    virtual
    auto get_available_worker(pack::packet_pointer packet_ptr,
                              worker_set& current_workers) -> df::worker_ptr = 0;
};

} // namespace slsfs::launcher::policy

#endif // POLICY_WORKER_FILETOWORKER_HPP__
