// udp_ota file access: one file at a time in one flat directory (on the P4,
// the SPIFFS config partition at /winject). Wire: wire.hpp, ota_fs_*.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "wire.hpp"

namespace udp_ota
{

// The open file. Calls come from the server's one thread.
struct file_store
{
    virtual ~file_store() = default;
    // Opens name (validated by the server), creating it empty if missing.
    virtual status open(const std::string& name, uint32_t* size) = 0;
    // Up to len bytes at offset (offset <= size); *n = bytes read.
    virtual status read(uint32_t offset, uint8_t* out, size_t len, size_t* n) = 0;
    // Overwrites and/or appends at offset (offset <= size).
    virtual status write(uint32_t offset, const uint8_t* data, size_t len) = 0;
    // Shrinks the file to size (size <= current size).
    virtual status truncate(uint32_t size) = 0;
    // Flushes and closes; the store has no open file afterwards.
    virtual status close() = 0;
};

// POSIX file store: files are <dir>/<name>. Works on the host and on ESP-IDF
// (VFS, e.g. SPIFFS: shrinking ftruncate, overwrite in place).
class posix_file_store final : public file_store
{
public:
    explicit posix_file_store(std::string dir) : m_dir(std::move(dir)) {}
    ~posix_file_store() override;
    status open(const std::string& name, uint32_t* size) override;
    status read(uint32_t offset, uint8_t* out, size_t len, size_t* n) override;
    status write(uint32_t offset, const uint8_t* data, size_t len) override;
    status truncate(uint32_t size) override;
    status close() override;

private:
    std::string m_dir;
    int m_fd = -1;
};

// A name the server accepts: 1..max_len of [A-Za-z0-9._-], not starting with
// '.' (no paths, no hidden files).
bool valid_file_name(const char* name, size_t len, size_t max_len);

}  // namespace udp_ota
