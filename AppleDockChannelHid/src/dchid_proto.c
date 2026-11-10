// SPDX-License-Identifier: MIT
/*
 * dchid_proto.c - Apple DockChannel HID wire protocol, OS independent.
 * See dchid_proto.h. No OS headers on purpose; the host unit test builds
 * this file unchanged.
 */
#include "dchid_proto.h"

static uint16_t ld16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void st16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t n)
{
    while (n--)
        *dst++ = *src++;
}

/* Copy up to max bytes of a possibly unterminated string; dst has max+1. */
static void copy_name(char *dst, const uint8_t *src, size_t max)
{
    size_t i;

    for (i = 0; i < max && src[i] != 0; i++)
        dst[i] = (char)src[i];
    dst[i] = 0;
}

uint32_t dchid_sum_words(const uint8_t *p, size_t len)
{
    uint32_t sum = 0;

    while (len >= 4) {
        sum += ld32(p);
        p += 4;
        len -= 4;
    }
    return sum;
}

size_t dchid_frame_size(size_t data_len)
{
    return DCHID_HDR_SIZE + DCHID_SUBHDR_SIZE + ((data_len + 3u) & ~(size_t)3u) +
           DCHID_CSUM_SIZE;
}

size_t dchid_build_frame(uint8_t *out, size_t cap, uint8_t iface, uint8_t seq,
                         uint8_t flags, const uint8_t *data, size_t data_len)
{
    size_t padded = (data_len + 3u) & ~(size_t)3u;
    size_t total = dchid_frame_size(data_len);
    size_t body = DCHID_SUBHDR_SIZE + padded;
    size_t i;
    uint8_t *p = out;

    if (cap < total || body > 0xffffu)
        return 0;

    /* Header [H:279-284] */
    p[0] = (uint8_t)DCHID_HDR_SIZE;
    p[1] = (uint8_t)DCHID_CHANNEL_CMD;
    st16(p + 2, (uint16_t)body);
    p[4] = seq;
    p[5] = iface;
    st16(p + 6, 0);
    p += DCHID_HDR_SIZE;

    /* Sub-header: retcode is zero on requests [H:285-286] */
    p[0] = flags;
    p[1] = 0;
    st16(p + 2, (uint16_t)data_len);
    st32(p + 4, 0);
    p += DCHID_SUBHDR_SIZE;

    copy_bytes(p, data, data_len);
    for (i = data_len; i < padded; i++)
        p[i] = 0;
    p += padded;

    /* Checksum makes the word sum of the whole frame 0xffffffff. */
    st32(p, DCHID_CSUM_TARGET - dchid_sum_words(out, total - DCHID_CSUM_SIZE));
    return total;
}

int dchid_rx_init(dchid_rx *rx, uint8_t *body, size_t body_cap)
{
    if (body_cap < DCHID_MAX_BODY)
        return -1;
    rx->body = body;
    dchid_rx_reset(rx);
    return 0;
}

void dchid_rx_reset(dchid_rx *rx)
{
    rx->hdr_have = 0;
    rx->in_body = 0;
    rx->body_have = 0;
    rx->body_need = 0;
}

size_t dchid_rx_wanted(const dchid_rx *rx)
{
    if (rx->in_body)
        return rx->body_need - rx->body_have;
    return DCHID_HDR_SIZE - rx->hdr_have;
}

dchid_rx_result dchid_rx_feed(dchid_rx *rx, const uint8_t *data, size_t len,
                              size_t *consumed)
{
    size_t used = 0;

    while (used < len) {
        size_t n;

        if (!rx->in_body) {
            n = DCHID_HDR_SIZE - rx->hdr_have;
            if (n > len - used)
                n = len - used;
            copy_bytes(rx->hdr_raw + rx->hdr_have, data + used, n);
            rx->hdr_have += n;
            used += n;
            if (rx->hdr_have < DCHID_HDR_SIZE)
                break;

            rx->hdr.hdr_len = rx->hdr_raw[0];
            rx->hdr.channel = rx->hdr_raw[1];
            rx->hdr.length = ld16(rx->hdr_raw + 2);
            rx->hdr.seq = rx->hdr_raw[4];
            rx->hdr.iface = rx->hdr_raw[5];
            rx->hdr_have = 0;

            /* The reference drops only the 8 header bytes here [H:1034-1036]. */
            if (rx->hdr.hdr_len != DCHID_HDR_SIZE) {
                *consumed = used;
                return DCHID_RX_BAD_HDR_LEN;
            }
            rx->in_body = 1;
            rx->body_have = 0;
            rx->body_need = (size_t)rx->hdr.length + DCHID_CSUM_SIZE;
            continue;
        }

        n = rx->body_need - rx->body_have;
        if (n > len - used)
            n = len - used;
        copy_bytes(rx->body + rx->body_have, data + used, n);
        rx->body_have += n;
        used += n;
        if (rx->body_have < rx->body_need)
            break;

        rx->in_body = 0;
        *consumed = used;
        /*
         * Like the reference, sum header + body + checksum with a word sum
         * [H:1044-1045]. A length that is not a multiple of 4 leaves tail
         * bytes out of the sum; the reference has the same behaviour and the
         * device is documented to send multiples of 4 [H:34-41].
         */
        if (dchid_sum_words(rx->hdr_raw, DCHID_HDR_SIZE) +
                dchid_sum_words(rx->body, rx->body_need) != DCHID_CSUM_TARGET)
            return DCHID_RX_BAD_CHECKSUM;
        return DCHID_RX_PACKET;
    }

    *consumed = used;
    return DCHID_RX_NEED_MORE;
}

