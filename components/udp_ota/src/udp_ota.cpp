#include "udp_ota.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "fs.hpp"
#include "lwip/sockets.h"
#include "sdio_link_host.h"
#include "sdio_msg.h"
#include "sdio_ota.h"
#include "server.hpp"

namespace udp_ota
{
namespace
{

const char* TAG = "udp_ota";

udp_ota_config_t g_cfg;
/* sdio_link_host_start() succeeded (udp_ota_c6_link_started): before that the
 * link host's locks do not exist, so the C6 target and controls stay off. */
std::atomic<bool> g_c6_link {false};

void set_active(bool on)
{
    if (g_cfg.on_active)
    {
        g_cfg.on_active(on, g_cfg.ctx);
    }
}

const char* img_state_name(const esp_partition_t* p)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) != ESP_OK)
    {
        return "unknown";
    }
    switch (st)
    {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

/* The P4's inactive OTA slot, written in any order with every sector erased on
 * its first write: the P4 app target, and the staging area of the C6 target. */
class slot_writer
{
public:
    bool open()
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        /* A finished P4 image not booted yet: point the boot back at the
         * running one before its slot is overwritten. */
        if (esp_ota_get_boot_partition() != running)
        {
            esp_ota_set_boot_partition(running);
        }
        m_part = esp_ota_get_next_update_partition(nullptr);
        if (!m_part)
        {
            return false;
        }
        m_erased.assign((m_part->size + m_part->erase_size - 1) / m_part->erase_size, false);
        return true;
    }

    // Erases the sectors [offset, offset + len) touches that are not erased yet.
    bool erase_for(uint32_t offset, size_t len)
    {
        const uint32_t es = m_part->erase_size;
        for (uint32_t s = offset / es; s <= (offset + len - 1) / es; ++s)
        {
            if (!m_erased[s])
            {
                const esp_err_t err = esp_partition_erase_range(m_part, s * es, es);
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "erase sector %u: %s", (unsigned)s, esp_err_to_name(err));
                    return false;
                }
                m_erased[s] = true;
            }
        }
        return true;
    }

    void mark_erased(uint32_t sector) { m_erased[sector] = true; }
    const esp_partition_t* part() const { return m_part; }
    size_t capacity() const
    {
        const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
        return p ? p->size : 0;
    }

private:
    const esp_partition_t* m_part = nullptr;
    std::vector<bool> m_erased;
};

/* This P4's app. esp_ota_begin() would erase the whole image size up front
 * (seconds for ~1.8 MB, with the start reply waiting on it), so it is opened
 * with a 1-byte size (erases sector 0 only); esp_ota_write_with_offset takes
 * the fragments in any order. */
class p4_app final : public backend
{
public:
    size_t capacity() override { return m_slot.capacity(); }

    bool begin() override
    {
        abort();
        if (!m_slot.open())
        {
            return false;
        }
        const esp_err_t err = esp_ota_begin(m_slot.part(), 1, &m_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "begin %s: %s", m_slot.part()->label, esp_err_to_name(err));
            m_handle = 0;
            return false;
        }
        m_slot.mark_erased(0);
        return true;
    }

    bool write(uint32_t offset, const uint8_t* data, size_t len) override
    {
        if (!m_handle || !m_slot.erase_for(offset, len))
        {
            return false;
        }
        const esp_err_t err = esp_ota_write_with_offset(m_handle, data, len, offset);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "write at %u: %s", (unsigned)offset, esp_err_to_name(err));
            return false;
        }
        return true;
    }

    status finish(uint32_t size) override
    {
        if (!m_handle)
        {
            return status::eio;
        }
        const esp_partition_t* part = m_slot.part();
        /* esp_ota_end frees the handle on every path. */
        esp_err_t err = esp_ota_end(m_handle);
        m_handle = 0;
        if (err == ESP_OK)
        {
            /* A valid image for another board (same chip) would boot and
             * confirm itself; only this project is taken. */
            esp_app_desc_t desc {};
            const esp_app_desc_t* self = esp_app_get_description();
            if (esp_ota_get_partition_description(part, &desc) != ESP_OK ||
                std::strncmp(desc.project_name, self->project_name, sizeof(desc.project_name)) != 0)
            {
                ESP_LOGE(TAG, "image %u B is project '%.32s', running '%.32s': rejected", (unsigned)size,
                         desc.project_name, self->project_name);
                return status::ebadmsg;
            }
            err = esp_ota_set_boot_partition(part);
        }
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "image %u B in %s rejected: %s", (unsigned)size, part->label, esp_err_to_name(err));
            return err == ESP_ERR_OTA_VALIDATE_FAILED ? status::ebadmsg : status::eio;
        }
        ESP_LOGI(TAG, "image %u B in %s set to boot", (unsigned)size, part->label);
        return status::ok;
    }

    void abort() override
    {
        if (m_handle)
        {
            esp_ota_abort(m_handle);
            m_handle = 0;
        }
    }

    std::string describe() override
    {
        const esp_partition_t* running = esp_ota_get_running_partition();
        const esp_partition_t* boot = esp_ota_get_boot_partition();
        const esp_app_desc_t* app = esp_app_get_description();
        char elf[17];
        esp_app_get_elf_sha256(elf, sizeof(elf));
        char line[256];
        std::snprintf(line, sizeof(line), "running=%s state=%s boot=%s project=%s version=%s built=%s_%s elf=%s",
                      running->label, img_state_name(running), boot ? boot->label : "none", app->project_name,
                      app->version, app->date, app->time, elf);
        std::string out(line);
        /* app->date is "Nov  5 2026": keep the line space-separated key=value. */
        const size_t b = out.find("built=");
        const size_t e = out.find(" elf=");
        for (size_t i = b; b != std::string::npos && i < e && i < out.size(); ++i)
        {
            if (out[i] == ' ')
            {
                out[i] = '_';
            }
        }
        return out;
    }

