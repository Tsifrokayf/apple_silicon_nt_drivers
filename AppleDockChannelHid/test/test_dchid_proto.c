// SPDX-License-Identifier: MIT
/*
 * Host-side unit test for src/dchid_proto.c (framing, checksum, receive
 * state machine, event parsers). Runs on the build machine with the system
 * C compiler; see build/test.sh.
 *
 * The test vectors are synthesised from the protocol description, not
 * captured from hardware: they prove the code agrees with this project's
 * reading of the reference, not that the reading matches a real MTP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/dchid_proto.h"

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

static uint8_t body_buf[DCHID_MAX_BODY];

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/*
 * Build a device->host packet the way the device is described to: header,
 * sub-header (with retcode), payload padded to 4, checksum.
 */
static size_t make_pkt(uint8_t *out, uint8_t channel, uint8_t iface, uint8_t seq,
                       uint8_t flags, uint32_t retcode, const uint8_t *payload,
                       size_t len)
{
    size_t padded = (len + 3) & ~(size_t)3;
    size_t body = DCHID_SUBHDR_SIZE + padded;
    size_t total = DCHID_HDR_SIZE + body + 4;

    memset(out, 0, total);
    out[0] = DCHID_HDR_SIZE;
    out[1] = channel;
    wr16(out + 2, (uint16_t)body);
    out[4] = seq;
    out[5] = iface;
    out[8] = flags;
    wr16(out + 10, (uint16_t)len);
    wr32(out + 12, retcode);
    memcpy(out + 16, payload, len);
    wr32(out + total - 4, 0xffffffffu - dchid_sum_words(out, total - 4));
    return total;
}

static void test_build_frame(void)
{
    uint8_t data[2];
    uint8_t frame[64];
    size_t n;

    dchid_build_enable(data, 3);
    CHECK(data[0] == 0xb4 && data[1] == 3);

    CHECK(dchid_frame_size(2) == 8 + 8 + 4 + 4);
    n = dchid_build_frame(frame, sizeof(frame), DCHID_IFACE_COMM, 7,
                          dchid_flags(DCHID_TYPE_FEATURE, DCHID_REQ_SET_REPORT),
                          data, sizeof(data));
    CHECK(n == 24);
    CHECK(frame[0] == 8);
    CHECK(frame[1] == DCHID_CHANNEL_CMD);
    CHECK(frame[2] == 12 && frame[3] == 0); /* round_up(2,4) + subhdr */
    CHECK(frame[4] == 7);
    CHECK(frame[5] == 0);
    CHECK(frame[8] == 0x80); /* FEATURE << 6 | SET */
    CHECK(frame[10] == 2 && frame[11] == 0);
    CHECK(frame[16] == 0xb4 && frame[17] == 3 && frame[18] == 0 && frame[19] == 0);
    CHECK(dchid_sum_words(frame, n) == 0xffffffffu);

    /* Too small a buffer must be refused, not overrun. */
    CHECK(dchid_build_frame(frame, n - 1, 0, 0, 0, data, sizeof(data)) == 0);

    /* Word-aligned payload gets no padding. */
    {
        uint8_t four[4] = {1, 2, 3, 4};

        n = dchid_build_frame(frame, sizeof(frame), 2, 0, 0x81, four, 4);
        CHECK(n == 24);
        CHECK(frame[2] == 12);
        CHECK(dchid_sum_words(frame, n) == 0xffffffffu);
    }
}

