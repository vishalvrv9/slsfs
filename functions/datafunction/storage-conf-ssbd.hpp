#pragma once
#ifndef STORAGE_CONF_SSBD_HPP__
#define STORAGE_CONF_SSBD_HPP__

#include <slsfs.hpp>

#include "storage-conf.hpp"

namespace slsfsdf
{

// Storage backend configuration for SSBD
class storage_conf_ssbd : public storage_conf
{
protected:
    boost::asio::io_context &io_context_;
public:
    storage_conf_ssbd(boost::asio::io_context &io): io_context_{io} {}

    void init(slsfs::base::json const& config) override
    {
        for (auto&& element : config["hosts"])
        {
            std::string const host = element["host"].get<std::string>();
            std::string const port = element["port"].get<std::string>();

            slsfs::log::logstring(fmt::format("adding {}:{}", host, port));

            hostlist_.push_back(std::make_shared<slsfs::storage::ssbd>(io_context_, host, port));
        }

        connect();
    }

    auto fullsize()   -> std::uint32_t & { static std::uint32_t s = 4 * 1024; return s; } // b$
    auto headersize() -> std::uint32_t   { return 4; } // byte
    auto blocksize()  -> std::uint32_t   { return fullsize() - headersize(); }

    auto perform(slsfs::jsre::request_parser<slsfs::base::byte> const& input) -> slsfs::base::buf override
    {
        slsfs::base::buf response;
        switch (input.operation())
        {
        case slsfs::jsre::operation_t::write:
        {
            auto const write_buf = input.data();

            std::uint32_t realpos  = input.position();
            std::uint32_t writesize = input.size(); // input["size"].get<std::size_t>();
            std::uint32_t endpos   = realpos + writesize;
            while (realpos < endpos)
            {
                std::uint32_t blockid = realpos / blocksize();
                std::uint32_t offset  = realpos % blocksize();
                std::uint32_t blockwritesize = std::min<std::uint32_t>(endpos - realpos, blocksize());

                if (offset + writesize > blocksize())
                    blockwritesize = blocksize() - offset;
                realpos += blockwritesize;

                slsfs::base::buf b(blockwritesize);
                std::copy_n(write_buf, blockwritesize, b.begin());

                for (std::shared_ptr<slsfs::storage::interface> host : hostlist_)
                {
                    //slsfs::base::buf resp = host->write_key(input.uuid(), blockid, offset, blockwritesize);

                    host->write_key(input.uuid(), blockid, b, offset, 0);
                    break;
                }
            }
            break;

            response = {'O', 'K'};
            break;
        }

        case slsfs::jsre::operation_t::create:
        {
            break;
        }

        case slsfs::jsre::operation_t::read:
        {
            std::uint32_t realpos  = input.position();
            std::uint32_t readsize = input.size(); // input["size"].get<std::size_t>();
            std::uint32_t endpos   = realpos + readsize;
            while (realpos < endpos)
            {
                std::uint32_t blockid = realpos / blocksize();
                std::uint32_t offset  = realpos % blocksize();
                slsfs::log::logstring("_data_ perform_single_request reading");

                std::uint32_t blockreadsize = std::min<std::uint32_t>(endpos - realpos, blocksize());
                if (offset + readsize > blocksize())
                    blockreadsize = blocksize() - offset;
                realpos += blockreadsize;

                slsfs::log::logstring(fmt::format("_data_ read perform_single_request sending: {}, {}, {}", blockid, offset, blockreadsize));
                for (std::shared_ptr<slsfs::storage::interface> host : hostlist_)
                {
                    slsfs::base::buf resp = host->read_key(input.uuid(), blockid, offset, blockreadsize);
                    response.insert(response.end(), resp.begin(), resp.end());
                    break;
                }
            }
            break;
        }
        }

        return response;
    }
};

} // namespace slsfsdf

#endif // STORAGE_CONF_SSBD_HPP__
