/*
 * Over-the-air protocol (no encryption): 6-byte packet
 *   [0] type  PKT_BUTTONS / PKT_PAIR
 *   [1..4] remote ID (low 32 bits of FICR DEVICEID, little endian)
 *   [5] buttons  bit0 = BTN1 -> OUT1, bit1 = BTN2 -> OUT2
 * The 16-bit radio CRC protects against corruption; pairing makes the receiver
 * ignore other remotes. No protection against deliberate replay/spoofing.
 */
#ifndef PROTO_H
#define PROTO_H
#include <stdint.h>
#include <string.h>

#define PKT_BUTTONS     0x01
#define PKT_PAIR        0x02
#define PKT_LEN         6           /* type(1) + remote ID(4) + buttons(1) */

/* Radio settings (both sides must match) */
#define RF_CHANNEL      77          /* 2477 MHz: between Wi-Fi 11 and BLE adv ch 39 */
#define RF_BASE0        0xA5C35A3Cu /* logical address 0 (unused: poor reception in tests) */
#define RF_PREFIX0      0xE7u
#define RF_BASE1        0x3C5AA5C3u /* logical address 1: all packets */
#define RF_PREFIX1      0x96u
#define RF_ADDR_DATA    0
#define RF_ADDR_PAIR    1
#define RF_ADDR_TX      RF_ADDR_PAIR

typedef struct {
    uint8_t  type;
    uint32_t id;          /* low 32 bits of FICR DEVICEID */
    uint8_t  buttons;     /* bit0 = BTN1 -> OUT1, bit1 = BTN2 -> OUT2 */
} pkt_t;

static inline void pkt_build(const pkt_t *p, uint8_t out[PKT_LEN])
{
    out[0] = p->type;
    out[1] = (uint8_t)p->id; out[2] = (uint8_t)(p->id >> 8);
    out[3] = (uint8_t)(p->id >> 16); out[4] = (uint8_t)(p->id >> 24);
    out[5] = p->buttons;
}

static inline int pkt_parse(const uint8_t *in, uint8_t len, pkt_t *p)
{
    if (len != PKT_LEN) return 0;
    if (in[0] != PKT_BUTTONS && in[0] != PKT_PAIR) return 0;
    if (in[5] & ~0x03u) return 0;
    p->type = in[0];
    p->id = (uint32_t)in[1] | ((uint32_t)in[2] << 8) | ((uint32_t)in[3] << 16) | ((uint32_t)in[4] << 24);
    p->buttons = in[5];
    return 1;
}

#endif