static void test_rx_basic(void)
{
    dchid_rx rx;
    uint8_t pkt[64];
    uint8_t payload[5] = {0xf1, 4, 0xaa, 0xbb, 0xcc};
    size_t n, i, used;
    dchid_rx_result r = DCHID_RX_NEED_MORE;
    dchid_msg msg;

    CHECK(dchid_rx_init(&rx, body_buf, DCHID_MAX_BODY - 1) == -1);
    CHECK(dchid_rx_init(&rx, body_buf, sizeof(body_buf)) == 0);
    CHECK(dchid_rx_wanted(&rx) == 8);

    n = make_pkt(pkt, DCHID_CHANNEL_REPORT, 0, 9, 0x00, 0, payload, sizeof(payload));

    /* Byte at a time: nothing until the very last byte. */
    for (i = 0; i < n; i++) {
        if (i == 3)
            CHECK(dchid_rx_wanted(&rx) == 5);
        if (i == 8)
            CHECK(dchid_rx_wanted(&rx) == 8 + 8 + 4); /* body + checksum */
        r = dchid_rx_feed(&rx, pkt + i, 1, &used);
        CHECK(used == 1);
        if (i + 1 < n)
            CHECK(r == DCHID_RX_NEED_MORE);
    }
    CHECK(r == DCHID_RX_PACKET);
    CHECK(rx.hdr.channel == DCHID_CHANNEL_REPORT);
    CHECK(rx.hdr.seq == 9);
    CHECK(rx.hdr.length == 16);
    CHECK(dchid_rx_wanted(&rx) == 8);

    CHECK(dchid_decode_msg(&rx.hdr, rx.body, &msg) == 0);
    CHECK(msg.group == DCHID_TYPE_INPUT);
    CHECK(msg.payload_len == 5);
    CHECK(memcmp(msg.payload, payload, 5) == 0);
}

static void test_rx_stream(void)
{
    dchid_rx rx;
    uint8_t stream[256];
    uint8_t a[3] = {0x01, 0x02, 0x03};
    uint8_t b[8] = {0x02, 1, 2, 3, 4, 5, 6, 7};
    size_t na, nb, used, off;
    dchid_rx_result r;

    dchid_rx_init(&rx, body_buf, sizeof(body_buf));
    na = make_pkt(stream, DCHID_CHANNEL_REPORT, 1, 0, 0, 0, a, sizeof(a));
    nb = make_pkt(stream + na, DCHID_CHANNEL_REPORT, 2, 1, 0, 0, b, sizeof(b));

    /* Two packets in one buffer: feed must stop after the first. */
    r = dchid_rx_feed(&rx, stream, na + nb, &used);
    CHECK(r == DCHID_RX_PACKET);
    CHECK(used == na);
    CHECK(rx.hdr.iface == 1);
    off = used;
    r = dchid_rx_feed(&rx, stream + off, na + nb - off, &used);
    CHECK(r == DCHID_RX_PACKET);
    CHECK(used == nb);
    CHECK(rx.hdr.iface == 2);
}

static void test_rx_errors(void)
{
    dchid_rx rx;
    uint8_t stream[256];
    uint8_t p[4] = {9, 9, 9, 9};
    size_t n1, n2, used, off;
    dchid_rx_result r;

    dchid_rx_init(&rx, body_buf, sizeof(body_buf));

    /* Corrupted checksum: packet dropped, parser ready for the next one. */
    n1 = make_pkt(stream, DCHID_CHANNEL_REPORT, 1, 0, 0, 0, p, sizeof(p));
    stream[16] ^= 0x40;
    n2 = make_pkt(stream + n1, DCHID_CHANNEL_REPORT, 3, 0, 0, 0, p, sizeof(p));
    r = dchid_rx_feed(&rx, stream, n1 + n2, &used);
    CHECK(r == DCHID_RX_BAD_CHECKSUM);
    CHECK(used == n1);
    off = used;
    r = dchid_rx_feed(&rx, stream + off, n1 + n2 - off, &used);
    CHECK(r == DCHID_RX_PACKET);
    CHECK(rx.hdr.iface == 3);

    /* Bad hdr_len: exactly 8 bytes are dropped, as in the reference. */
    dchid_rx_reset(&rx);
    memset(stream, 0, 8);
    stream[0] = 12;
    n2 = make_pkt(stream + 8, DCHID_CHANNEL_REPORT, 5, 0, 0, 0, p, sizeof(p));
    r = dchid_rx_feed(&rx, stream, 8 + n2, &used);
    CHECK(r == DCHID_RX_BAD_HDR_LEN);
    CHECK(used == 8);
    r = dchid_rx_feed(&rx, stream + 8, n2, &used);
    CHECK(r == DCHID_RX_PACKET);
    CHECK(rx.hdr.iface == 5);

    /* Sub-header claiming more payload than the body holds. */
    {
        dchid_hdr h = {8, DCHID_CHANNEL_REPORT, 8, 0, 0};
        uint8_t body[12] = {0, 0, 1, 0};
        dchid_msg m;

        CHECK(dchid_decode_msg(&h, body, &m) == -1);
        h.length = 4;
        CHECK(dchid_decode_msg(&h, body, &m) == -1);
    }
}

