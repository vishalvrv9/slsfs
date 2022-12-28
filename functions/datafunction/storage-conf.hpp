#pragma once
#ifndef STORAGE_CONF_HPP__
#define STORAGE_CONF_HPP__

#include <slsfs.hpp>
#include <memory>
#include <vector>

namespace slsfsdf
{

// Used to configurate the backend storage configuration.
class storage_conf
{
protected:
    std::vector<std::shared_ptr<slsfs::storage::interface>> hostlist_;
    void connect()
    {
        for (std::shared_ptr<slsfs::storage::interface>& host : hostlist_)
            host->connect();
    }

public:
    virtual ~storage_conf() {}
    virtual void init(slsfs::base::json const& config) { connect(); }
    virtual auto blocksize() -> std::uint32_t = 0;
    virtual bool use_async() { return false; }
    virtual auto perform(slsfs::jsre::request_parser<slsfs::base::byte> const& input) -> slsfs::base::buf = 0;
    virtual void start_perform(slsfs::jsre::request_parser<slsfs::base::byte> const& input, std::function<void(slsfs::base::buf)> next) {
        assert(false);// "to use start perform, please override this function");
    };
};

} // namespace slsfsdf

#endif // STORAGE_CONF_HPP__
