// Host tests for the udp_ota core (no ESP-IDF):
//   test/run.sh
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

#include "fs.hpp"
#include "server.hpp"

using namespace udp_ota;

namespace
{

int g_failed = 0;

#define CHECK(cond)                                                     \
    do                                                                  \
    {                                                                   \
        if (!(cond))                                                    \
        {                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                 \
        }                                                               \
    } while (0)

struct fake_flash : backend
{
    size_t cap = 8 * k_fragment_size;
    std::vector<uint8_t> img;
    int begins = 0, writes = 0, aborts = 0;
    bool fail_write = false;
    status finish_status = status::ok;
    uint32_t finished_size = 0;

    size_t capacity() override { return cap; }
    bool begin() override
    {
        ++begins;
        img.assign(cap, 0xff);
        return true;
    }
    bool write(uint32_t off, const uint8_t* d, size_t n) override
    {
        ++writes;
        if (fail_write)
        {
            return false;
        }
        std::memcpy(img.data() + off, d, n);
        return true;
    }
    status finish(uint32_t size) override
    {
        finished_size = size;
        return finish_status;
    }
    void abort() override { ++aborts; }
    std::string describe() override { return "running=ota_0"; }
};

const peer k_a {0x0100007f, 2430};
const peer k_b {0x0200000a, 5555};

struct reply
{
    size_t n = 0;
    uint8_t b[server::k_reply_max];
    uint8_t type() const { return b[0]; }
    uint16_t id() const { return get_u16(b + 1); }
    status st() const { return static_cast<status>(b[3]); }
};

reply start(server& s, uint16_t count, const peer& p, uint32_t t = 0)
{
    uint8_t m[3] = {0x01};
    put_u16(m + 1, count);
    reply r;
    r.n = s.handle(m, sizeof(m), p, t, r.b);
    return r;
}

reply frag(server& s, uint16_t i, size_t len, uint8_t fill, const peer& p, uint32_t t = 0)
{
    std::vector<uint8_t> m(3 + len, fill);
    m[0] = 0x02;
    put_u16(m.data() + 1, i);
    reply r;
    r.n = s.handle(m.data(), m.size(), p, t, r.b);
    return r;
}

reply simple(server& s, uint8_t type, const peer& p)
{
    reply r;
    r.n = s.handle(&type, 1, p, 0, r.b);
    return r;
}

void test_happy_path_out_of_order_and_dups()
{
    fake_flash f;
    int active_on = 0, active_off = 0;
    server s(f, 30000, [&](bool on) { (on ? active_on : active_off)++; });
    reply r = start(s, 3, k_a);
    CHECK(r.n == 4 && r.type() == 0x81 && r.id() == 3 && r.st() == status::ok);
    CHECK(active_on == 1);
    CHECK(start(s, 3, k_a).st() == status::ok);  // retransmitted start
    CHECK(f.begins == 1);
    CHECK(frag(s, 2, 100, 0xcc, k_a).st() == status::ok);
    CHECK(frag(s, 0, k_fragment_size, 0xaa, k_a).st() == status::ok);
    CHECK(frag(s, 0, k_fragment_size, 0xaa, k_a).st() == status::ok);  // dup
    CHECK(f.writes == 2);
    CHECK(s.current() == server::state::active);
    r = frag(s, 1, k_fragment_size, 0xbb, k_a);
    CHECK(r.type() == 0x82 && r.id() == 1 && r.st() == status::ok);
    CHECK(s.current() == server::state::done);
    CHECK(f.finished_size == 2 * k_fragment_size + 100);
    CHECK(f.img[0] == 0xaa && f.img[k_fragment_size] == 0xbb && f.img[2 * k_fragment_size + 99] == 0xcc);
    CHECK(active_off == 1);
    // A late retransmit after completion is still OK and writes nothing.
    CHECK(frag(s, 2, 100, 0xcc, k_a).st() == status::ok);
    CHECK(f.writes == 3);
}

void test_multi_client()
{
    fake_flash f;
    server s(f, 30000);
    CHECK(start(s, 2, k_a).st() == status::ok);
    CHECK(start(s, 2, k_b).st() == status::einprogress);
    CHECK(start(s, 3, k_a).st() == status::einprogress);  // owner, other image
    CHECK(frag(s, 0, k_fragment_size, 1, k_b).st() == status::einprogress);
    reply st = simple(s, 0x04, k_b);  // status from anyone
    CHECK(st.n > 2 && st.b[0] == 0x84 && st.b[1] == 0);
    const std::string text(reinterpret_cast<const char*>(st.b + 2), st.n - 2);
    CHECK(text == "running=ota_0 ota=active fragments=0/2 target=p4");
}

void test_bounds()
{
    fake_flash f;
    server s(f, 30000);
    CHECK(frag(s, 0, 10, 1, k_a).st() == status::enoent);
    CHECK(start(s, 0, k_a).st() == status::einval);
    CHECK(start(s, 9, k_a).st() == status::efbig);  // cap is 8 fragments
    CHECK(f.begins == 0);
    CHECK(start(s, 2, k_a).st() == status::ok);
    CHECK(frag(s, 2, 10, 1, k_a).st() == status::einval);                     // out of range
    CHECK(frag(s, 0, 10, 1, k_a).st() == status::einval);                     // short, not last
    CHECK(frag(s, 1, k_fragment_size + 1, 1, k_a).st() == status::einval);    // too long
    CHECK(frag(s, 1, 0, 1, k_a).st() == status::einval);                      // empty
    uint8_t bad[2] = {0x01, 0};
    reply r;
    r.n = s.handle(bad, sizeof(bad), k_a, 0, r.b);
    CHECK(r.n == 4 && r.st() == status::einval);
    uint8_t resp = 0x81;  // a response is never answered
    CHECK(s.handle(&resp, 1, k_a, 0, r.b) == 0);
    uint8_t unknown = 0x7e;
    r.n = s.handle(&unknown, 1, k_a, 0, r.b);
    CHECK(r.n == 2 && r.b[0] == 0xfe && r.b[1] == static_cast<uint8_t>(status::einval));
}

void test_timeout_and_failures()
{
    fake_flash f;
    int off = 0;
    server s(f, 1000, [&](bool on) { off += !on; });
    CHECK(start(s, 2, k_a, 0).st() == status::ok);
    CHECK(frag(s, 0, k_fragment_size, 1, k_a, 900).st() == status::ok);
    s.tick(1800);
    CHECK(s.current() == server::state::active);  // refreshed at 900
    s.tick(1900);
    CHECK(s.current() == server::state::idle && f.aborts == 1 && off == 1);
    CHECK(start(s, 2, k_b, 2000).st() == status::ok);  // free for another client

    f.fail_write = true;
    CHECK(frag(s, 0, k_fragment_size, 1, k_b, 2000).st() == status::eio);
    CHECK(s.current() == server::state::idle && off == 2);

    f.fail_write = false;
    f.finish_status = status::ebadmsg;
    CHECK(start(s, 1, k_a, 3000).st() == status::ok);
    CHECK(frag(s, 0, 5, 1, k_a, 3000).st() == status::ebadmsg);
    CHECK(s.current() == server::state::idle);
}

void test_reboot()
{
    fake_flash f;
    server s(f, 30000);
    CHECK(start(s, 2, k_a).st() == status::ok);
    reply r = simple(s, 0x03, k_b);
    CHECK(r.n == 2 && r.b[0] == 0x83 && r.b[1] == 0);
    CHECK(s.reboot_requested());
    CHECK(s.current() == server::state::idle && f.aborts == 1);
}

void test_restart_after_done()
{
    fake_flash f;
    server s(f, 30000);
    CHECK(start(s, 1, k_a).st() == status::ok);
    CHECK(frag(s, 0, 7, 1, k_a).st() == status::ok);
    CHECK(s.current() == server::state::done);
    CHECK(start(s, 2, k_b).st() == status::ok);  // a new image replaces the finished one
    CHECK(f.begins == 2 && s.current() == server::state::active);
}

// ota_fs_* request builders and reply fields (wire.hpp).
std::vector<uint8_t> fs_open_req(const std::string& name)
{
    std::vector<uint8_t> m {0x05};
    m.insert(m.end(), name.begin(), name.end());
    return m;
}

std::vector<uint8_t> fs_read_req(uint32_t off, uint16_t size)
{
    std::vector<uint8_t> m(7);
    m[0] = 0x07;
    put_u32(m.data() + 1, off);
    put_u16(m.data() + 5, size);
    return m;
}

std::vector<uint8_t> fs_write_req(uint32_t off, const std::string& data)
{
    std::vector<uint8_t> m(7);
    m[0] = 0x08;
    put_u32(m.data() + 1, off);
    put_u16(m.data() + 5, static_cast<uint16_t>(data.size()));
    m.insert(m.end(), data.begin(), data.end());
    return m;
}

std::vector<uint8_t> fs_trunc_req(uint32_t size)
{
    std::vector<uint8_t> m(5);
    m[0] = 0x09;
    put_u32(m.data() + 1, size);
    return m;
}

reply send(server& s, const std::vector<uint8_t>& m, const peer& p, uint32_t t = 0)
{
    reply r;
    r.n = s.handle(m.data(), m.size(), p, t, r.b);
    return r;
}

// Status byte of each fs reply type.
status fs_st(const reply& r)
{
    switch (r.b[0])
    {
    case 0x87: return static_cast<status>(r.b[5]);
    case 0x88: return static_cast<status>(r.b[7]);
    default: return static_cast<status>(r.b[1]);
    }
}

std::string slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

std::string make_dir()
{
    char tmpl[] = "/tmp/udp_ota_fs_XXXXXX";
    return mkdtemp(tmpl);
}

void test_fs_write_overwrite_append_truncate()
{
    const std::string dir = make_dir();
    { std::ofstream(dir + "/winject.conf") << "radio.channel = 1\n"; }
    fake_flash f;
    posix_file_store files(dir);
    server s(f, 30000, {}, &files);

    reply r = send(s, fs_open_req("winject.conf"), k_a);
    CHECK(r.n == 6 && r.b[0] == 0x85 && fs_st(r) == status::ok && get_u32(r.b + 2) == 18);
    CHECK(fs_st(send(s, fs_open_req("winject.conf"), k_a)) == status::ok);  // repeated open

    // Overwrite in place, then append past the end.
    r = send(s, fs_write_req(16, "13\n"), k_a);
    CHECK(r.n == 12 && r.b[0] == 0x88 && get_u32(r.b + 1) == 16 && get_u16(r.b + 5) == 3 && fs_st(r) == status::ok);
    CHECK(get_u32(r.b + 8) == 19);
    r = send(s, fs_write_req(19, "x = 2\n"), k_a);
    CHECK(fs_st(r) == status::ok && get_u32(r.b + 8) == 25);
    CHECK(fs_st(send(s, fs_write_req(19, "x = 2\n"), k_a)) == status::ok);  // resend: same bytes
    CHECK(fs_st(send(s, fs_write_req(26, "gap"), k_a)) == status::einval);    // past the end

    r = send(s, fs_read_req(0, 1024), k_a);
    CHECK(r.b[0] == 0x87 && fs_st(r) == status::ok && get_u32(r.b + 1) == 0);
    CHECK(std::string(reinterpret_cast<char*>(r.b + 6), r.n - 6) == "radio.channel = 13\nx = 2\n");
    r = send(s, fs_read_req(25, 10), k_a);
    CHECK(fs_st(r) == status::ok && r.n == 6);                                 // at the end: empty
    CHECK(fs_st(send(s, fs_read_req(26, 10), k_a)) == status::einval);        // past the end
    CHECK(fs_st(send(s, fs_read_req(0, 1025), k_a)) == status::einval);       // too big

    r = send(s, fs_trunc_req(19), k_a);
    CHECK(r.b[0] == 0x89 && fs_st(r) == status::ok && get_u32(r.b + 2) == 19);
    CHECK(fs_st(send(s, fs_trunc_req(20), k_a)) == status::einval);           // only shrinks
    r = send(s, {0x06}, k_a);
    CHECK(r.n == 2 && r.b[0] == 0x86 && fs_st(r) == status::ok);
    CHECK(fs_st(send(s, {0x06}, k_a)) == status::ok);                          // repeated close
    CHECK(slurp(dir + "/winject.conf") == "radio.channel = 13\n");

    // No file open: read, write and truncate get ENOENT.
    CHECK(fs_st(send(s, fs_read_req(0, 1), k_a)) == status::enoent);
    CHECK(fs_st(send(s, fs_write_req(0, "a"), k_a)) == status::enoent);
    CHECK(fs_st(send(s, fs_trunc_req(0), k_a)) == status::enoent);
}

void test_fs_one_file_owner_names_timeout()
{
    const std::string dir = make_dir();
    fake_flash f;
    posix_file_store files(dir);
    server s(f, 1000, {}, &files);

    reply r = send(s, fs_open_req("new.txt"), k_a, 0);
    CHECK(fs_st(r) == status::ok && get_u32(r.b + 2) == 0);                    // created empty
    CHECK(fs_st(send(s, fs_open_req("new.txt"), k_b, 0)) == status::einprogress);
    CHECK(fs_st(send(s, fs_open_req("other.txt"), k_a, 0)) == status::einprogress);
    CHECK(fs_st(send(s, fs_write_req(0, "b"), k_b, 0)) == status::einprogress);
    CHECK(fs_st(send(s, fs_read_req(0, 1), k_b, 0)) == status::einprogress);
    CHECK(fs_st(send(s, {0x06}, k_b, 0)) == status::einprogress);
    CHECK(fs_st(send(s, fs_write_req(0, "a"), k_a, 500)) == status::ok);

    // The OTA side is independent of the open file.
    CHECK(start(s, 2, k_b, 500).st() == status::ok);

    s.tick(1400);  // refreshed at 500
    CHECK(fs_st(send(s, fs_open_req("other.txt"), k_b, 1400)) == status::einprogress);
    s.tick(1600);  // idle: closed
    CHECK(fs_st(send(s, fs_open_req("other.txt"), k_b, 1600)) == status::ok);
    CHECK(slurp(dir + "/new.txt") == "a");

    for (const char* bad : {"", ".hidden", "../x", "a/b", "sp ace", "1234567890123456789012345678901"})
    {
        CHECK(fs_st(send(s, fs_open_req(bad), k_a, 1600)) == status::einval);
    }
    CHECK(valid_file_name("123456789012345678901234567890", 30, k_file_name_max));

    // Without a file store every fs request gets ENOENT.
    server none(f, 1000);
    CHECK(fs_st(send(none, fs_open_req("winject.conf"), k_a)) == status::enoent);
}

struct fake_store : file_store
{
    uint32_t size = 10;
    int closes = 0;
    bool fail_write = false;
    status open(const std::string&, uint32_t* sz) override { *sz = size; return status::ok; }
    status read(uint32_t, uint8_t*, size_t, size_t* n) override { *n = 0; return status::ok; }
    status write(uint32_t, const uint8_t*, size_t) override { return fail_write ? status::enospc : status::ok; }
    status truncate(uint32_t) override { return status::ok; }
    status close() override { ++closes; return status::ok; }
};

void test_fs_reboot_and_write_failure_close()
{
    fake_flash f;
    fake_store st;
    server s(f, 30000, {}, &st);
    CHECK(fs_st(send(s, fs_open_req("winject.conf"), k_a)) == status::ok);
    CHECK(simple(s, 0x03, k_b).b[1] == 0);  // reboot from anyone flushes the file
    CHECK(st.closes == 1 && s.reboot_requested());
    CHECK(fs_st(send(s, fs_read_req(0, 1), k_a)) == status::enoent);

    server s2(f, 30000, {}, &st);
    CHECK(fs_st(send(s2, fs_open_req("winject.conf"), k_a)) == status::ok);
    st.fail_write = true;
    CHECK(fs_st(send(s2, fs_write_req(0, "abc"), k_a)) == status::enospc);
    CHECK(st.closes == 2);                                                  // closed on failure
    CHECK(fs_st(send(s2, fs_write_req(0, "abc"), k_a)) == status::enoent);  // must reopen
    CHECK(fs_st(send(s2, fs_open_req("winject.conf"), k_b)) == status::ok); // free again
}

struct fake_c6 : fake_flash
{
    bool writing = false;
    bool busy() override { return writing; }
    status finish(uint32_t size) override
    {
        finished_size = size;
        writing = finish_status == status::ok;  // the SDIO write starts
        return finish_status;
    }
    std::string describe() override { return writing ? "c6=writing" : "c6=idle"; }
};

struct fake_radio : radio_control
{
    int resets = 0, downloads = 0;
    status reset() override { ++resets; return status::ok; }
    status download() override { ++downloads; return status::ok; }
};

reply start_target(server& s, uint16_t count, uint8_t tgt, const peer& p, uint32_t t = 0)
{
    uint8_t m[4] = {0x01, 0, 0, tgt};
    put_u16(m + 1, count);
    reply r;
    r.n = s.handle(m, sizeof(m), p, t, r.b);
    return r;
}

void test_c6_target()
{
    fake_flash p4;
    fake_c6 c6;
    fake_radio radio;
    server s(p4, 30000, {}, nullptr, &c6, &radio);

    CHECK(start_target(s, 2, 2, k_a).st() == status::einval);  // unknown target
    CHECK(start_target(s, 2, 1, k_a).st() == status::ok);
    CHECK(c6.begins == 1 && p4.begins == 0);
    CHECK(start_target(s, 2, 1, k_a).st() == status::ok);        // repeated start
    CHECK(start(s, 2, k_a).st() == status::einprogress);          // same count, other target
    CHECK(simple(s, 0x0a, k_b).b[1] == static_cast<uint8_t>(status::einprogress));  // no reset mid-image
    CHECK(frag(s, 0, k_fragment_size, 0x11, k_a).st() == status::ok);
    CHECK(frag(s, 1, 50, 0x22, k_a).st() == status::ok);          // completes: staged, C6 write starts
    CHECK(c6.finished_size == k_fragment_size + 50 && c6.img[0] == 0x11 && p4.writes == 0);
    CHECK(frag(s, 1, 50, 0x22, k_a).st() == status::ok);          // late retransmit still OK

    reply st = simple(s, 0x04, k_b);
    const std::string text(reinterpret_cast<const char*>(st.b + 2), st.n - 2);
    CHECK(text == "running=ota_0 c6=writing ota=done fragments=2/2 target=c6");
    // While the C6 is written: no new OTA of either kind, no reboot, no C6 control.
    CHECK(start(s, 1, k_b).st() == status::einprogress);
    CHECK(start_target(s, 1, 1, k_b).st() == status::einprogress);
    CHECK(simple(s, 0x03, k_b).b[1] == static_cast<uint8_t>(status::einprogress));
    CHECK(!s.reboot_requested());
    CHECK(simple(s, 0x0b, k_b).b[1] == static_cast<uint8_t>(status::einprogress));

    c6.writing = false;  // done
    reply r = simple(s, 0x0b, k_b);
    CHECK(r.n == 2 && r.b[0] == 0x8b && r.b[1] == 0 && radio.downloads == 1);
    r = simple(s, 0x0a, k_b);
    CHECK(r.n == 2 && r.b[0] == 0x8a && r.b[1] == 0 && radio.resets == 1);
    CHECK(start(s, 1, k_b).st() == status::ok);                   // a P4 OTA again

    // A board without a C6 target or reset lines.
    server none(p4, 30000);
    CHECK(start_target(none, 1, 1, k_a).st() == status::enoent);
    CHECK(simple(none, 0x0a, k_a).b[1] == static_cast<uint8_t>(status::enoent));
    CHECK(start_target(none, 1, 0, k_a).st() == status::ok);      // explicit target 0 = P4
}

}  // namespace

int main()
{
    test_happy_path_out_of_order_and_dups();
    test_multi_client();
    test_bounds();
    test_timeout_and_failures();
    test_reboot();
    test_restart_after_done();
    test_fs_write_overwrite_append_truncate();
    test_fs_one_file_owner_names_timeout();
    test_fs_reboot_and_write_failure_close();
    test_c6_target();
    if (g_failed)
    {
        std::printf("%d check(s) failed\n", g_failed);
        return 1;
    }
    std::printf("udp_ota_test OK\n");
    return 0;
}
