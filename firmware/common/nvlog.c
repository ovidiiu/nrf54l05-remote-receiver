/* Wear-levelled rotating log of 32-bit values in RRAM (one 16-byte line per record). */
#include <stdint.h>
#include "nvlog.h"

/* record: w0 = magic, w1 = sequence, w2 = value, w3 = ~(w0 ^ w1 ^ w2) */
#define NVLOG_MAGIC 0x4C4F4731u   /* "LOG1" */

static int nvrec_valid(const volatile uint32_t *r)
{
    return r[0] == NVLOG_MAGIC && r[3] == ~(r[0] ^ r[1] ^ r[2]);
}

static int nvlog_latest(uintptr_t base, uint32_t lines, uint32_t *idx)
{
    int found = 0; uint32_t best = 0;
    for (uint32_t i = 0; i < lines; i++) {
        const volatile uint32_t *r = (const volatile uint32_t *)(base + 16u * i);
        if (nvrec_valid(r) && (!found || (int32_t)(r[1] - best) > 0)) {
            best = r[1]; *idx = i; found = 1;
        }
    }
    return found;
}

int nvlog_read(uintptr_t base, uint32_t lines, uint32_t *value)
{
    uint32_t idx;
    if (!nvlog_latest(base, lines, &idx)) return 0;
    *value = ((const volatile uint32_t *)(base + 16u * idx))[2];
    return 1;
}

void nvlog_write(uintptr_t base, uint32_t lines, uint32_t value)
{
    uint32_t idx = lines - 1, seq = 0;
    if (nvlog_latest(base, lines, &idx))
        seq = ((const volatile uint32_t *)(base + 16u * idx))[1] + 1;
    idx = (idx + 1) % lines;
    uint32_t w[4] = { NVLOG_MAGIC, seq, value, 0 };
    w[3] = ~(w[0] ^ w[1] ^ w[2]);
    rram_write_line(base + 16u * idx, w);
}