static void test_ack(void)
{
    dchid_rx rx;
    uint8_t pkt[64];
    uint8_t resp[4] = {0x10, 0xac, 0x05, 0x99};
    dchid_pending pend = {4, 2, 0x81, 0x10};
    dchid_msg msg;
    size_t n, used;

    dchid_rx_init(&rx, body_buf, sizeof(body_buf));
    n = make_pkt(pkt, DCHID_CHANNEL_CMD, 4, 2, 0x81, 0x1234, resp, sizeof(resp));
    CHECK(dchid_rx_feed(&rx, pkt, n, &used) == DCHID_RX_PACKET);
    CHECK(dchid_decode_msg(&rx.hdr, rx.body, &msg) == 0);
    CHECK(msg.sub.retcode == 0x1234);
    CHECK(dchid_match_ack(&pend, &rx.hdr, &msg) == DCHID_ACK_MATCH);

    pend.seq = 3;
    CHECK(dchid_match_ack(&pend, &rx.hdr, &msg) == DCHID_ACK_BAD_SEQ);
    pend.seq = 2;
    pend.report_id = 0x11;
    CHECK(dchid_match_ack(&pend, &rx.hdr, &msg) == DCHID_ACK_BAD_REPORT);
    pend.report_id = 0x10;
    pend.flags = 0x80;
    CHECK(dchid_match_ack(&pend, &rx.hdr, &msg) == DCHID_ACK_BAD_FLAGS);
    pend.flags = 0x81;
    pend.iface = 0;
    CHECK(dchid_match_ack(&pend, &rx.hdr, &msg) == DCHID_ACK_BAD_IFACE);
    pend.iface = 4;
    msg.payload_len = 0;
    CHECK(dchid_match_ack(&pend, &rx.hdr, &msg) == DCHID_ACK_EMPTY);
}

static size_t put_block(uint8_t *p, uint16_t type, const void *data, uint16_t len)
{
    wr16(p, type);
    wr16(p + 2, len);
    memcpy(p + 4, data, len);
    return 4u + len;
}

