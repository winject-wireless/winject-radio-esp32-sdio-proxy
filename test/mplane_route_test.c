/* Host test for components/winject_proxy/mplane_route.c, from the repo root:
 *   mkdir -p build_test && cc -std=c11 -Wall -Wextra -Werror -Icomponents/winject_proxy
 *      test/mplane_route_test.c components/winject_proxy/mplane_route.c -o build_test/mplane_route_test
 *   ./build_test/mplane_route_test
 */
#include "mplane_route.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

static struct sockaddr_in peer(uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

typedef struct {
    int count;
    uint16_t port[8];
    char text[8][256];
} sink_t;

static void emit(void *ctx, const struct sockaddr_in *to, const uint8_t *data, size_t len)
{
    sink_t *s = ctx;
    if (s->count < 8) {
        s->port[s->count] = ntohs(to->sin_port);
        snprintf(s->text[s->count], sizeof(s->text[0]), "%.*s", (int)len, (const char *)data);
    }
    ++s->count;
}

static const char *request(mplane_router_t *r, uint16_t from_port, const char *line)
{
    static char out[2048];
    const struct sockaddr_in from = peer(from_port);
    mplane_route_batch_t batch;
    int n = mplane_route_request(r, &from, (const uint8_t *)line, strlen(line), (uint8_t *)out, sizeof(out) - 1,
                                 &batch);
    if (n < 0) {
        return NULL;
    }
    out[n] = '\0';
    return out;
}

static size_t reply(mplane_router_t *r, sink_t *s, const char *text)
{
    uint8_t scratch[1600];
    memset(s, 0, sizeof(*s));
    return mplane_route_reply(r, (const uint8_t *)text, strlen(text), scratch, sizeof(scratch), emit, s);
}

static void two_clients_with_the_same_tag(void)
{
    mplane_router_t r;
    mplane_router_init(&r);
    sink_t s;
    CHECK(strcmp(request(&r, 1000, "cmd:5 radio_info\n"), "cmd:1 radio_info\n") == 0);
    CHECK(strcmp(request(&r, 2000, "cmd:5 tx_info\n"), "cmd:2 tx_info\n") == 0);
    // Replies in either order reach their own sender with its own tag.
    CHECK(reply(&r, &s, "OK:2 tx_info packets=3\n") == 0);
    CHECK(s.count == 1 && s.port[0] == 2000 && strcmp(s.text[0], "OK:5 tx_info packets=3\n") == 0);
    CHECK(reply(&r, &s, "OK:1 radio_info channel=1\n") == 0);
    CHECK(s.count == 1 && s.port[0] == 1000 && strcmp(s.text[0], "OK:5 radio_info channel=1\n") == 0);
    CHECK(reply(&r, &s, "NOK:1 EINVAL\n") == 0);
    CHECK(s.count == 1 && s.port[0] == 1000 && strcmp(s.text[0], "NOK:5 EINVAL\n") == 0);
}

static void untagged_client_gets_untagged_replies(void)
{
    mplane_router_t r;
    mplane_router_init(&r);
    sink_t s;
    CHECK(strcmp(request(&r, 1000, "  radio_info  \n"), "cmd:1 radio_info\n") == 0);
    CHECK(reply(&r, &s, "OK:1 radio_info a\nOK:1 rx_info b\n") == 0);
    CHECK(s.count == 1 && strcmp(s.text[0], "OK radio_info a\nOK rx_info b\n") == 0);
    CHECK(reply(&r, &s, "OK:1") == 0);
    CHECK(s.count == 1 && strcmp(s.text[0], "OK") == 0);
    CHECK(reply(&r, &s, "NOK:1 ENOSYS\n") == 0);
    CHECK(s.count == 1 && strcmp(s.text[0], "NOK ENOSYS\n") == 0);
}

static void multi_line_request_gets_an_id_per_line(void)
{
    mplane_router_t r;
    mplane_router_init(&r);
    sink_t s;
    CHECK(strcmp(request(&r, 1000, "radio_info\n# note\n\ncmd:200 tx_info\nrx_info"),
                 "cmd:1 radio_info\n# note\n\ncmd:2 tx_info\ncmd:3 rx_info") == 0);
    CHECK(reply(&r, &s, "OK:1 a\nOK:2 b\nOK:3 c\n") == 0);
    CHECK(s.count == 1 && strcmp(s.text[0], "OK a\nOK:200 b\nOK c\n") == 0);
}

static void reply_message_for_two_senders_is_split(void)
{
    mplane_router_t r;
    mplane_router_init(&r);
    sink_t s;
    request(&r, 1000, "cmd:7 a\n");
    request(&r, 2000, "cmd:8 b\n");
    CHECK(reply(&r, &s, "OK:1 x\nOK:1 y\nOK:2 z\n") == 0);
    CHECK(s.count == 2);
    CHECK(s.port[0] == 1000 && strcmp(s.text[0], "OK:7 x\nOK:7 y\n") == 0);
    CHECK(s.port[1] == 2000 && strcmp(s.text[1], "OK:8 z\n") == 0);
}

static void malformed_prefix_passes_through_and_untagged_reply_goes_to_last_sender(void)
{
    mplane_router_t r;
    mplane_router_init(&r);
    sink_t s;
    CHECK(strcmp(request(&r, 1000, "cmd:300 radio_info\n"), "cmd:300 radio_info\n") == 0);
    CHECK(strcmp(request(&r, 1000, "cmd:5\n"), "cmd:5\n") == 0);
    CHECK(reply(&r, &s, "NOK EINVAL\n") == 0);
    CHECK(s.count == 1 && s.port[0] == 1000 && strcmp(s.text[0], "NOK EINVAL\n") == 0);
}

static void unknown_id_and_no_sender_are_dropped(void)
{
    mplane_router_t r;
    mplane_router_init(&r);
    sink_t s;
    CHECK(reply(&r, &s, "OK something\n") == 1);  // no request yet
    CHECK(s.count == 0);
    request(&r, 1000, "cmd:1 a\n");
    CHECK(reply(&r, &s, "OK:42 stale\nOK:1 good\n") == 1);
    CHECK(s.count == 1 && strcmp(s.text[0], "OK:1 good\n") == 0);
}

static void ids_wrap_from_255_to_1(void)
{
    mplane_router_t r;
    mplane_router_init(&r);
    sink_t s;
    char response[32];
    for (int i = 1; i < 255; ++i) {
        CHECK(request(&r, 1000, "a\n") != NULL);
        snprintf(response, sizeof(response), "OK:%d a\n", i);
        CHECK(reply(&r, &s, response) == 0);
    }
    CHECK(strcmp(request(&r, 1000, "a\n"), "cmd:255 a\n") == 0);
    CHECK(reply(&r, &s, "OK:255 a\n") == 0);
    CHECK(strcmp(request(&r, 1000, "a\n"), "cmd:1 a\n") == 0);
}

static void request_that_does_not_fit_is_rejected(void)
{
    mplane_router_t r;
    mplane_router_init(&r);
    const struct sockaddr_in from = peer(1000);
    uint8_t out[10];
    mplane_route_batch_t batch;
    CHECK(strcmp(request(&r, 1000, "cmd:9 ping"), "cmd:1 ping") == 0);
    CHECK(mplane_route_request(&r, &from, (const uint8_t *)"radio_info", 10, out, sizeof(out), &batch) == -1);
    sink_t s;
    CHECK(reply(&r, &s, "OK:1 pong\n") == 0);
    CHECK(s.count == 1 && s.port[0] == 1000 && strcmp(s.text[0], "OK:9 pong\n") == 0);
    CHECK(strcmp(request(&r, 2000, "ping"), "cmd:2 ping") == 0);
}

static void pending_ids_are_not_reused_and_failed_send_can_be_cancelled(void)
{
    mplane_router_t r;
    mplane_router_init(&r);
    const struct sockaddr_in from = peer(1000);
    uint8_t out[32];
    mplane_route_batch_t batch;
    for (int id = 1; id <= 255; ++id) {
        CHECK(mplane_route_request(&r, &from, (const uint8_t *)"ping", 4, out, sizeof(out), &batch) > 0);
    }
    CHECK(mplane_route_request(&r, &from, (const uint8_t *)"ping", 4, out, sizeof(out), &batch) == -1);

    sink_t s;
    CHECK(reply(&r, &s, "OK:1 pong\nOK:2 pong\n") == 0);
    CHECK(mplane_route_request(&r, &from, (const uint8_t *)"ping", 4, out, sizeof(out), &batch) > 0);
    CHECK(batch.count == 1 && batch.id[0] == 1);
    mplane_route_cancel_request(&r, &batch);
    CHECK(reply(&r, &s, "OK:1 stale\n") == 1);
}

int main(void)
{
    two_clients_with_the_same_tag();
    untagged_client_gets_untagged_replies();
    multi_line_request_gets_an_id_per_line();
    reply_message_for_two_senders_is_split();
    malformed_prefix_passes_through_and_untagged_reply_goes_to_last_sender();
    unknown_id_and_no_sender_are_dropped();
    ids_wrap_from_255_to_1();
    request_that_does_not_fit_is_rejected();
    pending_ids_are_not_reused_and_failed_send_can_be_cancelled();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    puts("mplane_route_test: all passed");
    return EXIT_SUCCESS;
}
