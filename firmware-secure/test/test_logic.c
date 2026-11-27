/* Host tests for protocol + RRAM log + remote counter reservation. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "../common/proto.h"
#include "../common/nvlog.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

/* ---- mock RRAM: 4 KB filled with 0xFF like an erased chip ---- */
static uint32_t rram[1024];
static int writes, fail_next_write;
void rram_write_line(uintptr_t addr, const uint32_t w[4])
{
    uint32_t *p = (uint32_t *)addr;
    writes++;
    if (fail_next_write) { p[0] = w[0]; p[1] = 0xDEAD; fail_next_write = 0; return; } /* torn write */
    memcpy(p, w, 16);
}

/* ---- copy of the remote's counter logic (remote/main.c) ---- */
#define CTR_BLOCK 1024u
static uint32_t g_ctr, g_lim;
static void counter_init(void) {
    uint32_t s; if (!nvlog_read((uintptr_t)rram, 256, &s) || s == 0) s = 1;
    g_ctr = s; g_lim = s + CTR_BLOCK; nvlog_write((uintptr_t)rram, 256, g_lim);
}
static uint32_t counter_next(void) {
    if (g_ctr >= g_lim) { g_lim = g_ctr + CTR_BLOCK; nvlog_write((uintptr_t)rram, 256, g_lim); }
    return g_ctr++;
}
/* copy of receiver's check */
static int counter_is_newer(uint32_t c, uint32_t last) { uint32_t d = c - last; return d != 0 && d < 0x40000000u; }

int main(void)
{
    memset(rram, 0xFF, sizeof rram);
    uint32_t v;

    /* --- nvlog basics --- */
    CHECK(!nvlog_read((uintptr_t)rram, 256, &v), "empty log reads nothing");
    for (uint32_t i = 1; i <= 1000; i++) {           /* wraps the 256-line ring ~4 times */
        nvlog_write((uintptr_t)rram, 256, i * 7);
        if (!nvlog_read((uintptr_t)rram, 256, &v) || v != i * 7) { CHECK(0, "nvlog read-after-write"); break; }
    }
    fail_next_write = 1; nvlog_write((uintptr_t)rram, 256, 999999);    /* torn write */
    CHECK(nvlog_read((uintptr_t)rram, 256, &v) && v == 7000, "torn write falls back to previous value");

    /* --- remote counter never repeats across 'battery removals' --- */
    memset(rram, 0xFF, sizeof rram);
    uint32_t prev = 0; int ok = 1;
    srand(1);
    for (int wake = 0; wake < 3000; wake++) {
        counter_init();                               /* every wake / battery change */
        int pkts = rand() % 3000;                     /* some presses are long */
        for (int k = 0; k < pkts; k++) {
            uint32_t c = counter_next();
            if (c <= prev) { ok = 0; break; }
            prev = c;
        }
        if (!ok) break;
    }
    CHECK(ok, "remote counters strictly increase across reboots");
    printf("  3000 wakes: last counter %u, RRAM line writes %d\n", prev, writes);

    /* --- packet build / verify / tamper / replay --- */
    uint64_t id = 0x1122334455667788ull;
    uint8_t key[16], key_rx[16], buf[PKT_LEN];
    derive_key(id, key);
    derive_key(id, key_rx);
    CHECK(memcmp(key, key_rx, 16) == 0, "both sides derive same key");
    uint8_t key_other[16]; derive_key(id ^ 1, key_other);
    CHECK(memcmp(key, key_other, 16) != 0, "different remotes get different keys");

    pkt_t p = { PKT_BUTTONS, id, 5000, 2 }, q;
    pkt_build(&p, key, buf);
    CHECK(pkt_parse(buf, PKT_LEN, &q) && q.id == id && q.counter == 5000 && q.buttons == 2, "parse");
    CHECK(pkt_verify(buf, key_rx), "valid MAC accepted");
    for (int byte = 0; byte < PKT_LEN; byte++) {
        uint8_t t[PKT_LEN]; memcpy(t, buf, PKT_LEN); t[byte] ^= 0x01;
        pkt_t r;
        if (pkt_parse(t, PKT_LEN, &r) && pkt_verify(t, key_rx)) { CHECK(0, "tampered packet accepted"); break; }
    }
    CHECK(!pkt_verify(buf, key_other), "wrong key rejected");
    CHECK(!pkt_parse(buf, PKT_LEN - 1, &q), "wrong length rejected");

    CHECK(counter_is_newer(5001, 5000), "newer accepted");
    CHECK(!counter_is_newer(5000, 5000), "same counter (replay) rejected");
    CHECK(!counter_is_newer(4000, 5000), "older counter rejected");
    CHECK(counter_is_newer(5000 + 1024, 5000), "jump after battery change accepted");
    CHECK(counter_is_newer(3, 0xFFFFFFF0u), "wraparound handled");

    puts(fails ? "LOGIC TESTS FAILED" : "logic: all tests OK");
    return fails;
}