private:
    slot_writer m_slot;
    esp_ota_handle_t m_handle = 0;
};

/* ---- C6 over the SDIO OTA plane (sdio_ota.h): one request in flight, each
 * retried on a lost request or reply; a repeated DATA is acknowledged by the
 * C6 without writing it again. */

QueueHandle_t g_c6_replies;  // sdio_ota_reply_t
uint8_t g_c6_msg[sizeof(sdio_ota_req_t) + SDIO_OTA_CHUNK_MAX];
constexpr int k_c6_tries = 3;

esp_err_t c6_request(uint8_t op, uint32_t arg, const uint8_t* body, uint16_t body_len, uint32_t timeout_ms,
                     sdio_ota_reply_t* out)
{
    sdio_ota_req_t req = {};
    req.op = op;
    req.arg = arg;
    std::memcpy(g_c6_msg, &req, sizeof(req));
    if (body_len)
    {
        std::memcpy(g_c6_msg + sizeof(req), body, body_len);
    }
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (int t = 0; t < k_c6_tries; t++)
    {
        xQueueReset(g_c6_replies);
        err = sdio_link_host_send(SDIO_MSG_PLANE_OTA, g_c6_msg, sizeof(req) + body_len, 1000);
        if (err != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
        while ((int32_t)(xTaskGetTickCount() - deadline) < 0)
        {
            if (xQueueReceive(g_c6_replies, out, pdMS_TO_TICKS(50)) == pdTRUE && out->op == op)
            {
                return out->status == SDIO_OTA_STATUS_OK ? ESP_OK : static_cast<esp_err_t>(out->err);
            }
        }
        err = ESP_ERR_TIMEOUT;
        ESP_LOGW(TAG, "c6 op %u arg %u: no reply, retry %d", op, (unsigned)arg, t + 1);
    }
    return err;
}

/* The C6 target: the image is staged in the P4's inactive slot (fragments in
 * any order, as for the P4 app), checked for a C6 header, then a task writes
 * it to the C6 in order over SDIO, resets the C6 and checks the new image
 * answers. The new C6 image confirms itself on its first link message; if it
 * never answers, a second reset makes its bootloader boot the previous one. */
class c6_radio final : public backend, public radio_control
{
public:
    size_t capacity() override { return m_slot.capacity(); }

    bool begin() override
    {
        if (!g_c6_link.load())
        {
            ESP_LOGE(TAG, "c6: the SDIO link was never started");
            return false;
        }
        /* The staging slot holds the P4's previous image while the running one
         * is unconfirmed: overwriting it would remove the P4's rollback. */
        esp_ota_img_states_t st;
        if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
            st == ESP_OTA_IMG_PENDING_VERIFY)
        {
            ESP_LOGE(TAG, "c6: the running P4 image is not confirmed yet; its rollback slot is kept");
            return false;
        }
        if (!m_slot.open())
        {
            return false;
        }
        m_state.store(state_staging);
        return true;
    }

    bool write(uint32_t offset, const uint8_t* data, size_t len) override
    {
        if (!m_slot.erase_for(offset, len))
        {
            return false;
        }
        const esp_err_t err = esp_partition_write(m_slot.part(), offset, data, len);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "c6 stage at %u: %s", (unsigned)offset, esp_err_to_name(err));
            return false;
        }
        return true;
    }

    status finish(uint32_t size) override
    {
        esp_image_header_t hdr {};
        if (esp_partition_read(m_slot.part(), 0, &hdr, sizeof(hdr)) != ESP_OK)
        {
            m_state.store(state_failed);
            return status::eio;
        }
        if (hdr.magic != ESP_IMAGE_HEADER_MAGIC || hdr.chip_id != ESP_CHIP_ID_ESP32C6)
        {
            ESP_LOGE(TAG, "c6: image %u B is not an ESP32-C6 app (magic 0x%02x chip %u)", (unsigned)size,
                     hdr.magic, (unsigned)hdr.chip_id);
            m_state.store(state_failed);
            return status::ebadmsg;
        }
        m_size = size;
        m_written.store(0);
        m_state.store(state_writing);
        if (xTaskCreate(write_task, "udp_ota_c6", 4096, this, g_cfg.task_prio, nullptr) != pdPASS)
        {
            m_state.store(state_failed);
            return status::eio;
        }
        return status::ok;
    }

    void abort() override
    {
        if (m_state.load() == state_staging)
        {
            m_state.store(state_idle);
        }
    }

    bool busy() override { return m_state.load() == state_writing; }

    std::string describe() override
    {
        char line[96];
        switch (m_state.load())
        {
        case state_writing:
            std::snprintf(line, sizeof(line), "c6=writing c6_written=%u/%u", (unsigned)m_written.load(),
                          (unsigned)m_size);
            break;
        case state_staging: std::snprintf(line, sizeof(line), "c6=staging"); break;
        case state_ok: std::snprintf(line, sizeof(line), "c6=ok"); break;
        case state_failed: std::snprintf(line, sizeof(line), "c6=failed c6_err=%s", esp_err_to_name(m_err)); break;
        default:
            std::snprintf(line, sizeof(line), "c6=idle");
            break;
        }
        std::string out(line);
        out += sdio_link_host_in_download() ? " c6_link=download" : sdio_link_host_is_up() ? " c6_link=up" : " c6_link=down";
        return out;
    }

    status reset() override
    {
        if (!g_c6_link.load())
        {
            return status::enoent;
        }
        return sdio_link_host_reset_c6(10000) == ESP_OK ? status::ok : status::eio;
    }

    status download() override
    {
        if (!g_c6_link.load())
        {
            return status::enoent;
        }
        return sdio_link_host_enter_download() == ESP_OK ? status::ok : status::eio;
    }

