#pragma once

// P4 -> C6 send queue of the SDIO link (no IDF dependencies; host-tested).
//
// A ring of slots, each the size of one C6 receive buffer. Messages are packed
// into slots whole, in order: one never crosses a slot boundary, and a slot
// with room left ends with a zero header (plane 0, len 0), which ends parsing
// of that buffer. Consecutive slots go to the C6 in one host write, which the
// slave splits into its receive buffers along the same boundaries, so the C6
// parses each buffer in place.
//
// Not thread-safe: the caller serialises access.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *mem;        // slots * slot_size bytes
    uint16_t *msgs;      // messages per slot
    uint32_t *bytes;     // payload bytes per slot
    uint16_t slots;
    uint16_t slot_size;
    uint16_t head;       // oldest slot holding data
    uint16_t used;       // slots holding data, the open one included
    bool open;           // the newest used slot still takes messages
    uint16_t fill;       // bytes in the open slot
} sdio_tx_ring_t;

// mem / msgs / bytes are the caller's (slots entries each).
void sdio_tx_ring_init(sdio_tx_ring_t *r, uint8_t *mem, uint16_t *msgs, uint32_t *bytes, uint16_t slots,
                       uint16_t slot_size);

typedef enum {
    SDIO_TX_RING_QUEUED,
    SDIO_TX_RING_FULL,     // all slots used and the open one too full: retry later
    SDIO_TX_RING_INVALID,  // unknown plane, or larger than a slot: never fits
} sdio_tx_ring_put_result_t;

// Appends one message; the queue is unchanged unless it was queued.
sdio_tx_ring_put_result_t sdio_tx_ring_put(sdio_tx_ring_t *r, uint8_t plane, const void *payload, uint16_t len);

// The run of slots to write next: up to `max` slots from the head, contiguous
// in memory (not past the ring's end). The open slot, if in the run, is closed.
// Returns the slot count (0 = empty); *first = index of the first slot.
uint16_t sdio_tx_ring_take(sdio_tx_ring_t *r, uint16_t max, uint16_t *first);

// Messages and payload bytes in `n` slots from the head (for counters).
void sdio_tx_ring_count(const sdio_tx_ring_t *r, uint16_t n, uint32_t *msgs, uint32_t *bytes);

// Frees `n` slots from the head (written, or dropped).
void sdio_tx_ring_release(sdio_tx_ring_t *r, uint16_t n);

// Drops everything (link down).
void sdio_tx_ring_clear(sdio_tx_ring_t *r);

#ifdef __cplusplus
}
#endif
