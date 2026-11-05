// m-plane routing for several UDP clients on one C6.
//
// The C6 m-plane is request-response: it runs one request line at a time and,
// for a line prefixed `cmd:<u8>`, tags every reply line `OK:<u8>` /
// `NOK:<u8>`. A reply can span several SDIO messages and has no end marker,
// so replies are routed by tag, not by order. The router gives each request
// line its own proxy id, remembers the sender and the sender's own tag (if
// any), and on the way back puts the sender's tag back (or removes the tag for
// an untagged sender) and sends the line to that sender.
//
// Plain C with no ESP-IDF dependency, so it builds on the host for tests. Not
// thread safe: the caller serialises calls.
#ifndef WINJECT_PROXY_MPLANE_ROUTE_H_
#define WINJECT_PROXY_MPLANE_ROUTE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <netinet/in.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    struct sockaddr_in peer;
    bool used;
    bool tagged;  // the sender's line had its own cmd:<u8>
    uint8_t tag;  // that tag
} mplane_route_slot_t;

typedef struct {
    // Indexed by proxy id 1-255. An id remains reserved until the ordered C6
    // reply stream advances to the following id.
    mplane_route_slot_t slot[256];
    uint8_t next_id;
    // Untagged reply lines (the C6's NOK for a malformed cmd: prefix) go to
    // the sender of the most recent request.
    struct sockaddr_in last_peer;
    bool have_last;
    // The C6 processes command lines serially. Once a reply changes id, the
    // previous id is complete and can be reused.
    uint8_t active_reply_id;
} mplane_router_t;

typedef struct {
    uint8_t id[255];
    size_t count;
} mplane_route_batch_t;

void mplane_router_init(mplane_router_t *r);

// Rewrites one request datagram from `from` for the C6: each command line gets
// `cmd:<proxy id> `. Blank and `#` lines and lines with a malformed cmd:
// prefix pass through unchanged. Returns the output length, or -1 if the
// output does not fit in `cap` or no request id is free. On failure the
// router is unchanged. `batch` records the allocated ids so the caller can
// cancel them if sending the rewritten request fails.
int mplane_route_request(mplane_router_t *r, const struct sockaddr_in *from, const uint8_t *in, size_t len,
                         uint8_t *out, size_t cap, mplane_route_batch_t *batch);

// Releases ids allocated by a successful request that was not sent. The
// caller must serialize this with request and reply routing.
void mplane_route_cancel_request(mplane_router_t *r, const mplane_route_batch_t *batch);

typedef void (*mplane_route_emit_fn)(void *ctx, const struct sockaddr_in *to, const uint8_t *data, size_t len);

// Routes one reply message from the C6. Consecutive lines for the same sender
// are passed to `emit` as one datagram, with the sender's tag restored.
// `scratch` holds one datagram (at least `len` + 4 bytes). Returns the number
// of lines dropped (no known sender for the tag, or no sender at all).
size_t mplane_route_reply(mplane_router_t *r, const uint8_t *in, size_t len, uint8_t *scratch, size_t cap,
                          mplane_route_emit_fn emit, void *ctx);

#ifdef __cplusplus
}
#endif

#endif  // WINJECT_PROXY_MPLANE_ROUTE_H_
