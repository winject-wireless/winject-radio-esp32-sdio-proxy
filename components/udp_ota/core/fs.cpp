#include "fs.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace udp_ota
{
namespace
{

status from_errno(int e)
{
    return e == ENOSPC ? status::enospc : status::eio;
}

}  // namespace

bool valid_file_name(const char* name, size_t len, size_t max_len)
{
    if (len == 0 || len > max_len || name[0] == '.')
    {
        return false;
    }
    for (size_t i = 0; i < len; ++i)
    {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

posix_file_store::~posix_file_store()
{
    if (m_fd >= 0)
    {
        ::close(m_fd);
    }
}

status posix_file_store::open(const std::string& name, uint32_t* size)
{
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    const std::string path = m_dir + "/" + name;
    m_fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (m_fd < 0)
    {
        return from_errno(errno);
    }
    const off_t end = ::lseek(m_fd, 0, SEEK_END);
    if (end < 0)
    {
        const int e = errno;
        ::close(m_fd);
        m_fd = -1;
        return from_errno(e);
    }
    *size = static_cast<uint32_t>(end);
    return status::ok;
}

status posix_file_store::read(uint32_t offset, uint8_t* out, size_t len, size_t* n)
{
    *n = 0;
    if (m_fd < 0)
    {
        return status::enoent;
    }
    if (::lseek(m_fd, static_cast<off_t>(offset), SEEK_SET) < 0)
    {
        return from_errno(errno);
    }
    while (*n < len)
    {
        const ssize_t r = ::read(m_fd, out + *n, len - *n);
        if (r < 0)
        {
            return from_errno(errno);
        }
        if (r == 0)
        {
            break;  // end of file
        }
        *n += static_cast<size_t>(r);
    }
    return status::ok;
}

status posix_file_store::write(uint32_t offset, const uint8_t* data, size_t len)
{
    if (m_fd < 0)
    {
        return status::enoent;
    }
    if (::lseek(m_fd, static_cast<off_t>(offset), SEEK_SET) < 0)
    {
        return from_errno(errno);
    }
    size_t done = 0;
    while (done < len)
    {
        const ssize_t w = ::write(m_fd, data + done, len - done);
        if (w <= 0)
        {
            return w < 0 ? from_errno(errno) : status::enospc;
        }
        done += static_cast<size_t>(w);
    }
    return status::ok;
}

status posix_file_store::truncate(uint32_t size)
{
    if (m_fd < 0)
    {
        return status::enoent;
    }
    return ::ftruncate(m_fd, static_cast<off_t>(size)) == 0 ? status::ok : from_errno(errno);
}

status posix_file_store::close()
{
    if (m_fd < 0)
    {
        return status::ok;
    }
    const bool synced = ::fsync(m_fd) == 0;
    const int e = errno;
    const bool closed = ::close(m_fd) == 0;
    m_fd = -1;
    return synced && closed ? status::ok : from_errno(e);
}

}  // namespace udp_ota