static void test_init(void)
{
    uint8_t ev[256];
    uint8_t desc[6] = {0x05, 0x01, 0x09, 0x06, 0xa1, 0x01};
    uint8_t gpio[DCHID_GPIO_REQ_SIZE];
    const char product[] = "Apple Internal Keyboard";
    uint8_t junk[2] = {1, 2};
    size_t n = 0;
    dchid_init_info info;

    memset(ev, 0, sizeof(ev));
    ev[0] = DCHID_EVENT_INIT;
    ev[3] = 6;
    memcpy(ev + 4, "keyboard", 8);
    ev[20] = 0; /* no more packets */
    n = DCHID_INIT_HDR_SIZE;

    memset(gpio, 0, sizeof(gpio));
    wr16(gpio + 2, 0x0102);
    memcpy(gpio + 4, "stm-reset", 9);

    n += put_block(ev + n, DCHID_INIT_HID_DESCRIPTOR, desc, sizeof(desc));
    n += put_block(ev + n, DCHID_INIT_GPIO_REQUEST, gpio, sizeof(gpio));
    n += put_block(ev + n, DCHID_INIT_PRODUCT_NAME, product, sizeof(product));
    n += put_block(ev + n, 0x55, junk, sizeof(junk));
    n += put_block(ev + n, DCHID_INIT_TERMINATOR, junk, 0);
    /* Anything after the terminator must be ignored. */
    n += put_block(ev + n, DCHID_INIT_HID_DESCRIPTOR, junk, sizeof(junk));

    CHECK(dchid_parse_init(ev, n, &info) == 0);
    CHECK(info.iface == 6);
    CHECK(strcmp(info.name, "keyboard") == 0);
    CHECK(info.more_packets == 0);
    CHECK(info.hid_desc_len == sizeof(desc));
    CHECK(info.hid_desc && memcmp(info.hid_desc, desc, sizeof(desc)) == 0);
    CHECK(info.has_gpio == 1);
    CHECK(info.gpio_id == 0x0102);
    CHECK(strcmp(info.gpio_name, "stm-reset") == 0);
    CHECK(info.product_name_len == sizeof(product));
    CHECK(info.unknown_blocks == 1);

    /* A 16-character name with no NUL must still come back terminated. */
    memcpy(ev + 4, "0123456789abcdef", 16);
    ev[20] = 1;
    CHECK(dchid_parse_init(ev, DCHID_INIT_HDR_SIZE, &info) == 0);
    CHECK(strlen(info.name) == 16);
    CHECK(info.more_packets == 1);
    CHECK(info.hid_desc == NULL);

    /* A block longer than the packet stops the walk. */
    n = DCHID_INIT_HDR_SIZE;
    wr16(ev + n, DCHID_INIT_HID_DESCRIPTOR);
    wr16(ev + n + 2, 100);
    CHECK(dchid_parse_init(ev, n + 8, &info) == 0);
    CHECK(info.hid_desc == NULL);

    CHECK(dchid_parse_init(ev, DCHID_INIT_HDR_SIZE - 1, &info) == -1);
}

static void test_small_events(void)
{
    uint8_t ready[2] = {DCHID_EVENT_READY, 5};
    uint8_t gcmd[5] = {DCHID_EVENT_GPIO_CMD, 2, 7, 0, DCHID_GPIO_CMD_PULSE};
    uint8_t ack[16];
    uint8_t stm[DCHID_STM_ID_SIZE] = {0, 0xac, 0x05, 0x41, 0x03, 0x10, 0x02,
                                      0, 0, 0x2a, 12};
    uint8_t iface = 0;
    dchid_gpio_cmd gc;
    dchid_stm_id id;

    CHECK(dchid_parse_ready(ready, 2, &iface) == 0 && iface == 5);
    CHECK(dchid_parse_ready(ready, 1, &iface) == -1);

    CHECK(dchid_parse_gpio_cmd(gcmd, 5, &gc) == 0);
    CHECK(gc.iface == 2 && gc.gpio == 7 && gc.cmd == DCHID_GPIO_CMD_PULSE);
    CHECK(dchid_parse_gpio_cmd(gcmd, 4, &gc) == -1);

    CHECK(dchid_build_gpio_ack(ack, sizeof(ack), DCHID_GPIO_RET_ERROR, gcmd, 5) == 10);
    CHECK(ack[0] == 0xa1);
    CHECK(rd32(ack + 1) == 0xe000f00du);
    CHECK(memcmp(ack + 5, gcmd, 5) == 0);
    CHECK(dchid_build_gpio_ack(ack, 9, 0, gcmd, 5) == 0);

    CHECK(dchid_parse_stm_id(stm, sizeof(stm), &id) == 0);
    CHECK(id.vendor_id == 0x05ac && id.product_id == 0x0341 && id.version == 0x0210);
    CHECK(id.keyboard_type == 0x2a);
    CHECK(dchid_parse_stm_id(stm, sizeof(stm) - 1, &id) == -1);
}

int main(void)
{
    test_build_frame();
    test_rx_basic();
    test_rx_stream();
    test_rx_errors();
    test_ack();
    test_init();
    test_small_events();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
