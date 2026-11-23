#include "sdio_tx_ring.h"

#include <string.h>

#include "sdio_msg.h"

void sdio_tx_ring_init(sdio_tx_ring_t *r, uint8_t *mem, uint16_t *msgs, uint32_t *bytes, uint16_t slots,
                       uint16_t slot_size)
{
    memset(r, 0, sizeof(*r));
    r->mem = mem;
    r->msgs = msgs;
    r->bytes = bytes;
    r->slots = slots;
    r->slot_size = slot_size;
}

static uint8_t *slot(const sdio_tx_ring_t *r, uint16_t i)
{
    return r->mem + (size_t)i * r->slot_size;
}

sdio_tx_ring_put_result_t sdio_tx_ring_put(sdio_tx_ring_t *r, uint8_t plane, const void *payload, uint16_t len)
{
    const size_t size = SDIO_MSG_SIZE(len);
    if (len > SDIO_MSG_MAX_PAYLOAD || size > r->slot_size) {
        return SDIO_TX_RING_INVALID;
    }
    const bool new_slot = !r->open || r->fill + size > r->slot_size;
    if (new_slot && r->used == r->slots) {
        return SDIO_TX_RING_FULL;
    }
    const uint16_t i = (uint16_t)((r->head + r->used - (new_slot ? 0 : 1)) % r->slots);
    const uint16_t at = new_slot ? 0 : r->fill;
    uint8_t *p = slot(r, i) + at;
    // Encoded before the slot is taken; sdio_msg_encode writes nothing when
    // it rejects a message (unknown plane), so the queue stays as it was.
    if (sdio_msg_encode(p, plane, payload, len) != size) {
        return SDIO_TX_RING_INVALID;
    }
    if (new_slot) {
        r->used++;
        r->open = true;
        r->msgs[i] = 0;
        r->bytes[i] = 0;
    }
    r->fill = (uint16_t)(at + size);
    r->msgs[i]++;
    r->bytes[i] += len;
    // End the buffer here unless the message filled it.
    if (r->fill + SDIO_MSG_HDR_LEN <= r->slot_size) {
        memset(p + size, 0, SDIO_MSG_HDR_LEN);
    }
    return SDIO_TX_RING_QUEUED;
}

uint16_t sdio_tx_ring_take(sdio_tx_ring_t *r, uint16_t max, uint16_t *first)
{
    *first = r->head;
    uint16_t n = r->used;
    if (n > r->slots - r->head) {
        n = (uint16_t)(r->slots - r->head);
    }
    if (n > max) {
        n = max;
    }
    if (n > 0 && r->open && n == r->used) {
        r->open = false;  // the newest slot goes out with this run
    }
    return n;
}

void sdio_tx_ring_count(const sdio_tx_ring_t *r, uint16_t n, uint32_t *msgs, uint32_t *bytes)
{
    *msgs = 0;
    *bytes = 0;
    for (uint16_t k = 0; k < n && k < r->used; k++) {
        const uint16_t i = (uint16_t)((r->head + k) % r->slots);
        *msgs += r->msgs[i];
        *bytes += r->bytes[i];
    }
}

void sdio_tx_ring_release(sdio_tx_ring_t *r, uint16_t n)
{
    if (n > r->used) {
        n = r->used;
    }
    r->head = (uint16_t)((r->head + n) % r->slots);
    r->used = (uint16_t)(r->used - n);
    if (r->used == 0) {
        r->open = false;
    }
}

void sdio_tx_ring_clear(sdio_tx_ring_t *r)
{
    sdio_tx_ring_release(r, r->used);
}