private:
    enum state_t
    {
        state_idle,
        state_staging,
        state_writing,
        state_ok,
        state_failed,
    };

    static void write_task(void* arg)
    {
        static_cast<c6_radio*>(arg)->run_write();
        vTaskDelete(nullptr);
    }

    void run_write()
    {
        /* Video (or whatever the host pauses) stays off while the C6 flashes:
         * its radio stalls on every flash write anyway. The server's
         * on_active(true) from the ota_start still holds (active_filter keeps
         * its false back while this runs); this task ends it. */
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = write_c6();
        m_err = err;
        m_state.store(err == ESP_OK ? state_ok : state_failed);
        set_active(false);
        printf("UDP_OTA c6 %s: %u B in %lld ms\n", err == ESP_OK ? "updated" : esp_err_to_name(err),
               (unsigned)m_size, (long long)((esp_timer_get_time() - t0) / 1000));
    }

    esp_err_t write_c6()
    {
        static uint8_t chunk[SDIO_OTA_CHUNK_MAX];
        sdio_ota_reply_t r;
        esp_err_t err = c6_request(SDIO_OTA_BEGIN, m_size, nullptr, 0, 5000, &r);
        if (err != ESP_OK)
        {
            return err;
        }
        for (uint32_t off = 0; off < m_size;)
        {
            const uint32_t n = m_size - off < sizeof(chunk) ? m_size - off : sizeof(chunk);
            err = esp_partition_read(m_slot.part(), off, chunk, n);
            if (err == ESP_OK)
            {
                err = c6_request(SDIO_OTA_DATA, off, chunk, n, 3000, &r);
            }
            if (err == ESP_OK && r.arg != off + n)
            {
                err = ESP_ERR_INVALID_RESPONSE;
            }
            if (err != ESP_OK)
            {
                c6_request(SDIO_OTA_ABORT, 0, nullptr, 0, 1000, &r);
                return err;
            }
            off += n;
            m_written.store(off);
        }
        /* esp_ota_end on the C6 reads the whole image back to check it. */
        err = c6_request(SDIO_OTA_END, 0, nullptr, 0, 15000, &r);
        if (err != ESP_OK)
        {
            return err;
        }
        /* Boot the new image. Its first message from us confirms it; if it never
         * answers, reset once more and its bootloader boots the previous one. */
        err = sdio_link_host_reset_c6(10000);
        if (err == ESP_OK)
        {
            err = c6_request(SDIO_OTA_ABORT, 0, nullptr, 0, 2000, &r);
        }
        if (err != ESP_OK)
        {
            sdio_link_host_reset_c6(10000);
        }
        return err;
    }

    slot_writer m_slot;
    std::atomic<int> m_state {state_idle};
    std::atomic<uint32_t> m_written {0};
    uint32_t m_size = 0;
    esp_err_t m_err = ESP_OK;
};

