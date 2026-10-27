#include "dplane_queue.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include <bfc/cv_reactor.hpp>
#include <bfc/event_queue.hpp>
#include <bfc/shared_sized_buffer.hpp>

#include "sdkconfig.h"
#ifdef CONFIG_WINJECT_PROXY_DPLANE_PSRAM
#include "esp_heap_caps.h"
#endif

namespace {

using dplane_queue_t = bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>;
using dplane_queue_registry = bfc::cv_reactor_circular_lossy_queue_registry<bfc::shared_sized_buffer>;
using dplane_reactor_t = bfc::cv_reactor<>;

// Set once by dplane_queue_bind() and never released: the reactor keeps a raw
// pointer to the inject queue.
std::mutex s_mu;
std::shared_ptr<dplane_queue_t> s_inject;
std::shared_ptr<dplane_queue_t> s_forward;
std::shared_ptr<dplane_reactor_t> s_reactor;

}  // namespace

const char *dplane_queue_bind(const char *inject_queue, const char *forward_queue, const char *reactor,
                              dplane_queue_ready_cb_t on_ready, void *ctx)
{
    if (!inject_queue || !*inject_queue || !forward_queue || !*forward_queue || !reactor || !*reactor ||
        !on_ready) {
        return "needs inject queue, forward queue and reactor";
    }
    if (std::string(inject_queue) == forward_queue) {
        return "inject and forward queues must differ";
    }
    auto inject = dplane_queue_registry::get_instance().get(inject_queue);
    auto forward = dplane_queue_registry::get_instance().get(forward_queue);
    auto cv = bfc::cv_reactor_registry::get_instance().get(reactor);
    if (!inject) {
        return "inject queue not registered";
    }
    if (!forward) {
        return "forward queue not registered";
    }
    if (!cv) {
        return "reactor not registered";
    }
    if (inject == forward) {
        return "inject and forward are the same queue";
    }
    if (inject->capacity() == 0 || forward->capacity() == 0) {
        return "queue capacity is 0";
    }

    std::lock_guard<std::mutex> lock(s_mu);
    if (s_reactor) {
        return "already bound";
    }
    s_inject = inject;
    s_forward = std::move(forward);
    s_reactor = cv;
    cv->add_read_rdy(*inject, [on_ready, ctx]() { on_ready(ctx); });
    // Datagrams queued before the bind.
    cv->wake_up();
    return nullptr;
}

size_t dplane_queue_drain(dplane_queue_datagram_cb_t cb, void *ctx, size_t max)
{
    std::shared_ptr<dplane_queue_t> inject;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        inject = s_inject;
    }
    if (!inject) {
        return 0;
    }
    size_t n = 0;
    while (n < max) {
        auto pkt = inject->pop();
        if (!pkt) {
            break;
        }
        ++n;
        cb(ctx, pkt->u8(), pkt->size());
    }
    return n;
}

bool dplane_queue_push(const uint8_t *data, size_t len)
{
    std::shared_ptr<dplane_queue_t> forward;
    std::shared_ptr<dplane_reactor_t> cv;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        forward = s_forward;
        cv = s_reactor;
    }
    if (!forward || !cv) {
        return false;
    }
#ifdef CONFIG_WINJECT_PROXY_DPLANE_PSRAM
    auto *bytes = static_cast<std::byte *>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!bytes) {
        return true;  // dropped: counted the same as an eviction
    }
    std::memcpy(bytes, data, len);
    const size_t before =
        forward->push(bfc::shared_sized_buffer::adopt(bytes, len, len, [](std::byte *p) { heap_caps_free(p); }));
#else
    const size_t before = forward->push(bfc::shared_sized_buffer::copy_from(data, len));
#endif
    const size_t capacity = forward->capacity();
    cv->wake_up();
    return capacity != 0 && before >= capacity;
}
