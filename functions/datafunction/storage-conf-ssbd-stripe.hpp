#pragma once
#ifndef STORAGE_CONF_SSBD_STRIPE_HPP__
#define STORAGE_CONF_SSBD_STRIPE_HPP__

#include "storage-conf.hpp"

#include <slsfs.hpp>

#include <boost/coroutine2/all.hpp>

#include <vector>

namespace slsfsdf
{

// Storage backend configuration for SSBD stripe
class storage_conf_ssbd_stripe : public storage_conf
{
    boost::asio::io_context &io_context_;
    static auto static_engine() -> std::mt19937&
    {
        static thread_local std::mt19937 mt;
        return mt;
    }

    auto select_replica(slsfs::pack::key_t const& uuid, int count) -> std::vector<int>
    {
        std::seed_seq seeds {uuid.begin(), uuid.end()};
        static_engine().seed(seeds);

        std::uniform_int_distribution<> dist(0, hostlist_.size());
        auto gen = [this, &dist] () { return dist(static_engine()); };

        std::vector<int> rv(count);
        std::generate(rv.begin(), rv.end(), gen);

        return rv;
    }

public:
    storage_conf_ssbd_stripe(boost::asio::io_context &io): io_context_{io} {}

    void init() override
    {
        hostlist_.push_back(std::make_shared<slsfs::storage::ssbd>(io_context_, "192.168.0.94",  "12000"));
        hostlist_.push_back(std::make_shared<slsfs::storage::ssbd>(io_context_, "192.168.0.165", "12000"));
        hostlist_.push_back(std::make_shared<slsfs::storage::ssbd>(io_context_, "192.168.0.242", "12000"));
        hostlist_.push_back(std::make_shared<slsfs::storage::ssbd>(io_context_, "192.168.0.183", "12000"));
        hostlist_.push_back(std::make_shared<slsfs::storage::ssbd>(io_context_, "192.168.0.86",  "12000"));
        hostlist_.push_back(std::make_shared<slsfs::storage::ssbd>(io_context_, "192.168.0.207", "12000"));
        hostlist_.push_back(std::make_shared<slsfs::storage::ssbd>(io_context_, "192.168.0.143", "12000"));
        hostlist_.push_back(std::make_shared<slsfs::storage::ssbd>(io_context_, "192.168.0.184", "12000"));
        hostlist_.push_back(std::make_shared<slsfs::storage::ssbd>(io_context_, "192.168.0.8",   "12000"));
    }

    constexpr std::size_t fullsize()   { return 4 * 1024; } // byte
    constexpr std::size_t headersize() { return 4; } // byte
    int blocksize() override { return fullsize() - headersize(); }
    constexpr int replication_size()   { return 3; }

    auto perform(slsfs::jsre::request_parser<slsfs::base::byte> const& input) -> slsfs::base::buf override
    {
        boost::system::error_code ec;

        slsfs::base::buf response;
        switch (input.operation())
        {
        case slsfs::jsre::operation_t::write:
        {
            slsfs::log::logstring("_data_ perform_single_request get data");
            auto const write_buf = input.data();
            slsfs::pack::key_t const uuid = input.uuid();

            int const realpos = input.position();
            int const blockid = realpos / blocksize();
            int const offset  = realpos % blocksize();

            std::vector<int> const selected_host_index = select_replica(uuid, replication_size());

            // 2PC first phase
            slsfs::log::logstring("_data_ perform_single_request check version");
            bool version_valid = false;
            std::uint32_t v = 0;
            while (not version_valid)
            {
                version_valid = true;
                std::uint32_t setv = std::max(v, version());

                for (int const index : selected_host_index)
                {
                    if (bool ok = hostlist_.at(index)->check_version_ok(input.uuid(), blockid, setv); not ok)
                        version_valid = false;
                    v = setv;
                }
                //** add failure and recovery here **
            }

            slsfs::log::logstring("_data_ perform_single_request agreed");
            // 2PC second phase
            slsfs::base::buf b;
            std::copy_n(write_buf, input.size(), std::back_inserter(b));
            for (int const index : selected_host_index)
                hostlist_.at(index)->write_key(uuid, blockid, b, offset, v);

            response = {'O', 'K'};
            break;
        }

        case slsfs::jsre::operation_t::create:
        {
            break;
        }

        case slsfs::jsre::operation_t::read:
        {
            int const realpos = input.position();
            int const blockid = realpos / blocksize();
            int const offset  = realpos % blocksize();
            slsfs::log::logstring("_data_ perform_single_request reading");

            std::uint32_t const size = input.size(); // input["size"].get<std::size_t>();

            slsfs::log::logstring(fmt::format("_data_ perform_single_request sending: {}, {}, {}, {}", blockid, offset, size, slsfs::pack::ntoh(size)));

            std::vector<int> const selected_host_index = select_replica(input.uuid(), replication_size());
            for (std::shared_ptr<slsfs::storage::interface> host : hostlist_)
                response = host->read_key(input.uuid(), blockid, offset, size);

            slsfs::log::logstring("_data_ perform_single_request read from ssbd");
            break;
        }
        }
        return response;
    }

