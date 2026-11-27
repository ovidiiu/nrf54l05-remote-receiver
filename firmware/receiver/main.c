/*
 * RECEIVER (3.3 V module, nRF54L05) - simple version, no encryption
 *
 *  - Radio always listening.
 *  - Packet from a paired remote sets OUT1 (P1.05) / OUT2 (P1.06) to the
 *    remote's buttons. Outputs drop on a "released" packet, or 300 ms after
 *    the last packet (signal lost).
 *  - Hold SW30 (P1.09) 3 s, release: pairing mode 30 s (LED blinks).
 *    First remote that sends a pairing burst is stored; LED on 2 s = done.
 *  - Hold SW30 10 s: forget all remotes.
 *  - LED is on while any output is active.
 */
#include "hw.h"
#include "proto.h"

#define MAX_REMOTES        4
#define HOLD_TIMEOUT_MS  300
#define PAIR_WINDOW_MS 30000
#define BTN_PAIR_MS     3000
#define BTN_CLEAR_MS   10000
#define PAIR_MAGIC  0x50414952u          /* "PAIR" */

typedef struct { int used; uint32_t id; } remote_t;
static remote_t g_rem[MAX_REMOTES];
static uint8_t  g_outputs;
static uint32_t g_t_last_pkt;
static int      g_active_slot = -1;

/* Debug counters, read with OpenOCD: mdw 0x20000004 17 */
typedef struct {
    uint32_t magic;          /* 0xDB600002 = simple protocol */
    uint32_t rx_crc_ok;      /* packets with good CRC */
    uint32_t rej_parse;      /* wrong length/type */
    uint32_t rej_pair;       /* pairing packet outside pairing mode */
    uint32_t rej_unpaired;   /* ID not in table */
    uint32_t accepted;       /* button packets accepted (incl. duplicates) */
    int32_t  last_rssi;      /* dBm */
    int32_t  rssi_min;
    int32_t  rssi_max;
    uint32_t hfxo_status;    /* 3 = started + tuned */
    uint32_t press_rx;       /* packets accepted in the last press (2 per 20 ms if none lost) */
    uint32_t press_ms;       /* duration of the last press as seen by the receiver */
    uint32_t presses;        /* number of presses seen */
    uint32_t last_len;
    uint8_t  last_pkt[8];
    uint32_t last_id;
} dbg_t;
__attribute__((used)) volatile dbg_t g_dbg = { .magic = 0xDB600002u, .rssi_min = 0, .rssi_max = -127 };
static uint32_t g_press_start;

static uint32_t slot_pair(int s) { return NV_PAIR_BASE + (uint32_t)s * 16u; }

/* Pairing table format kept from the secure version: {magic, id_lo, id_hi, check}.
   Only id_lo is used, so remotes paired with the old firmware stay paired. */
static void load_pairings(void)
{
    for (int s = 0; s < MAX_REMOTES; s++) {
        const volatile uint32_t *r = (const volatile uint32_t *)slot_pair(s);
        g_rem[s].used = (r[0] == PAIR_MAGIC) && (r[3] == ~(r[0] ^ r[1] ^ r[2]));
        g_rem[s].id = r[1];
    }
}

static void store_pairing(int s, uint32_t id)
{
    uint32_t w[4] = { PAIR_MAGIC, id, 0, 0 };
    w[3] = ~(w[0] ^ w[1] ^ w[2]);
    rram_write_line(slot_pair(s), w);
    g_rem[s].used = 1; g_rem[s].id = id;
}

static void clear_pairings(void)
{
    const uint32_t zero[4] = { 0, 0, 0, 0 };
    for (int s = 0; s < MAX_REMOTES; s++) { rram_write_line(slot_pair(s), zero); g_rem[s].used = 0; }
}

static int find_slot(uint32_t id)
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

static void release_all(void)
{
    if (g_outputs) g_dbg.press_ms = millis() - g_press_start;
    set_outputs(0);
    g_active_slot = -1;
}

/* returns 1 if a remote was paired */
static int handle_packet(const uint8_t *buf, int len, int pairing)
{
    pkt_t p;
    g_dbg.rx_crc_ok++;
    g_dbg.last_rssi = radio_last_rssi();
    if (g_dbg.last_rssi < g_dbg.rssi_min) g_dbg.rssi_min = g_dbg.last_rssi;
    if (g_dbg.last_rssi > g_dbg.rssi_max) g_dbg.rssi_max = g_dbg.last_rssi;
    g_dbg.last_len = (uint32_t)len;
    for (int i = 0; i < len && i < 8; i++) g_dbg.last_pkt[i] = buf[i];
    if (!pkt_parse(buf, (uint8_t)len, &p)) { g_dbg.rej_parse++; return 0; }
    g_dbg.last_id = p.id;

    if (p.type == PKT_PAIR) {
        if (!pairing) { g_dbg.rej_pair++; return 0; }
        int s = find_slot(p.id);
        if (s < 0) for (s = 0; s < MAX_REMOTES && g_rem[s].used; s++) { }
        if (s >= MAX_REMOTES) s = 0;               /* full: overwrite slot 0 */
        store_pairing(s, p.id);
        return 1;
    }

    int s = find_slot(p.id);
    if (s < 0) { g_dbg.rej_unpaired++; return 0; }
    if (g_active_slot >= 0 && g_active_slot != s && g_outputs) return 0; /* another remote owns outputs */
    g_dbg.accepted++;
    g_t_last_pkt = millis();

    if (p.buttons == 0) { if (g_outputs) release_all(); return 0; }
    if (!g_outputs) { g_press_start = g_t_last_pkt; g_dbg.press_rx = 0; g_dbg.presses++; }
    g_dbg.press_rx++;
    g_active_slot = s;
    set_outputs(p.buttons);
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

    for (int i = 0; i < 2; i++) { led(1); delay_ms(100); led(0); delay_ms(100); }

    radio_on();
    g_dbg.hfxo_status = g_hfxo_status;
    radio_rx_start();

    uint8_t  buf[32];
    uint32_t addr;
    int      pairing = 0, btn_prev = 0;
    uint32_t t_pair_start = 0, t_btn_down = 0, t_ok_led = 0;

    for (;;) {
        uint32_t now = millis();

        int len = radio_rx_poll(buf, sizeof buf, &addr);
        if (len > 0 && handle_packet(buf, len, pairing)) {
            pairing = 0;
            release_all();
            led(1); t_ok_led = now ? now : 1;
        }

        if (g_outputs && (uint32_t)(now - g_t_last_pkt) > HOLD_TIMEOUT_MS) release_all();

        int btn = !gpio_read(PIN_BTN1);
        if (btn && !btn_prev) t_btn_down = now;
        uint32_t held = btn ? (uint32_t)(now - t_btn_down) : 0;
        if (btn) {
            if (held >= BTN_CLEAR_MS)      led((now / 50u) & 1u);
            else if (held >= BTN_PAIR_MS)  led(1);
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
