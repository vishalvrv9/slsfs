#pragma once
#ifndef WORKER_CONFIG_HPP__
#define WORKER_CONFIG_HPP__

#include "basic.hpp"
#include "serializer.hpp"
#include "worker.hpp"
#include "uuid.hpp"

#include <type_traits>
#include <concepts>

namespace slsfs::launcher
{

/* This class serves as a configuration interface for worker */
    // should keep this?
template<typename T>
class worker_config
{
private:
    std::string json_config_;
public:
    worker_config(std::string const& jsonconfig): json_config_{jsonconfig} {}

    virtual
    auto get_worker_config() -> std::string& {
        return json_config_;
    }

    auto get() -> T {}
};

} // namespace slsfs::launcher

#endif // WORKER_CONFIG_HPP__
