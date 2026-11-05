#include "mplane_route.h"

#include <string.h>

static bool is_blank(uint8_t c)
{
    return c == ' ' || c == '\t';
}

static bool same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

// Parses a decimal u8 at p[0..len); returns the digit count, or 0.
static size_t parse_u8(const uint8_t *p, size_t len, unsigned *value)
{
    size_t i = 0;
    unsigned v = 0;
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        v = v * 10 + (unsigned)(p[i] - '0');
        if (v > 255 || i >= 3) {
            return 0;
        }
        ++i;
    }
    *value = v;
    return i;
}

static bool append(uint8_t *out, size_t cap, size_t *pos, const void *data, size_t len)
{
    if (*pos + len > cap) {
        return false;
    }
    memcpy(out + *pos, data, len);
    *pos += len;
    return true;
}

static size_t format_u8(char *buf, unsigned v)
{
    char tmp[3];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (size_t i = 0; i < n; ++i) {
        buf[i] = tmp[n - 1 - i];
    }
    return n;
}

static bool batch_has_id(const mplane_route_batch_t *batch, uint8_t id)
{
    for (size_t i = 0; i < batch->count; ++i) {
        if (batch->id[i] == id) {
            return true;
        }
    }
    return false;
}

static uint8_t allocate_id(const mplane_router_t *r, const mplane_route_batch_t *batch, uint8_t *next_id)
{
    uint8_t id = *next_id;
    for (unsigned i = 0; i < 255; ++i) {
        if (!r->slot[id].used && !batch_has_id(batch, id)) {
            *next_id = (uint8_t)(id == 255 ? 1 : id + 1);
            return id;
        }
        id = (uint8_t)(id == 255 ? 1 : id + 1);
    }
    return 0;
}

void mplane_router_init(mplane_router_t *r)
{
    memset(r, 0, sizeof(*r));
    r->next_id = 1;
}

// Same rules as the C6 (mplane_peel_cmd_prefix): case-insensitive "cmd:",
// decimal 0-255, then a blank, then a non-empty command. Returns the offset of
// the command, 0 for no prefix, or -1 for a malformed one.
static int peel_cmd(const uint8_t *s, size_t len, unsigned *tag)
{
    if (len < 4 || !((s[0] | 0x20) == 'c' && (s[1] | 0x20) == 'm' && (s[2] | 0x20) == 'd' && s[3] == ':')) {
        return 0;
    }
    size_t digits = parse_u8(s + 4, len - 4, tag);
    size_t p = 4 + digits;
    if (digits == 0 || p >= len || !is_blank(s[p])) {
        return -1;
    }
    while (p < len && is_blank(s[p])) {
        ++p;
    }
    return p < len ? (int)p : -1;
}

int mplane_route_request(mplane_router_t *r, const struct sockaddr_in *from, const uint8_t *in, size_t len,
                         uint8_t *out, size_t cap, mplane_route_batch_t *batch)
{
    size_t pos = 0;
    size_t start = 0;
    uint8_t next_id = r->next_id;
    bool route_tagged[255];
    uint8_t route_tag[255];
    batch->count = 0;
    while (start < len) {
        size_t end = start;
        while (end < len && in[end] != '\n') {
            ++end;
        }
        const bool has_newline = end < len;
        // The line without blanks at either end, as the C6 trims it.
        size_t b = start;
        size_t e = end;
        while (b < e && is_blank(in[b])) {
            ++b;
        }
        while (e > b && is_blank(in[e - 1])) {
            --e;
        }
        unsigned tag = 0;
        const int cmd = (b == e || in[b] == '#') ? -1 : peel_cmd(in + b, e - b, &tag);
        if (cmd < 0) {
            // Nothing to route (blank, comment) or the C6 answers it untagged.
            if (!append(out, cap, &pos, in + start, end - start)) {
                return -1;
            }
        } else {
            if (batch->count == 255) {
                batch->count = 0;
                return -1;
            }
            const uint8_t id = allocate_id(r, batch, &next_id);
            if (id == 0) {
                batch->count = 0;
                return -1;
            }
            const size_t route = batch->count;
            batch->id[route] = id;
            route_tagged[route] = cmd > 0;
            route_tag[route] = (uint8_t)tag;
            ++batch->count;
            char prefix[8] = "cmd:";
            size_t n = 4 + format_u8(prefix + 4, id);
            prefix[n++] = ' ';
            if (!append(out, cap, &pos, prefix, n) || !append(out, cap, &pos, in + b + cmd, e - b - (size_t)cmd)) {
                batch->count = 0;
                return -1;
            }
        }
        if (has_newline && !append(out, cap, &pos, "\n", 1)) {
            batch->count = 0;
            return -1;
        }
        start = end + 1;
    }
    for (size_t i = 0; i < batch->count; ++i) {
        mplane_route_slot_t *s = &r->slot[batch->id[i]];
        s->peer = *from;
        s->used = true;
        s->tagged = route_tagged[i];
        s->tag = route_tag[i];
    }
    r->next_id = next_id;
    r->last_peer = *from;
    r->have_last = true;
    return (int)pos;
}