c6_radio g_c6;

/* The server's on_active, with its false held back while the C6 write runs:
 * one true ... false pair per image, false only from the write task. */
void active_filter(bool on)
{
    if (!on && g_c6.busy())
    {
        return;
    }
    set_active(on);
}

uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

struct task_args
{
    int fd;
};

void server_task(void* arg)
{
    const int fd = static_cast<task_args*>(arg)->fd;
    static p4_app app;
    static posix_file_store files(g_cfg.fs_dir ? g_cfg.fs_dir : "");
    server srv(app, g_cfg.idle_timeout_ms, active_filter, g_cfg.fs_dir ? &files : nullptr, &g_c6, &g_c6);
    static uint8_t req[1600];
    static uint8_t reply[server::k_reply_max];
    for (;;)
    {
        sockaddr_in from {};
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(fd, req, sizeof(req), 0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 0)
        {
            srv.tick(now_ms());  // receive timeout
            continue;
        }
        udp_ota_confirm_p4_image();
        peer p;
        p.addr = from.sin_addr.s_addr;
        p.port = from.sin_port;
        const size_t r = srv.handle(req, static_cast<size_t>(n), p, now_ms(), reply);
        if (r)
        {
            sendto(fd, reply, r, 0, reinterpret_cast<sockaddr*>(&from), from_len);
        }
        if (srv.reboot_requested())
        {
            printf("UDP_OTA reboot\n");
            fflush(stdout);
            /* Give the reply time to leave (over the air: L3 TX queue). */
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
    }
}

}  // namespace
}  // namespace udp_ota

extern "C" esp_err_t udp_ota_start(const udp_ota_config_t* config)
{
    using namespace udp_ota;
    g_cfg = *config;
    g_c6_replies = xQueueCreate(2, sizeof(sdio_ota_reply_t));
    if (!g_c6_replies)
    {
        return ESP_ERR_NO_MEM;
    }
    const int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0)
    {
        return ESP_FAIL;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config->port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        close(fd);
        return ESP_FAIL;
    }
    timeval tv {};
    tv.tv_sec = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    static task_args args;
    args.fd = fd;
    const BaseType_t core = config->task_core < 0 ? tskNO_AFFINITY : config->task_core;
    if (xTaskCreatePinnedToCore(server_task, "udp_ota", config->task_stack, &args, config->task_prio, nullptr, core) !=
        pdPASS)
    {
        close(fd);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "UDP %u%s%s", (unsigned)config->port, config->fs_dir ? ", files in " : "",
             config->fs_dir ? config->fs_dir : "");
    return ESP_OK;
}

extern "C" void udp_ota_on_c6_reply(const uint8_t* payload, uint16_t len)
{
    sdio_ota_reply_t r;
    if (udp_ota::g_c6_replies == nullptr || len < sizeof(r))
    {
        return;
    }
    std::memcpy(&r, payload, sizeof(r));
    xQueueSend(udp_ota::g_c6_replies, &r, 0);
}

extern "C" void udp_ota_c6_link_started(void)
{
    udp_ota::g_c6_link.store(true);
}

extern "C" void udp_ota_confirm_p4_image(void)
{
    static std::atomic<bool> done {false};
    if (done.exchange(true))
    {
        return;
    }
    esp_ota_img_states_t state;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI("udp_ota", "new P4 image on %s confirmed (reached over the network)", running->label);
    }
}
