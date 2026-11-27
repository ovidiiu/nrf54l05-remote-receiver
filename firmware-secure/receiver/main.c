/*
 * RECEIVER (3.3 V module, nRF54L05)
 *
 *  - Radio always listening.
 *  - Valid packet from a paired remote (good MAC, counter newer than the last
 *    one accepted) sets OUT1 (P1.05) / OUT2 (P1.06) to the remote's buttons.
 *  - Outputs drop low on a "released" packet, or 300 ms after the last packet
 *    (signal lost / remote out of range).
 *  - Hold SW30 (P1.09) 3 s, release: pairing mode for 30 s (LED blinks).
 *    The first remote that sends a pairing burst is stored; LED on 2 s = done.
 *  - Hold SW30 10 s: forget all remotes (LED flashes fast, then off).
 *  - LED is on while any output is active.
 *
 * Up to MAX_REMOTES remotes. Last accepted counter per remote is saved in RRAM
 * at the end of every press, so replays stay blocked across power cycles.
 */
#include "hw.h"
#include "proto.h"

#define MAX_REMOTES        4
#define HOLD_TIMEOUT_MS  300
#define PAIR_WINDOW_MS 30000
#define BTN_PAIR_MS     3000
#define BTN_CLEAR_MS   10000
#define SLOT_LOG_LINES    64u            /* 64 lines (1 KB) per remote */
#define PAIR_MAGIC  0x50414952u          /* "PAIR" */

typedef struct {
    int      used;
    uint64_t id;
    uint8_t  key[16];
    uint32_t last_ctr;       /* last accepted counter */
    uint32_t saved_ctr;      /* last counter written to RRAM */
} remote_t;

static remote_t g_rem[MAX_REMOTES];

/* Debug counters, read with OpenOCD: mdw <&g_dbg> 12 (address printed in README) */
typedef struct {
    uint32_t magic;          /* 0xDB600001 */
    uint32_t rx_crc_ok;      /* packets with good CRC */
    uint32_t rx_addr0;       /* on data address  */
    uint32_t rx_addr1;       /* on pair address  */
    uint32_t rej_parse;      /* wrong length/type */
    uint32_t rej_addr;       /* type/address mismatch or pair pkt outside pair mode */
    uint32_t rej_unpaired;   /* ID not in table */
    uint32_t rej_counter;    /* counter not newer (replay/old) */
    uint32_t rej_mac;        /* MAC wrong */
    uint32_t accepted;       /* button packets accepted */
    uint32_t last_len;
    uint8_t  last_pkt[24];
    int32_t  last_rssi;      /* dBm */
    uint32_t hfxo_status;    /* bit0 started, bit1 tuned, bit2 failed, bit3 manual tune, bit4 timeout */
    uint32_t press_first_ctr;/* counter of first accepted packet of the last press */
    uint32_t press_last_ctr; /* counter of last accepted packet of the last press  */
    uint32_t press_rx;       /* packets accepted during the last press */
    int32_t  rssi_min;       /* weakest good packet seen (dBm) */
    int32_t  rssi_max;       /* strongest good packet seen (dBm) */
} dbg_t;
__attribute__((used)) volatile dbg_t g_dbg = { .magic = 0xDB600001u, .rssi_min = 0, .rssi_max = -127 };
static uint8_t  g_outputs;
static uint32_t g_t_last_pkt;
static int      g_active_slot = -1;

static uint32_t slot_log(int s) { return NV_LOG_BASE + (uint32_t)s * SLOT_LOG_LINES * 16u; }
static uint32_t slot_pair(int s) { return NV_PAIR_BASE + (uint32_t)s * 16u; }

static void load_pairings(void)
{
    for (int s = 0; s < MAX_REMOTES; s++) {
        const volatile uint32_t *r = (const volatile uint32_t *)slot_pair(s);
        remote_t *m = &g_rem[s];
        m->used = (r[0] == PAIR_MAGIC) && (r[3] == ~(r[0] ^ r[1] ^ r[2]));
        if (!m->used) continue;
        m->id = (uint64_t)r[1] | ((uint64_t)r[2] << 32);
        derive_key(m->id, m->key);
        uint32_t c = 0;
        nvlog_read(slot_log(s), SLOT_LOG_LINES, &c);
        m->last_ctr = m->saved_ctr = c;
    }
}

static void store_pairing(int s, uint64_t id, uint32_t ctr)
{
    uint32_t w[4] = { PAIR_MAGIC, (uint32_t)id, (uint32_t)(id >> 32), 0 };
    w[3] = ~(w[0] ^ w[1] ^ w[2]);
    rram_write_line(slot_pair(s), w);
    nvlog_write(slot_log(s), SLOT_LOG_LINES, ctr);
    remote_t *m = &g_rem[s];
    m->used = 1; m->id = id; derive_key(id, m->key);
    m->last_ctr = m->saved_ctr = ctr;
}

static void clear_pairings(void)
{
    const uint32_t zero[4] = { 0, 0, 0, 0 };
    for (int s = 0; s < MAX_REMOTES; s++) {
        rram_write_line(slot_pair(s), zero);
        g_rem[s].used = 0;
    }
}

static int find_slot(uint64_t id)
{
    for (int s = 0; s < MAX_REMOTES; s++) if (g_rem[s].used && g_rem[s].id == id) return s;
    return -1;
}

static void set_outputs(uint8_t b)
{
    g_outputs = b & 3u;
    gpio_set(PIN_OUT1, g_outputs & 1u);
    gpio_set(PIN_OUT2, (g_outputs >> 1) & 1u);
    led(g_outputs != 0);
}

