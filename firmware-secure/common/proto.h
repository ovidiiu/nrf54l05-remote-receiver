/*
 * Over-the-air protocol shared by remote and receiver.
 *
 * Every packet (22 bytes payload):
 *   [0]      type      PKT_BUTTONS or PKT_PAIR
 *   [1..8]   remote ID (FICR DEVICEID, 64 bit, little endian)
 *   [9..12]  counter   (32 bit, little endian, strictly increasing per remote)
 *   [13]     buttons   bit0 = BTN1 -> OUT1, bit1 = BTN2 -> OUT2
 *   [14..21] MAC       first 8 bytes of AES-CMAC(remote_key, bytes 0..13)
 *
 * remote_key = AES128(MASTER_SECRET, ID || "RMTKEY01")
 *   -> the receiver can derive it from the ID it sees during pairing; a packet
 *      only verifies if the sender knows MASTER_SECRET (i.e. runs this firmware).
 * Replay protection: receiver only accepts counter > last accepted counter.
 */
#ifndef PROTO_H
#define PROTO_H
#include <stdint.h>
#include <string.h>
#include "crypto.h"
#include "secret.h"

#define PKT_BUTTONS     0x01
#define PKT_PAIR        0x02
#define PKT_LEN         22
#define PKT_MAC_OFS     14
#define PKT_MAC_LEN     8

/* Radio settings (both sides must match) */
#define RF_CHANNEL      77          /* 2477 MHz: between Wi-Fi 11 and BLE adv ch 39 */
#define RF_BASE0        0xA5C35A3Cu /* logical address 0: normal packets */
#define RF_PREFIX0      0xE7u
#define RF_BASE1        0x3C5AA5C3u /* logical address 1: pairing packets */
#define RF_PREFIX1      0x96u
#define RF_ADDR_DATA    0
#define RF_ADDR_PAIR    1

typedef struct {
    uint8_t  type;
    uint64_t id;
    uint32_t counter;
    uint8_t  buttons;
} pkt_t;

static inline void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static inline uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }

static inline void derive_key(uint64_t id, uint8_t key[16])
{
    uint8_t blk[16];
    put32(blk, (uint32_t)id);
    put32(blk + 4, (uint32_t)(id >> 32));
    memcpy(blk + 8, "RMTKEY01", 8);
    aes128_encrypt(MASTER_SECRET, blk, key);
}

static inline void pkt_build(const pkt_t *p, const uint8_t key[16], uint8_t out[PKT_LEN])
{
    uint8_t mac[16];
    out[0] = p->type;
    put32(out + 1, (uint32_t)p->id);
    put32(out + 5, (uint32_t)(p->id >> 32));
    put32(out + 9, p->counter);
    out[13] = p->buttons;
    aes_cmac(key, out, PKT_MAC_OFS, mac);
    memcpy(out + PKT_MAC_OFS, mac, PKT_MAC_LEN);
}

/* Parse header only (no authentication yet). Returns 0 if malformed. */
static inline int pkt_parse(const uint8_t *in, uint8_t len, pkt_t *p)
{
    if (len != PKT_LEN) return 0;
    if (in[0] != PKT_BUTTONS && in[0] != PKT_PAIR) return 0;
    p->type = in[0];
    p->id = (uint64_t)get32(in + 1) | ((uint64_t)get32(in + 5) << 32);
    p->counter = get32(in + 9);
    p->buttons = in[13] & 0x03;
    return 1;
}

static inline int pkt_verify(const uint8_t *in, const uint8_t key[16])
{
    uint8_t mac[16];
    aes_cmac(key, in, PKT_MAC_OFS, mac);
    return ct_equal(mac, in + PKT_MAC_OFS, PKT_MAC_LEN);
}

#endif
