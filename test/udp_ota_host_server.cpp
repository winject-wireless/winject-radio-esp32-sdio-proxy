// The udp_ota core on a host UDP socket, writing the image to a file; for
// testing tools/udp_ota.py over a lossy path:
//   udp_ota_host_server <port> <out file> [drop_pct] [dup_pct] [fs dir]
// drop_pct of requests are ignored, and dup_pct of replies are sent twice. A
// C6 image (flash-c6) goes to <out file>.c6, and its "C6 write" takes 2 s.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "fs.hpp"
#include "server.hpp"

using namespace udp_ota;

namespace
{

struct file_flash : backend
{
    std::string path;
    std::vector<uint8_t> img;
    size_t capacity() override { return 3 * 1024 * 1024; }
    bool begin() override
    {
        img.assign(capacity(), 0xff);
        return true;
    }
    bool write(uint32_t off, const uint8_t* d, size_t n) override
    {
        std::copy(d, d + n, img.begin() + off);
        return true;
    }
    status finish(uint32_t size) override
    {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f)
        {
            return status::eio;
        }
        std::fwrite(img.data(), 1, size, f);
        std::fclose(f);
        return status::ok;
    }
    void abort() override {}
    std::string describe() override { return "running=host"; }
};

// The C6 target: stages like file_flash, then pretends to write the C6.
struct file_c6 : file_flash, radio_control
{
    uint32_t done_at = 0;  // now_ms() when the "C6 write" ends; 0 = none
    uint32_t size = 0;
    uint32_t (*clock)() = nullptr;
    status finish(uint32_t sz) override
    {
        size = sz;
        const status st = file_flash::finish(sz);
        if (st == status::ok)
        {
            done_at = clock() + 2000;
        }
        return st;
    }
    bool busy() override { return done_at && clock() < done_at; }
    std::string describe() override
    {
        if (!done_at)
        {
            return "c6=idle c6_link=up";
        }
        if (busy())
        {
            return "c6=writing c6_written=" + std::to_string(size / 2) + "/" + std::to_string(size) + " c6_link=up";
        }
        return "c6=ok c6_link=up";
    }
    status reset() override { return status::ok; }
    status download() override { return status::ok; }
};

uint32_t now_ms()
{
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <port> <out file> [drop_pct] [dup_pct] [fs dir]\n", argv[0]);
        return 2;
    }
    const int drop = argc > 3 ? std::atoi(argv[3]) : 0;
    const int dup = argc > 4 ? std::atoi(argv[4]) : 0;
    file_flash flash;
    flash.path = argv[2];
    file_c6 c6;
    c6.path = std::string(argv[2]) + ".c6";
    c6.clock = now_ms;
    posix_file_store files(argc > 5 ? argv[5] : "");
    server srv(flash, 30000, {}, argc > 5 ? &files : nullptr, &c6, &c6);
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(std::atoi(argv[1])));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        std::perror("bind");
        return 1;
    }
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> pct(0, 99);
    uint8_t req[2048];
    uint8_t reply[server::k_reply_max];
    for (;;)
    {
        sockaddr_in from {};
        socklen_t fl = sizeof(from);
        const ssize_t n = recvfrom(fd, req, sizeof(req), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        if (n < 0 || pct(rng) < drop)
        {
            srv.tick(now_ms());
            continue;
        }
        const size_t r = srv.handle(req, static_cast<size_t>(n), {from.sin_addr.s_addr, from.sin_port}, now_ms(), reply);
        for (int i = 0; r && i < (pct(rng) < dup ? 2 : 1); ++i)
        {
            sendto(fd, reply, r, 0, reinterpret_cast<sockaddr*>(&from), fl);
        }
        if (srv.reboot_requested())
        {
            std::printf("reboot requested\n");
            return 0;
        }
    }
}
