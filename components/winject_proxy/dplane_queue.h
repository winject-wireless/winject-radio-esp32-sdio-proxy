#pragma once

// Queue d-plane of winject_proxy: the same datagrams as UDP 9000, carried over
// two in-process bfc::reactive_circular_lossy_queue<bfc::shared_sized_buffer>
// found by name in bfc::cv_reactor_circular_lossy_queue_registry, woken by a
// bfc::cv_reactor<> from bfc::cv_reactor_registry. The host registers all three
// and runs the reactor; winject-l3's WifiQueue is the other end (its wifi_tx is
// our inject queue, its wifi_rx our forward queue).
//
// Bound once for the life of the program: the queues and the reactor are held
// from then on, so the host may unregister them without pulling them away.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runs on the reactor thread when the inject queue has data. Keep it short
// (e.g. notify the task that drains it): the reactor is shared.
typedef void (*dplane_queue_ready_cb_t)(void *ctx);
typedef void (*dplane_queue_datagram_cb_t)(void *ctx, const uint8_t *data, size_t len);

// Looks up the queues and the reactor and starts watching the inject queue.
// Returns NULL, or why it failed.
const char *dplane_queue_bind(const char *inject_queue, const char *forward_queue, const char *reactor,
                              dplane_queue_ready_cb_t on_ready, void *ctx);

// Pops up to `max` inject datagrams and passes each to `cb`; the data is valid
// only during the call. Returns how many were popped.
size_t dplane_queue_drain(dplane_queue_datagram_cb_t cb, void *ctx, size_t max);

// Pushes a copy of a forward frame and wakes the reactor. Returns true when the
// full queue dropped its oldest frame to make room.
bool dplane_queue_push(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