/* end of a press: outputs low + persist counter (one RRAM write per press) */
static void release_all(void)
{
    set_outputs(0);
    if (g_active_slot >= 0) {
        remote_t *m = &g_rem[g_active_slot];
        if (m->last_ctr != m->saved_ctr) {
            nvlog_write(slot_log(g_active_slot), SLOT_LOG_LINES, m->last_ctr);
            m->saved_ctr = m->last_ctr;
        }
    }
    g_active_slot = -1;
}

static int counter_is_newer(uint32_t c, uint32_t last)
{
    uint32_t d = c - last;
    return d != 0 && d < 0x40000000u;
}

/* returns 1 if a remote was paired */
static int handle_packet(const uint8_t *buf, int len, uint32_t addr, int pairing)
{
    pkt_t p;
    g_dbg.rx_crc_ok++;
    g_dbg.last_rssi = radio_last_rssi();
    if (g_dbg.last_rssi < g_dbg.rssi_min) g_dbg.rssi_min = g_dbg.last_rssi;
    if (g_dbg.last_rssi > g_dbg.rssi_max) g_dbg.rssi_max = g_dbg.last_rssi;
    if (addr == 0) g_dbg.rx_addr0++; else if (addr == 1) g_dbg.rx_addr1++;
    g_dbg.last_len = (uint32_t)len;
    for (int i = 0; i < len && i < 24; i++) g_dbg.last_pkt[i] = buf[i];
    if (!pkt_parse(buf, (uint8_t)len, &p)) { g_dbg.rej_parse++; return 0; }

    if (p.type == PKT_PAIR) {
        /* Don't trust RXMATCH (nRF54L reports address 1 for most packets);
           the type byte + MAC identify the packet. */
        if (!pairing) { g_dbg.rej_addr++; return 0; }
        uint8_t key[16];
        derive_key(p.id, key);
        if (!pkt_verify(buf, key)) { g_dbg.rej_mac++; return 0; }   /* not our firmware */
        int s = find_slot(p.id);
        if (s < 0) for (s = 0; s < MAX_REMOTES && g_rem[s].used; s++) { }
        if (s >= MAX_REMOTES) s = 0;                   /* full: overwrite slot 0 */
        store_pairing(s, p.id, p.counter);
        return 1;
    }

    int s = find_slot(p.id);
    if (s < 0) { g_dbg.rej_unpaired++; return 0; }               /* not paired */
    remote_t *m = &g_rem[s];
    if (!counter_is_newer(p.counter, m->last_ctr)) { g_dbg.rej_counter++; return 0; } /* replay/old */
    if (!pkt_verify(buf, m->key)) { g_dbg.rej_mac++; return 0; } /* forged */
    m->last_ctr = p.counter;
    g_dbg.accepted++;
    if (g_active_slot < 0) { g_dbg.press_first_ctr = p.counter; g_dbg.press_rx = 0; }
    g_dbg.press_last_ctr = p.counter;
    g_dbg.press_rx++;

    if (g_active_slot >= 0 && g_active_slot != s && g_outputs) return 0; /* another remote owns outputs */
    g_active_slot = s;
    g_t_last_pkt = millis();
    if (p.buttons == 0) { if (g_outputs) release_all(); }   /* repeats of "released" are no-ops */
    else set_outputs(p.buttons);
    return 0;
}

int main(void)
{
    (void)reset_reason_and_clear();
    gpio_output(PIN_OUT1, 0);
    gpio_output(PIN_OUT2, 0);
    gpio_input_pullup(PIN_BTN1, 0);
    hw_init();

    load_pairings();

    /* power-up: 2 blinks */
    for (int i = 0; i < 2; i++) { led(1); delay_ms(100); led(0); delay_ms(100); }

    radio_on();
    g_dbg.hfxo_status = g_hfxo_status;
    radio_rx_start();

    uint8_t  buf[32];
    uint32_t addr;
    int      pairing = 0;
    uint32_t t_pair_start = 0, t_btn_down = 0, t_ok_led = 0;
    int      btn_prev = 0;

    for (;;) {
        uint32_t now = millis();

        int len = radio_rx_poll(buf, sizeof buf, &addr);
        if (len > 0 && handle_packet(buf, len, addr, pairing)) {
            pairing = 0;
            release_all();
            led(1); t_ok_led = now ? now : 1;      /* LED on 2 s = paired */
        }

        /* signal lost -> outputs off */
        if (g_outputs && (uint32_t)(now - g_t_last_pkt) > HOLD_TIMEOUT_MS) release_all();

        /* pairing button SW30 */
        int btn = !gpio_read(PIN_BTN1);
        if (btn && !btn_prev) t_btn_down = now;
        uint32_t held = btn ? (uint32_t)(now - t_btn_down) : 0;
        if (btn) {
            if (held >= BTN_CLEAR_MS)      led((now / 50u) & 1u);     /* fast flash: will clear */
            else if (held >= BTN_PAIR_MS)  led(1);                    /* solid: will pair */
        }
        if (!btn && btn_prev) {
            uint32_t h = (uint32_t)(now - t_btn_down);
            if (h >= BTN_CLEAR_MS) { clear_pairings(); pairing = 0; led(0); }
            else if (h >= BTN_PAIR_MS) { pairing = 1; t_pair_start = now; }
            else if (!g_outputs) led(0);
        }
        btn_prev = btn;

        if (pairing) {
            led((now / 250u) & 1u);
            if ((uint32_t)(now - t_pair_start) > PAIR_WINDOW_MS) { pairing = 0; led(0); }
        }
        if (t_ok_led && (uint32_t)(now - t_ok_led) > 2000) { t_ok_led = 0; led(g_outputs != 0); }
    }
}
