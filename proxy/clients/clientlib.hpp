#pragma once

#ifndef CLIENT_CLIENTLIB_HPP__
#define CLIENT_CLIENTLIB_HPP__

#include "../uuid.hpp"

namespace slsfs::client
{

auto mkdir(pack::key_t const& directory)
    -> pack::packet_pointer
{
    pack::packet_pointer cptr = std::make_shared<slsfs::pack::packet>();

    cptr->header.type = pack::msg_t::trigger;
    cptr->header.key  = directory;

    jsre::request r;
    r.type = jsre::type_t::metadata;
    r.operation = static_cast<jsre::operation_t>(jsre::meta_operation_t::mkdir);
    r.size = 0;
    r.to_network_format();

    cptr->data.buf.resize(sizeof (r));
    std::memcpy(cptr->data.buf.data(), &r, sizeof (r));

    cptr->header.gen();
    BOOST_LOG_TRIVIAL(debug) << "creating mkdir meta request " << cptr->header;

    return cptr;
}

auto mkdir(std::string const directory)
    -> pack::packet_pointer {
    return mkdir(uuid::get_uuid(directory));
}

auto addfile(pack::key_t const& directory, std::string const filename)
    -> pack::packet_pointer
{
    pack::packet_pointer cptr = std::make_shared<pack::packet>();

    jsre::meta::filemeta filemeta {
        .owner = 0,
        .permission = 040777,
        .filename = {}
    };

    std::memcpy(filemeta.filename.data(),
                filename.data(), std::min(filename.size(), sizeof(filemeta)));

    filemeta.to_network_format();

    cptr->header.type = pack::msg_t::trigger;
    cptr->header.key = directory;

    jsre::request r;
    r.type = jsre::type_t::metadata;
    r.operation = static_cast<jsre::operation_t>(jsre::meta_operation_t::addfile);
    r.size = sizeof(filemeta);
    r.position = 0;
    r.to_network_format();

    cptr->data.buf.resize(sizeof (r) + sizeof(filemeta));
    std::memcpy(cptr->data.buf.data(), &r, sizeof (r));
    std::memcpy(cptr->data.buf.data() + sizeof (r), &filemeta, sizeof(filemeta));

    cptr->header.gen();
    BOOST_LOG_TRIVIAL(debug) << "creating addfile meta request " << cptr->header;

    return cptr;
}

auto addfile(std::string const directory, std::string const filename)
    -> pack::packet_pointer {
    return addfile(uuid::get_uuid(directory), filename);
}

auto ls (pack::key_t const& directory)
    -> pack::packet_pointer
{
    pack::packet_pointer cptr = std::make_shared<pack::packet>();

    cptr->header.type = pack::msg_t::trigger;
    cptr->header.key = directory;

    jsre::request r;
    r.type = jsre::type_t::metadata;
    r.operation = static_cast<jsre::operation_t>(jsre::meta_operation_t::ls);
    r.size = 0;
    r.to_network_format();

    cptr->data.buf.resize(sizeof (r));
    std::memcpy(cptr->data.buf.data(), &r, sizeof (r));

    cptr->header.gen();
    BOOST_LOG_TRIVIAL(debug) << "creating ls meta request " << cptr->header;
    return cptr;
}

auto ls (std::string const directory)
    -> pack::packet_pointer {
    return ls (uuid::get_uuid(directory));
}

template<typename BufContainer>
auto write (pack::key_t const& filename, BufContainer const& buf)
    -> pack::packet_pointer
{
    pack::packet_pointer ptr = std::make_shared<pack::packet>();

    ptr->header.type = pack::msg_t::trigger;
    ptr->header.key = filename;

    jsre::request r;
    r.type = jsre::type_t::file;
    r.operation = jsre::operation_t::write;
    r.position = 0;
    r.size = buf.size();
    r.to_network_format();

    ptr->data.buf.resize(sizeof (r) + buf.size());
    std::memcpy(ptr->data.buf.data(), &r, sizeof (r));
    std::memcpy(ptr->data.buf.data() + sizeof (r), buf.data(), buf.size());

    ptr->header.gen();
    BOOST_LOG_TRIVIAL(debug) << "creating file write request " << ptr->header;
    return ptr;
}

template<typename BufContainer>
auto write (std::string const filename, BufContainer const& buf)
    -> pack::packet_pointer {
    return write(uuid::get_uuid(filename), buf);
}

auto read (pack::key_t const& filename, std::uint32_t const size)
    -> pack::packet_pointer
{
    pack::packet_pointer ptr = std::make_shared<pack::packet>();

    ptr->header.type = pack::msg_t::trigger;
    ptr->header.key = filename;

    jsre::request r;
    r.type = jsre::type_t::file;
    r.operation = jsre::operation_t::read;
    r.position = 0;
    r.size = size;
    r.to_network_format();

    ptr->data.buf.resize(sizeof (r));
    std::memcpy(ptr->data.buf.data(), &r, sizeof (r));

    ptr->header.gen();
    BOOST_LOG_TRIVIAL(debug) << "creating file read request " << ptr->header;
    return ptr;
}

auto read (std::string const filename, std::uint32_t const size)
    -> pack::packet_pointer {
    return read(uuid::get_uuid(filename), size);
}

} // namespace client

#endif // CLIENT_CLIENTLIB_HPP__
