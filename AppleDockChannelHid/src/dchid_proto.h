// SPDX-License-Identifier: MIT
/*
 * dchid_proto.h - Apple DockChannel HID (MTP) wire protocol, OS independent.
 *
 * This file and dchid_proto.c must not include any Windows or Linux header.
 * They are compiled into the kernel driver and into the host unit test
 * (test/test_dchid_proto.c), which is the only place this code has been run.
 *
 * Protocol facts come from the Asahi Linux dockchannel-hid driver, used as a
 * reference only (see README.md, "Licence"). Citations in the form [H:nn]
 * refer to lines of that file as recorded in the project's hardware notes.
 *
 * All multi-byte wire fields are little-endian. Structures here are decoded
 * host-side copies, not overlays on the wire bytes, so packing and alignment
 * never matter.
 */
#ifndef DCHID_PROTO_H
#define DCHID_PROTO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Framing [H:28-41] ------------------------------------------------- */

#define DCHID_HDR_SIZE      8u   /* hdr_len, channel, length(16), seq, iface, pad(16) */
#define DCHID_SUBHDR_SIZE   8u   /* flags, unk, length(16), retcode(32) */
#define DCHID_CSUM_SIZE     4u
/* Largest body the receiver may have to hold: u16 length + checksum [H:29]. */
#define DCHID_MAX_BODY      (0xffffu + DCHID_CSUM_SIZE)
/* Sum of all LE u32 words of a valid packet, checksum included [H:254-313]. */
#define DCHID_CSUM_TARGET   0xffffffffu

#define DCHID_CHANNEL_CMD    0x11u  /* host->device commands and their ACKs */
#define DCHID_CHANNEL_REPORT 0x12u  /* device->host events and input reports */

#define DCHID_MAX_INTERFACES 16u   /* [H:26] */
#define DCHID_IFACE_COMM     0u    /* control interface, not HID [H:43] */
#define DCHID_IFACE_NAME_LEN 16u   /* name[16] in the INIT header [H:62-70] */

/*
 * Sub-header flags: GROUP [7:6] carries the HID report type, REQ [5:0] the
 * request [H:45-49]. The report type numbers are Linux's enum
 * hid_report_type (INPUT 0, OUTPUT 1, FEATURE 2). That header was NOT among
 * the reference files; the values are from general knowledge of linux/hid.h
 * and are therefore unverified here (README, "Open questions").
 */
#define DCHID_TYPE_INPUT     0u
#define DCHID_TYPE_OUTPUT    1u
#define DCHID_TYPE_FEATURE   2u
#define DCHID_REQ_SET_REPORT 0u
#define DCHID_REQ_GET_REPORT 1u

/* ---- Events sent by the device on the comm interface [H:58-60] ---------- */

#define DCHID_EVENT_GPIO_CMD 0xa0u
#define DCHID_EVENT_INIT     0xf0u
#define DCHID_EVENT_READY    0xf1u

/* INIT block types [H:72-75] */
#define DCHID_INIT_HID_DESCRIPTOR 0u
#define DCHID_INIT_GPIO_REQUEST   1u
#define DCHID_INIT_TERMINATOR     2u
#define DCHID_INIT_PRODUCT_NAME   7u

#define DCHID_INIT_HDR_SIZE       22u  /* type,unk1,unk2,iface,name[16],more,pad */
#define DCHID_GPIO_REQ_SIZE       36u  /* unk(16), id(16), name[32] */
#define DCHID_GPIO_NAME_LEN       32u
#define DCHID_GPIO_CMD_SIZE       5u   /* type, iface, gpio, unk, cmd */
#define DCHID_GPIO_CMD_PULSE      3u   /* [H:891-898] */
/* Error code the Linux driver returns for a failed GPIO command [H:869]. */
#define DCHID_GPIO_RET_ERROR      0xe000f00du

/* ---- Commands sent on the comm interface [H:77-80] ---------------------- */

#define DCHID_CMD_ENABLE_INTERFACE 0xb4u
#define DCHID_CMD_ACK_GPIO_CMD     0xa1u

/* STM feature reports [H:109-123] */
#define DCHID_STM_REPORT_ID        0x10u
#define DCHID_STM_ID_SIZE          11u

/* ---- Decoded structures ------------------------------------------------- */

typedef struct dchid_hdr {
    uint8_t  hdr_len;
    uint8_t  channel;
    uint16_t length;   /* body length without checksum; multiple of 4 */
    uint8_t  seq;
    uint8_t  iface;
} dchid_hdr;

typedef struct dchid_subhdr {
    uint8_t  flags;
    uint16_t length;   /* bytes of payload actually used */
    uint32_t retcode;  /* 0 = success, in ACKs */
} dchid_subhdr;

/* A decoded packet body: sub-header plus the payload it describes. */
typedef struct dchid_msg {
    dchid_subhdr   sub;
    uint8_t        group;        /* report type, from sub.flags */
    const uint8_t *payload;      /* points into the receive buffer */
    size_t         payload_len;  /* == sub.length */
} dchid_msg;

static inline uint8_t dchid_flags(uint8_t group, uint8_t req)
{
    return (uint8_t)(((group & 0x3u) << 6) | (req & 0x3fu));
}

/* ---- Checksum and transmit framing -------------------------------------- */

/* Sum of little-endian u32 words; len must be a multiple of 4. */
uint32_t dchid_sum_words(const uint8_t *p, size_t len);

