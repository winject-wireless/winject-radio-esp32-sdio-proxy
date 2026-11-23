// Host test of the P4 -> C6 send queue (components/sdio_link_host/sdio_tx_ring.c):
// every slot must parse on its own (as a C6 receive buffer) into whole
// messages, in send order.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdio_msg.h"
#include "sdio_tx_ring.h"

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

#define SLOTS 4
#define SLOT 1536

static uint8_t s_mem[SLOTS * SLOT];
static uint16_t s_msgs[SLOTS];
static uint32_t s_bytes[SLOTS];

struct seen {
    int n;
    uint32_t id[64];
    uint16_t len[64];
};

static void cb(void *ctx, uint8_t plane, const uint8_t *payload, uint16_t len)
{
    struct seen *s = ctx;
    CHECK(plane == SDIO_MSG_PLANE_DPLANE);
    uint32_t id;
    memcpy(&id, payload, sizeof(id));
    s->id[s->n] = id;
    s->len[s->n] = len;
    s->n++;
}

static void put(sdio_tx_ring_t *r, uint32_t id, uint16_t len, bool expect)
{
    uint8_t p[1504];
    memset(p, 0xee, sizeof(p));
    memcpy(p, &id, sizeof(id));
    CHECK((sdio_tx_ring_put(r, SDIO_MSG_PLANE_DPLANE, p, len) == SDIO_TX_RING_QUEUED) == expect);
}

// Parses slots [first, first + n) one at a time, as the C6 does.
static void parse_slots(uint16_t first, uint16_t n, struct seen *s)
{
    for (uint16_t k = 0; k < n; k++) {
        sdio_msg_parse_result_t r;
        const size_t used = sdio_msg_parse(s_mem + (size_t)(first + k) * SLOT, SLOT, cb, s, &r);
        CHECK(r == SDIO_MSG_PARSE_OK && used == SLOT);
    }
}

static void test_pack_and_order(void)
{
    sdio_tx_ring_t r;
    memset(s_mem, 0xaa, sizeof(s_mem));  // no zeroing by the caller
    sdio_tx_ring_init(&r, s_mem, s_msgs, s_bytes, SLOTS, SLOT);
    put(&r, 1, 1400, true);  // slot 0 (1404 B, 132 left)
    put(&r, 2, 100, true);   // slot 0 (104 B, 28 left)
    put(&r, 3, 100, true);   // does not fit: slot 1
    uint16_t first;
    uint16_t n = sdio_tx_ring_take(&r, 8, &first);
    CHECK(first == 0 && n == 2);
    uint32_t msgs, bytes;
    sdio_tx_ring_count(&r, n, &msgs, &bytes);
    CHECK(msgs == 3 && bytes == 1600);
    struct seen s = {0};
    parse_slots(first, n, &s);
    CHECK(s.n == 3 && s.id[0] == 1 && s.id[1] == 2 && s.id[2] == 3);
    // Taken run closed the open slot: a new message opens slot 2.
    put(&r, 5, 10, true);
    CHECK(r.used == 3);
    sdio_tx_ring_release(&r, n);
    CHECK(r.head == 2 && r.used == 1);
}

static void test_full_and_wrap(void)
{
    sdio_tx_ring_t r;
    sdio_tx_ring_init(&r, s_mem, s_msgs, s_bytes, SLOTS, SLOT);
    for (uint32_t i = 0; i < SLOTS; i++) {
        put(&r, 10 + i, 1400, true);
    }
    put(&r, 99, 1400, false);  // full
    put(&r, 98, 100, true);    // still fits in the open (last) slot
    uint16_t first;
    CHECK(sdio_tx_ring_take(&r, 3, &first) == 3 && first == 0);
    sdio_tx_ring_release(&r, 3);
    // Slot 3 still open; two more 1400s go to slots 0 and 1 (wrapped).
    put(&r, 20, 1400, true);
    put(&r, 21, 1400, true);
    CHECK(r.used == 3);
    // The run stops at the ring's end.
    uint16_t n = sdio_tx_ring_take(&r, 8, &first);
    CHECK(first == 3 && n == 1);
    struct seen s = {0};
    parse_slots(first, n, &s);
    CHECK(s.n == 2 && s.id[0] == 13 && s.id[1] == 98);
    sdio_tx_ring_release(&r, n);
    n = sdio_tx_ring_take(&r, 8, &first);
    CHECK(first == 0 && n == 2);
    memset(&s, 0, sizeof(s));
    parse_slots(first, n, &s);
    CHECK(s.n == 2 && s.id[0] == 20 && s.id[1] == 21);
    sdio_tx_ring_clear(&r);
    CHECK(r.used == 0 && !r.open);
}

static void test_exact_fill(void)
{
    // Messages that end exactly at the slot end leave no terminator.
    sdio_tx_ring_t r;
    sdio_tx_ring_init(&r, s_mem, s_msgs, s_bytes, SLOTS, SLOT);
    put(&r, 1, SLOT / 2 - 4, true);
    put(&r, 2, SLOT / 2 - 4, true);
    CHECK(r.fill == SLOT && r.used == 1);
    put(&r, 3, 4, true);
    CHECK(r.used == 2);
    uint16_t first;
    const uint16_t n = sdio_tx_ring_take(&r, 8, &first);
    struct seen s = {0};
    parse_slots(first, n, &s);
    CHECK(s.n == 3 && s.id[2] == 3);
    put(&r, 4, SDIO_MSG_MAX_PAYLOAD + 1, false);
}

static void test_rejected_plane(void)
{
    // An unknown plane is refused without taking a slot or touching the open
    // one; a full queue says FULL, an oversized message INVALID.
    sdio_tx_ring_t r;
    memset(s_mem, 0xaa, sizeof(s_mem));
    sdio_tx_ring_init(&r, s_mem, s_msgs, s_bytes, SLOTS, SLOT);
    uint8_t p[1400] = {0};
    CHECK(sdio_tx_ring_put(&r, 0x7f, p, 10) == SDIO_TX_RING_INVALID);
    CHECK(r.used == 0 && !r.open);
    put(&r, 1, 100, true);
    CHECK(sdio_tx_ring_put(&r, 0x7f, p, 10) == SDIO_TX_RING_INVALID);
    CHECK(r.used == 1 && r.fill == 104);
    uint16_t first;
    const uint16_t n = sdio_tx_ring_take(&r, 8, &first);
    struct seen s = {0};
    parse_slots(first, n, &s);
    CHECK(s.n == 1 && s.id[0] == 1);
    sdio_tx_ring_release(&r, n);
    for (int i = 0; i < SLOTS; i++) {
        put(&r, 10 + i, 1400, true);
    }
    CHECK(sdio_tx_ring_put(&r, SDIO_MSG_PLANE_DPLANE, p, 1400) == SDIO_TX_RING_FULL);
    CHECK(sdio_tx_ring_put(&r, SDIO_MSG_PLANE_DPLANE, p, SLOT) == SDIO_TX_RING_INVALID);
}

int main(void)
{
    test_pack_and_order();
    test_full_and_wrap();
    test_exact_fill();
    test_rejected_plane();
    printf("sdio_tx_ring_test OK\n");
    return 0;
}
