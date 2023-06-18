#pragma once
#ifndef CLIENTLIB_DIRECT_CLIENT_HPP__

#include "clientlib.hpp"

namespace slsfs::client
{

class direct_client
{
    boost::asio::io_context& io_context_;
    client client_;

public:
    direct_client(boost::asio::io_context& io, std::string const& zkhost):
        io_context_{io}, client_{io, zkhost} {}
};

} // namespace slsfs::client

#endif // CLIENTLIB_DIRECT_CLIENT_HPP__
