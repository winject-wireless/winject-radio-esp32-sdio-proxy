// udp_ota wire: one message per UDP datagram, little-endian, no version
// field. Every reply is the request type | 0x80 and goes to the request's
// sender, so several clients can talk to one server; one OTA runs at a time.
//
//   ota_start_request      01 | u16 fragment_count [| u8 target]
//   ota_start_response     81 | u16 fragment_count | u8 status
//   ota_fragment_request   02 | u16 fragment | data
//   ota_fragment_response  82 | u16 fragment | u8 status
//   reboot_request         03
//   reboot_response        83 | u8 status
//   status_request         04
//   status_response        84 | u8 status | text
//   c6_reset_request       0a
//   c6_reset_response      8a | u8 status
//   c6_download_request    0b
//   c6_download_response   8b | u8 status
//
// target: 0 = this P4's app (also when the byte is left out), 1 = the C6
// radio. A C6 image is staged in the P4's inactive app slot; the fragment that
// completes it is answered once its header is checked, and the P4 then
// writes it to the C6 over the SDIO link (status shows c6=writing / ok /
// failed), resets the C6 and checks that the new image answers. reboot,
// c6_reset, c6_download and another ota_start get EINPROGRESS meanwhile.
// c6_reset reboots the C6 normally; c6_download holds it in its ROM
// downloader for a UART reflash (until c6_reset).
//
// Fragments are k_fragment_size bytes; only the last may be shorter (1 to
// k_fragment_size). They may arrive in any order and more than once.
//
// File access, one file at a time (owner = the client that opened it; others
// get EINPROGRESS), in the server's file directory (the P4's config partition):
//   ota_fs_open_request        05 | name[1..30]
//   ota_fs_open_response       85 | u8 status | u32 file_size
//   ota_fs_close_request       06
//   ota_fs_close_response      86 | u8 status
//   ota_fs_read_request        07 | u32 offset | u16 size
//   ota_fs_read_response       87 | u32 offset | u8 status | data[0..size]
//   ota_fs_write_request       08 | u32 offset | u16 size | data[size]
//   ota_fs_write_response      88 | u32 offset | u16 size | u8 status | u32 file_size
//   ota_fs_truncate_request    09 | u32 size
//   ota_fs_truncate_response   89 | u8 status | u32 file_size
//
// open creates a missing file empty; repeating it for the same name from the
// owner is answered OK (a lost reply). The name is [A-Za-z0-9._-], not
// starting with '.'. read and write take 1..k_fragment_size bytes at an
// offset up to file_size: write overwrites and appends, read returns fewer
// bytes at the end of the file. truncate only shrinks. close flushes; with
// no file open it is answered OK. A file whose owner sends nothing for the
// idle timeout, or whose write fails, is closed; reboot_request closes it
// (flushed) before the restart. Every request is idempotent, so a client can resend
// any of them until its reply arrives.
#pragma once

#include <cstddef>
#include <cstdint>

namespace udp_ota
{

constexpr size_t k_fragment_size = 1024;
constexpr uint8_t k_response = 0x80;

enum class msg : uint8_t
{
    ota_start = 0x01,
    ota_fragment = 0x02,
    reboot = 0x03,
    status = 0x04,
    fs_open = 0x05,
    fs_close = 0x06,
    fs_read = 0x07,
    fs_write = 0x08,
    fs_truncate = 0x09,
    c6_reset = 0x0a,
    c6_download = 0x0b,
};

enum class target : uint8_t
{
    p4 = 0,
    c6 = 1,
};

// Longest file name (SPIFFS: CONFIG_SPIFFS_OBJ_NAME_LEN 32 with the leading
// '/' and the terminating NUL).
constexpr size_t k_file_name_max = 30;

enum class status : uint8_t
{
    ok = 0,
    einprogress = 1,  // another client's OTA, or a C6 write, is running
    einval = 2,       // malformed request, fragment out of range or wrong size
    enoent = 3,       // fragment without a started OTA; no file open; no C6
    eio = 4,          // flash begin / erase / write failed
    ebadmsg = 5,      // image complete but rejected (verify, other project, or set boot)
    efbig = 6,        // fragment_count * k_fragment_size exceeds the slot
    enospc = 7,       // file system full
};

const char* status_name(status s);

inline uint16_t get_u16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline void put_u16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

inline uint32_t get_u32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline void put_u32(uint8_t* p, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
    {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

}  // namespace udp_ota