    void start_perform(slsfs::jsre::request_parser<slsfs::base::byte> const& input, std::function<void(slsfs::base::buf)> next) override
    {
        switch (input.operation())
        {
        case slsfs::jsre::operation_t::write:
        {
            start_2pc_first_phase(input, std::move(next));
            break;
        }

        case slsfs::jsre::operation_t::create:
        {
            break;
        }

        case slsfs::jsre::operation_t::read:
        {
            auto uuid = input.uuid_shared();
            int const realpos = input.position();
            int const blockid = realpos / blocksize();
            int const offset  = realpos % blocksize();
            std::uint32_t const size = input.size();

            start_read_key(uuid, blockid, offset, size, input, std::move(next));
            break;
        }
        }
    }

    void start_2pc_first_phase (slsfs::jsre::request_parser<slsfs::base::byte> const& input, std::function<void(slsfs::base::buf)> next)
    {
        auto const uuid = input.uuid_shared();

        std::vector<int> const selected_host_index = select_replica(*uuid, replication_size());

        start_check_version_ok(0, selected_host_index,
                               uuid, 0,
                               input,
                               std::move(next));
    }

    void start_check_version_ok (int const write_index, std::vector<int> const selected_host_index,
                                 std::shared_ptr<slsfs::pack::key_t> const uuid, std::uint32_t version,
                                 slsfs::jsre::request_parser<slsfs::base::byte> const& input,
                                 std::function<void(slsfs::base::buf)> next)
    {
        slsfs::log::logstring(fmt::format("ssbd check version, starting with index {}", write_index));
        auto selected = hostlist_.at(selected_host_index.at(write_index));

        int const blockid = input.position() / blocksize();

        selected->start_check_version_ok(
            uuid, blockid, version,
            [=, this, next=std::move(next)] (bool ok) {
                if (not ok)
                {
                    slsfs::log::logstring("ssbd check version failed");
                    std::invoke(next, std::vector<unsigned char>{'F', 'A', 'I', 'L'});
                }
                else
                {
                    int next_index = write_index + 1;
                    if (next_index < replication_size())
                        start_check_version_ok(
                            next_index, selected_host_index,
                            uuid, version,
                            input,
                            std::move(next));
                    else
                        start_2pc_second_phase(
                            uuid, version,
                            input,
                            std::move(next));
                }
            });
    }

    void start_2pc_second_phase(std::shared_ptr<slsfs::pack::key_t> const uuid, std::uint32_t version,
                                slsfs::jsre::request_parser<slsfs::base::byte> const& input,
                                std::function<void(slsfs::base::buf)> next)
    {
        auto buffer = std::make_shared<slsfs::base::buf>();
        std::copy_n(input.data(), input.size(), std::back_inserter(*buffer));

        int const realpos = input.position();
        int const blockid = realpos / blocksize();
        int const offset  = realpos % blocksize();

        std::vector<int> const selected_host_index = select_replica(*uuid, replication_size());

        start_write_key(0, selected_host_index,
                        uuid,
                        blockid, offset, version,
                        buffer,
                        input,
                        std::move(next));
    }

    void start_write_key(int const write_index, std::vector<int> const selected_host_index,
                         std::shared_ptr<slsfs::pack::key_t> const uuid,
                         int const blockid, int const offset, std::uint32_t version,
                         std::shared_ptr<slsfs::base::buf> buffer,
                         slsfs::jsre::request_parser<slsfs::base::byte> const& input,
                         std::function<void(slsfs::base::buf)> next)
    {
        slsfs::log::logstring(fmt::format("ssbd start_write_key, starting with index {}", write_index));
        auto selected = hostlist_.at(selected_host_index.at(write_index));
        selected->start_write_key(
            uuid, blockid,
            buffer, offset,
            version,
            [=, this, next=std::move(next)] (slsfs::base::buf b) {
                int next_index = write_index + 1;
                if (next_index < replication_size())
                    start_write_key(
                        next_index, selected_host_index,
                        uuid,
                        blockid, offset, version,
                        buffer,
                        input,
                        next);
                else
                    std::invoke(next, b);
            });
    }

    void start_read_key(std::shared_ptr<slsfs::pack::key_t> const uuid,
                        int const blockid, int const offset, std::size_t size,
                        slsfs::jsre::request_parser<slsfs::base::byte> const input,
                        std::function<void(slsfs::base::buf)> next)
    {
        slsfs::log::logstring("ssbd start_read_key");
        auto selected = hostlist_.at(select_replica(*uuid, 1).front());
        selected->start_read_key(
            uuid, blockid,
            offset, size,
            [input, next=std::move(next)] (slsfs::base::buf b) {
                std::invoke(next, b);
            });
    }
};

} // namespace slsfsdf

#endif // STORAGE_CONF_SSBD_STRIPE_HPP__
