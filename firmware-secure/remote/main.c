/*
 * REMOTE (CR2032, nRF54L05)
 *
 *  - Sleeps in System OFF (~1 uA class) until a button pulls P1.09 / P1.10 low.
 *  - While exactly one button is held: sends an authenticated packet every 20 ms.
 *  - On release: sends 3 "all released" packets, then back to System OFF.
 *  - Both buttons held 3 s: pairing burst for 5 s (LED blinks fast).
 *  - Fresh battery inserted: one short LED blink, then sleep.
 *
 * Rolling counter: reserved in blocks of 1024 in an RRAM log before use,
 * so a counter value is never sent twice, even after a battery change.
 */
#include "hw.h"
#include "proto.h"

#define TX_PERIOD_MS        20
#define PAIR_HOLD_MS      3000
#define PAIR_BURST_MS     5000
#define PAIR_PERIOD_MS      50
#define STUCK_MS         30000   /* one button held this long = stuck, give up */
#define DEBOUNCE_MS          5
#define CTR_BLOCK         1024u

static uint8_t  g_key[16];
static uint64_t g_id;
static uint32_t g_ctr, g_ctr_limit;

static void counter_init(void)
{
    uint32_t start;
    if (!nvlog_read(NV_LOG_BASE, NV_LOG_LINES, &start) || start == 0) start = 1;
    g_ctr = start;
    g_ctr_limit = start + CTR_BLOCK;
    nvlog_write(NV_LOG_BASE, NV_LOG_LINES, g_ctr_limit);   /* reserve before first use */
}

static uint32_t counter_next(void)
{
    if (g_ctr >= g_ctr_limit) {
        g_ctr_limit = g_ctr + CTR_BLOCK;
        nvlog_write(NV_LOG_BASE, NV_LOG_LINES, g_ctr_limit);
    }
    return g_ctr++;
}

static void send(uint8_t type, uint8_t buttons)
{
    pkt_t p = { .type = type, .id = g_id, .counter = counter_next(), .buttons = buttons };
    uint8_t buf[PKT_LEN];
    pkt_build(&p, g_key, buf);
    /* Everything goes out on logical address 1: tests showed address 0 is almost
       never received, address 1 ~96 %. The receiver tells packets apart by type. */
    radio_tx(buf, PKT_LEN, RF_ADDR_PAIR);
    delay_us(300);
    radio_tx(buf, PKT_LEN, RF_ADDR_PAIR);   /* same packet twice: receiver drops the duplicate */
}

/* bit0 = BTN1 (P1.09), bit1 = BTN2 (P1.10); pressed = low */
static uint8_t buttons_raw(void)
{
    return (uint8_t)((!gpio_read(PIN_BTN1) ? 1u : 0u) | (!gpio_read(PIN_BTN2) ? 2u : 0u));
}

static uint8_t stable, last;
static uint32_t t_change;

static void buttons_debounce_init(void)
{
    stable = last = buttons_raw();
    t_change = millis();
}

static uint8_t buttons_debounced(void)
{
    uint8_t now = buttons_raw();
    if (now != last) { last = now; t_change = millis(); }
    if ((uint32_t)(millis() - t_change) >= DEBOUNCE_MS) stable = last;
    return stable;
}

static void go_to_sleep(uint8_t ignore_mask)
{
    radio_off();
    led(0);
    gpio_disconnect(PIN_LED);
    /* Wake on a low level. A stuck button is left without pull-up and sense,
       otherwise it would wake us immediately and drain the battery. */
    if (ignore_mask & 1u) gpio_disconnect(PIN_BTN1); else gpio_input_pullup(PIN_BTN1, 1);
    if (ignore_mask & 2u) gpio_disconnect(PIN_BTN2); else gpio_input_pullup(PIN_BTN2, 1);
    system_off();
}

int main(void)
{
    uint32_t reason = reset_reason_and_clear();
    gpio_input_pullup(PIN_BTN1, 0);
    gpio_input_pullup(PIN_BTN2, 0);
    hw_init();

    g_id = device_id();
    derive_key(g_id, g_key);

    delay_ms(DEBOUNCE_MS + 1);
    if (buttons_raw() == 0) {
        /* Woken without a button (battery insert, reset, debugger): show life, sleep */
        if (!(reason & RESET_RESETREAS_OFF_Msk)) { led(1); delay_ms(30); led(0); }
        go_to_sleep(0);
    }

    counter_init();
    radio_on();
    buttons_debounce_init();

    uint32_t t_start = millis(), t_last_tx = 0, t_both = 0;
    uint8_t  sent_any = 0, prev = 0;

    for (;;) {
        uint8_t b = buttons_debounced();
        uint32_t now = millis();

        if (b == 3u) {
            /* both held: no normal packets, count towards pairing */
            if (prev == 1u || prev == 2u) { for (int i = 0; i < 3; i++) { send(PKT_BUTTONS, 0); delay_ms(3); } }
            if (prev != 3u) t_both = now;
            if ((uint32_t)(now - t_both) >= PAIR_HOLD_MS) {
                uint32_t t0 = millis();
                while ((uint32_t)(millis() - t0) < PAIR_BURST_MS) {
                    led(((millis() - t0) / 100u) & 1u);
                    send(PKT_PAIR, 0);
                    delay_ms(PAIR_PERIOD_MS);
                }
                led(0);
                while (buttons_raw() != 0) { delay_ms(10); }   /* wait for release */
                go_to_sleep(0);
            }
            led(((now - t_both) / 500u) & 1u);   /* slow blink while waiting for pair */
        } else if (b != 0) {
            led(1);
            if (!sent_any || (uint32_t)(now - t_last_tx) >= TX_PERIOD_MS || b != prev) {
                send(PKT_BUTTONS, b);
                t_last_tx = now; sent_any = 1;
            }
            if ((uint32_t)(now - t_start) > STUCK_MS) {
                for (int i = 0; i < 3; i++) { send(PKT_BUTTONS, 0); delay_ms(3); }
                go_to_sleep(b);
            }
        } else {
            /* all released */
            if (sent_any) { for (int i = 0; i < 3; i++) { send(PKT_BUTTONS, 0); delay_ms(3); } }
            go_to_sleep(0);
        }
        prev = b;
        delay_ms(1);
    }
}