void mplane_route_cancel_request(mplane_router_t *r, const mplane_route_batch_t *batch)
{
    for (size_t i = 0; i < batch->count; ++i) {
        r->slot[batch->id[i]].used = false;
    }
}

size_t mplane_route_reply(mplane_router_t *r, const uint8_t *in, size_t len, uint8_t *scratch, size_t cap,
                          mplane_route_emit_fn emit, void *ctx)
{
    size_t dropped = 0;
    size_t pos = 0;
    struct sockaddr_in to;
    bool have_to = false;
    size_t start = 0;
    while (start < len) {
        size_t end = start;
        while (end < len && in[end] != '\n') {
            ++end;
        }
        const size_t line_end = end < len ? end + 1 : end;  // keeps the newline
        const uint8_t *line = in + start;
        const size_t line_len = line_end - start;

        // "OK:<id>" or "NOK:<id>", then end of line or a blank.
        const mplane_route_slot_t *slot = NULL;
        size_t word = 0;
        size_t id_end = 0;
        bool tagged_line = false;
        if (line_len >= 4 && memcmp(line, "OK:", 3) == 0) {
            word = 2;
        } else if (line_len >= 5 && memcmp(line, "NOK:", 4) == 0) {
            word = 3;
        }
        if (word) {
            unsigned id = 0;
            const size_t digits = parse_u8(line + word + 1, line_len - word - 1, &id);
            id_end = word + 1 + digits;
            if (digits && (id_end == line_len || is_blank(line[id_end]) || line[id_end] == '\n' ||
                           line[id_end] == '\r')) {
                tagged_line = true;
                slot = (id >= 1 && r->slot[id].used) ? &r->slot[id] : NULL;
                if (slot != NULL && r->active_reply_id != id) {
                    if (r->active_reply_id != 0) {
                        r->slot[r->active_reply_id].used = false;
                    }
                    r->active_reply_id = (uint8_t)id;
                }
            }
        }

        const struct sockaddr_in *dest = NULL;
        if (tagged_line) {
            dest = slot ? &slot->peer : NULL;
        } else if (r->have_last) {
            dest = &r->last_peer;
        }
        if (dest == NULL) {
            ++dropped;
            start = line_end;
            continue;
        }

        // Rebuild the line: the sender's own tag, or none.
        uint8_t head[8];
        size_t head_len = 0;
        const uint8_t *rest = line;
        size_t rest_len = line_len;
        if (tagged_line) {
            memcpy(head, line, word);
            head_len = word;
            if (slot->tagged) {
                head[head_len++] = ':';
                head_len += format_u8((char *)head + head_len, slot->tag);
            }
            rest = line + id_end;
            rest_len = line_len - id_end;
        }
        if (head_len + rest_len > cap) {
            ++dropped;  // one line longer than a datagram
            start = line_end;
            continue;
        }
        if (have_to && (!same_peer(&to, dest) || pos + head_len + rest_len > cap)) {
            emit(ctx, &to, scratch, pos);
            pos = 0;
        }
        to = *dest;
        have_to = true;
        append(scratch, cap, &pos, head, head_len);
        append(scratch, cap, &pos, rest, rest_len);
        start = line_end;
    }
    if (have_to && pos) {
        emit(ctx, &to, scratch, pos);
    }
    return dropped;
}
