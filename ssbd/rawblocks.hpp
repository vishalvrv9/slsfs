#pragma once

#ifndef RAWBLOCKS_HPP__
#define RAWBLOCKS_HPP__

#include "basic.hpp"

#include <leveldb/db.h>

namespace leveldb_pack
{

class rawblocks
{
    // [4bytes=version(big-endian), rest is data]
    std::string buf_;

public:
    using versionint_t = std::uint32_t;
    auto fullsize()   -> std::size_t& { static std::size_t s = 4 * 1024; return s; } // byte
    auto headersize() -> std::size_t  { return sizeof(versionint_t); } // byte
    auto blocksize()  -> std::size_t  { return fullsize() - headersize(); }

    auto version() -> versionint_t
    {
        versionint_t i;
        std::memcpy(&i, buf_.data(), sizeof(i));
        return ntoh(i);
    }

    void update_version(versionint_t i)
    {
        i = hton(i);
        std::memcpy(buf_.data(), &i, sizeof(i));
    }

    template<typename InputIterator>
    void write(std::size_t logic_position, InputIterator it, std::size_t size) {
        std::copy_n(it, size, std::next(buf_.begin(), logic_position + headersize()));
    }

    template<typename OutputIterator>
    void read(std::size_t logic_position, OutputIterator ot, std::size_t size) {
        std::copy_n(std::next(buf_.begin(), logic_position + headersize()),
                    std::min(buf_.size() - headersize() - logic_position, size),
                    ot);
    }

    void move(std::string && newbuf) {
        buf_ = std::move(newbuf);
    }

    auto bind(leveldb::DB& db, std::string const& key) -> leveldb::Status {
        return db.Get(leveldb::ReadOptions(), key, &buf_);
    }

    auto flush(leveldb::DB& db, std::string const& key) -> leveldb::Status {
        return db.Put(leveldb::WriteOptions(), key, buf_);
    }
};

}; // namespace leveldb_pack

#endif // RAWBLOCKS_HPP__