int dchid_decode_msg(const dchid_hdr *hdr, const uint8_t *body, dchid_msg *msg)
{
    if (hdr->length < DCHID_SUBHDR_SIZE)
        return -1;

    msg->sub.flags = body[0];
    msg->sub.length = ld16(body + 2);
    msg->sub.retcode = ld32(body + 4);
    msg->group = (uint8_t)(msg->sub.flags >> 6);

    /* [H:959-963] */
    if ((size_t)msg->sub.length + DCHID_SUBHDR_SIZE > hdr->length)
        return -1;

    msg->payload = body + DCHID_SUBHDR_SIZE;
    msg->payload_len = msg->sub.length;
    return 0;
}

dchid_ack_result dchid_match_ack(const dchid_pending *p, const dchid_hdr *hdr,
                                 const dchid_msg *msg)
{
    /* Checks in the reference's order [H:990-1010], plus the interface. */
    if (hdr->iface != p->iface)
        return DCHID_ACK_BAD_IFACE;
    if (msg->sub.flags != p->flags)
        return DCHID_ACK_BAD_FLAGS;
    if (msg->payload_len < 1)
        return DCHID_ACK_EMPTY;
    if (hdr->seq != p->seq)
        return DCHID_ACK_BAD_SEQ;
    if (msg->payload[0] != p->report_id)
        return DCHID_ACK_BAD_REPORT;
    return DCHID_ACK_MATCH;
}

int dchid_parse_init(const uint8_t *p, size_t len, dchid_init_info *out)
{
    int seen_gpio = 0;

    if (len < DCHID_INIT_HDR_SIZE)
        return -1;

    out->iface = p[3];
    copy_name(out->name, p + 4, DCHID_IFACE_NAME_LEN);
    out->more_packets = p[20];
    out->hid_desc = 0;
    out->hid_desc_len = 0;
    out->has_gpio = 0;
    out->gpio_id = 0;
    out->gpio_name[0] = 0;
    out->product_name = 0;
    out->product_name_len = 0;
    out->unknown_blocks = 0;

    p += DCHID_INIT_HDR_SIZE;
    len -= DCHID_INIT_HDR_SIZE;

    /* Block walk as in the reference [H:795-852]. */
    while (len >= 4) {
        uint16_t type = ld16(p);
        uint16_t blen = ld16(p + 2);

        p += 4;
        len -= 4;
        if (blen > len)
            break;

        switch (type) {
        case DCHID_INIT_HID_DESCRIPTOR:
            out->hid_desc = p;
            out->hid_desc_len = blen;
            break;
        case DCHID_INIT_GPIO_REQUEST:
            /*
             * The reference bounds the request by the remaining packet
             * length, not by the block length [H:811]; kept identical.
             * Only one GPIO per interface is supported there [H:814-818].
             */
            if (len >= DCHID_GPIO_REQ_SIZE && !seen_gpio) {
                seen_gpio = 1;
                out->has_gpio = 1;
                out->gpio_id = ld16(p + 2);
                copy_name(out->gpio_name, p + 4, DCHID_GPIO_NAME_LEN);
            }
            break;
        case DCHID_INIT_TERMINATOR:
            break;
        case DCHID_INIT_PRODUCT_NAME:
            out->product_name = p;
            out->product_name_len = blen;
            break;
        default:
            out->unknown_blocks++;
            break;
        }

        p += blen;
        len -= blen;
        if (type == DCHID_INIT_TERMINATOR)
            break;
    }
    return 0;
}

int dchid_parse_ready(const uint8_t *payload, size_t len, uint8_t *iface)
{
    if (len < 2)
        return -1;
    *iface = payload[1];
    return 0;
}

int dchid_parse_gpio_cmd(const uint8_t *payload, size_t len, dchid_gpio_cmd *out)
{
    if (len < DCHID_GPIO_CMD_SIZE)
        return -1;
    out->iface = payload[1];
    out->gpio = payload[2];
    out->cmd = payload[4];
    return 0;
}

size_t dchid_build_gpio_ack(uint8_t *out, size_t cap, uint32_t retcode,
                            const uint8_t *event, size_t event_len)
{
    size_t total = 5u + event_len;

    if (cap < total)
        return 0;
    out[0] = (uint8_t)DCHID_CMD_ACK_GPIO_CMD;
    st32(out + 1, retcode);
    copy_bytes(out + 5, event, event_len);
    return total;
}

size_t dchid_build_enable(uint8_t out[2], uint8_t iface)
{
    out[0] = (uint8_t)DCHID_CMD_ENABLE_INTERFACE;
    out[1] = iface;
    return 2;
}

int dchid_parse_stm_id(const uint8_t *resp, size_t len, dchid_stm_id *out)
{
    /* unk, vid, pid, version, unk2, unk3, keyboard_type, serial_length */
    if (len < DCHID_STM_ID_SIZE)
        return -1;
    out->vendor_id = ld16(resp + 1);
    out->product_id = ld16(resp + 3);
    out->version = ld16(resp + 5);
    out->keyboard_type = resp[9];
    return 0;
}