/* Size of a complete host->device frame carrying data_len payload bytes. */
size_t dchid_frame_size(size_t data_len);

/*
 * Build a complete command frame (header, sub-header, data padded with zeros
 * to 4 bytes, checksum) in out[]. data[0] is the report id by convention.
 * Returns the frame length, or 0 if cap is too small or data_len does not
 * fit the 16-bit length fields.
 */
size_t dchid_build_frame(uint8_t *out, size_t cap, uint8_t iface, uint8_t seq,
                         uint8_t flags, const uint8_t *data, size_t data_len);

/* ---- Receive state machine ---------------------------------------------- */

typedef enum dchid_rx_result {
    DCHID_RX_NEED_MORE = 0,   /* all input consumed, no packet complete yet */
    DCHID_RX_PACKET,          /* rx->hdr and rx->body hold one valid packet */
    DCHID_RX_BAD_HDR_LEN,     /* header dropped: hdr_len != 8 */
    DCHID_RX_BAD_CHECKSUM     /* whole packet dropped */
} dchid_rx_result;

typedef struct dchid_rx {
    uint8_t   hdr_raw[DCHID_HDR_SIZE];
    size_t    hdr_have;
    int       in_body;
    dchid_hdr hdr;
    uint8_t  *body;       /* caller-owned, >= DCHID_MAX_BODY bytes */
    size_t    body_have;
    size_t    body_need;  /* hdr.length + checksum */
} dchid_rx;

/* Returns 0, or -1 if body_cap < DCHID_MAX_BODY. */
int dchid_rx_init(dchid_rx *rx, uint8_t *body, size_t body_cap);
void dchid_rx_reset(dchid_rx *rx);

/* Bytes still needed to finish the current header or body (always >= 1). */
size_t dchid_rx_wanted(const dchid_rx *rx);

/*
 * Feed received bytes. Stops early after a packet or an error so the caller
 * can act on it before the body buffer is reused; *consumed says how much of
 * data[] was used. Errors resynchronise the same way the reference does:
 * the next byte is taken as the start of a new header (there is no magic to
 * scan for).
 */
dchid_rx_result dchid_rx_feed(dchid_rx *rx, const uint8_t *data, size_t len,
                              size_t *consumed);

/* Decode the body of a received packet. Returns 0, or -1 if malformed. */
int dchid_decode_msg(const dchid_hdr *hdr, const uint8_t *body, dchid_msg *msg);

/* ---- Command/ACK matching [H:980-1019] ---------------------------------- */

typedef struct dchid_pending {
    uint8_t iface;
    uint8_t seq;
    uint8_t flags;
    uint8_t report_id;
} dchid_pending;

typedef enum dchid_ack_result {
    DCHID_ACK_MATCH = 0,
    DCHID_ACK_BAD_IFACE,
    DCHID_ACK_BAD_FLAGS,
    DCHID_ACK_EMPTY,
    DCHID_ACK_BAD_SEQ,
    DCHID_ACK_BAD_REPORT
} dchid_ack_result;

/* On MATCH the response data is msg->payload + 1, msg->payload_len - 1. */
dchid_ack_result dchid_match_ack(const dchid_pending *p, const dchid_hdr *hdr,
                                 const dchid_msg *msg);

/* ---- Comm events -------------------------------------------------------- */

typedef struct dchid_init_info {
    uint8_t        iface;
    char           name[DCHID_IFACE_NAME_LEN + 1];   /* always NUL terminated */
    uint8_t        more_packets;
    const uint8_t *hid_desc;          /* NULL if this packet carried none */
    size_t         hid_desc_len;
    int            has_gpio;
    uint16_t       gpio_id;
    char           gpio_name[DCHID_GPIO_NAME_LEN + 1];
    const uint8_t *product_name;      /* raw bytes, may lack a NUL */
    size_t         product_name_len;
    unsigned       unknown_blocks;
} dchid_init_info;

/* payload starts with the 0xf0 type byte. Returns 0, or -1 if too short. */
int dchid_parse_init(const uint8_t *payload, size_t len, dchid_init_info *out);

/* READY: payload[1] = iface [H:732-737]. Returns 0, or -1 if too short. */
int dchid_parse_ready(const uint8_t *payload, size_t len, uint8_t *iface);

typedef struct dchid_gpio_cmd {
    uint8_t iface;
    uint8_t gpio;
    uint8_t cmd;
} dchid_gpio_cmd;

int dchid_parse_gpio_cmd(const uint8_t *payload, size_t len, dchid_gpio_cmd *out);

/*
 * GPIO ACK command: {0xa1, u32 retcode, original event bytes} [H:103-107,
 * H:904-917]. Returns the length written, or 0 if cap is too small.
 */
size_t dchid_build_gpio_ack(uint8_t *out, size_t cap, uint32_t retcode,
                            const uint8_t *event, size_t event_len);

/* ENABLE_INTERFACE command {0xb4, iface} [H:364-369]. Returns 2. */
size_t dchid_build_enable(uint8_t out[2], uint8_t iface);

typedef struct dchid_stm_id {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version;
    uint8_t  keyboard_type;
} dchid_stm_id;

/* Response of GET_REPORT feature 0x10 on "stm", report id already stripped. */
int dchid_parse_stm_id(const uint8_t *resp, size_t len, dchid_stm_id *out);

#ifdef __cplusplus
}
#endif

#endif /* DCHID_PROTO_H */
