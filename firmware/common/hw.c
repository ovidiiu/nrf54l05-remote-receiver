/*
 * Low-level drivers for nRF54L05, bare metal, register names from Nordic MDK.
 * Radio init follows Nordic's ESB driver for nRF54L (PHYEND shorts,
 * errata 20 constant latency, errata 39 PLL start).
 */
#include "hw.h"
#include "proto.h"
#include "nrf_erratas.h"

/* ------------------------------------------------------------------ time */
static volatile uint32_t g_ms;
void SysTick_Handler(void) { g_ms++; }
uint32_t millis(void) { return g_ms; }

void delay_ms(uint32_t ms)
{
    uint32_t t0 = g_ms;
    /* Busy-wait: SysTick stops while the CPU sleeps on nRF, so __WFI() here would hang. */
    while ((uint32_t)(g_ms - t0) < ms) { __NOP(); }
}

void delay_us(uint32_t us)
{
    extern uint32_t SystemCoreClock;
    uint32_t loops = (SystemCoreClock / 4000000u) * us;   /* ~4 cycles/loop */
    while (loops--) { __NOP(); }
}

/* ------------------------------------------------------------------ init */
#define HFXO_LOAD_CAP_X4  48   /* crystal CL = 12 pF (YSX211SL / X201632MOB4SI), in 0.25 pF */

static void hfxo_caps_init(void)
{
    uint32_t trim = NRF_FICR->XOSC32MTRIM;
    if (trim == 0xFFFFFFFFu) return;                       /* untrimmed: keep reset value */
    int32_t slope  = (int32_t)(trim & 0x1FFu);
    if (slope & 0x100) slope -= 0x200;                     /* 9-bit two's complement */
    int32_t offset = (int32_t)((trim >> 16) & 0x3FFu);
    /* Datasheet: INTCAP = ((C - 5.5) * (SLOPE + 791) + OFFSET * 4) / 256, C in pF */
    int32_t v = ((HFXO_LOAD_CAP_X4 - 22) * (slope + 791) + offset * 16) / 1024;
    if (v < 0) v = 0;
    if (v > 0x3F) v = 0x3F;
    NRF_OSCILLATORS_S->XOSC32M.CONFIG.INTCAP = (uint32_t)v;
}

void hw_init(void)
{
    extern uint32_t SystemCoreClock;
    /* Both boards have the 4.7 uH inductor on DCC: use the DC/DC (lower current) */
    NRF_REGULATORS_S->VREGMAIN.DCDCEN = REGULATORS_VREGMAIN_DCDCEN_VAL_Enabled;
    hfxo_caps_init();
    SysTick_Config(SystemCoreClock / 1000u);
    gpio_output(PIN_LED, 1);   /* LED off */
}

/* ------------------------------------------------------------------ GPIO */
#define PORT NRF_P1_S

void gpio_output(uint32_t pin, int level)
{
    gpio_set(pin, level);
    PORT->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
                         (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
}

void gpio_set(uint32_t pin, int level)
{
    if (level) PORT->OUTSET = 1u << pin; else PORT->OUTCLR = 1u << pin;
}

void gpio_input_pullup(uint32_t pin, int sense_low)
{
    PORT->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
                         (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
                         (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos) |
                         ((sense_low ? GPIO_PIN_CNF_SENSE_Low : GPIO_PIN_CNF_SENSE_Disabled)
                              << GPIO_PIN_CNF_SENSE_Pos);
}

void gpio_disconnect(uint32_t pin)
{
    PORT->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos) |
                         (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
}

int gpio_read(uint32_t pin) { return (PORT->IN >> pin) & 1u; }

/* ------------------------------------------------------------------ radio */
static uint8_t s_txbuf[1 + 32] __attribute__((aligned(4)));
static uint8_t s_rxbuf[1 + 32] __attribute__((aligned(4)));

static void radio_disable_wait(void)
{
    NRF_RADIO_S->SHORTS = 0;
    NRF_RADIO_S->EVENTS_DISABLED = 0;
    NRF_RADIO_S->TASKS_DISABLE = 1;
    uint32_t t0 = millis();
    while (NRF_RADIO_S->STATE != RADIO_STATE_STATE_Disabled) {
        if ((uint32_t)(millis() - t0) > 5) break;
    }
    NRF_RADIO_S->EVENTS_DISABLED = 0;
}

volatile uint32_t g_hfxo_status;   /* bit0 started, bit1 tuned, bit2 tune failed, bit3 manual tune, bit4 tune timeout */
static volatile int8_t g_last_rssi;
int8_t radio_last_rssi(void) { return g_last_rssi; }

static int wait_event(volatile uint32_t *ev, uint32_t timeout_ms)
{
    uint32_t t0 = millis();
    while (!*ev) { if ((uint32_t)(millis() - t0) > timeout_ms) return 0; }
    return 1;
}

void radio_on(void)
{
    /* Same order as Nordic's nrfx_clock_xo_start(): PLL first (errata 39), then XO.
       On nRF54L the HFXO is only stable after XOTUNED, not after XOSTARTED. */
    uint32_t st = 0;
    NRF_CLOCK_S->EVENTS_XOSTARTED = 0;
    NRF_CLOCK_S->EVENTS_XOTUNED = 0;
    NRF_CLOCK_S->EVENTS_XOTUNEFAILED = 0;
    NRF_CLOCK_S->EVENTS_XOTUNEERROR = 0;
    if (nrf54l_errata_39()) NRF_CLOCK_S->TASKS_PLLSTART = 1;
    if (nrf54l_errata_20()) NRF_POWER_S->TASKS_CONSTLAT = 1;
    NRF_CLOCK_S->TASKS_XOSTART = 1;
    if (wait_event(&NRF_CLOCK_S->EVENTS_XOSTARTED, 20)) st |= 1u;
    uint32_t t0 = millis();
    while (!NRF_CLOCK_S->EVENTS_XOTUNED && !NRF_CLOCK_S->EVENTS_XOTUNEFAILED &&
           (uint32_t)(millis() - t0) < 20) { }
    if (!NRF_CLOCK_S->EVENTS_XOTUNED) {
        /* not tuned automatically: request it */
        st |= 8u;
        NRF_CLOCK_S->EVENTS_XOTUNEFAILED = 0;
        NRF_CLOCK_S->TASKS_XOTUNE = 1;
        t0 = millis();
        while (!NRF_CLOCK_S->EVENTS_XOTUNED && !NRF_CLOCK_S->EVENTS_XOTUNEFAILED &&
               (uint32_t)(millis() - t0) < 20) { }
    }
    if (NRF_CLOCK_S->EVENTS_XOTUNED) st |= 2u;
    else if (NRF_CLOCK_S->EVENTS_XOTUNEFAILED) st |= 4u;
    else st |= 16u;
    g_hfxo_status = st;

    radio_disable_wait();
    NRF_RADIO_S->MODE      = RADIO_MODE_MODE_Nrf_1Mbit << RADIO_MODE_MODE_Pos;
    NRF_RADIO_S->TXPOWER   = RADIO_TXPOWER_TXPOWER_Pos8dBm << RADIO_TXPOWER_TXPOWER_Pos;
    NRF_RADIO_S->FREQUENCY = RF_CHANNEL << RADIO_FREQUENCY_FREQUENCY_Pos;     /* 2400 + n MHz */
    NRF_RADIO_S->PCNF0     = (8u << RADIO_PCNF0_LFLEN_Pos) |
                             (RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos);
    NRF_RADIO_S->PCNF1     = (32u << RADIO_PCNF1_MAXLEN_Pos) |
                             (0u << RADIO_PCNF1_STATLEN_Pos) |
                             (4u << RADIO_PCNF1_BALEN_Pos) |
                             (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                             (RADIO_PCNF1_WHITEEN_Enabled << RADIO_PCNF1_WHITEEN_Pos);
    NRF_RADIO_S->DATAWHITE = (0x89u << RADIO_DATAWHITE_POLY_Pos) |
                             ((0x40u | (RF_CHANNEL & 0x3Fu)) << RADIO_DATAWHITE_IV_Pos);
    NRF_RADIO_S->BASE0     = RF_BASE0;
    NRF_RADIO_S->BASE1     = RF_BASE1;
    NRF_RADIO_S->PREFIX0   = (RF_PREFIX0 << 0) | (RF_PREFIX1 << 8);
    NRF_RADIO_S->RXADDRESSES = (1u << RF_ADDR_DATA) | (1u << RF_ADDR_PAIR);
    NRF_RADIO_S->CRCCNF    = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
                             (RADIO_CRCCNF_SKIPADDR_Include << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO_S->CRCPOLY   = 0x11021u;
    NRF_RADIO_S->CRCINIT   = 0xFFFFu;
}

void radio_off(void)
{
    radio_disable_wait();
    if (nrf54l_errata_20()) NRF_POWER_S->TASKS_LOWPWR = 1;
    if (nrf54l_errata_39()) NRF_CLOCK_S->TASKS_PLLSTOP = 1;
    NRF_CLOCK_S->TASKS_XOSTOP = 1;
}

void radio_tx(const uint8_t *payload, uint8_t len, uint32_t logical_addr)
{
    if (len > 32) len = 32;
    radio_disable_wait();
    s_txbuf[0] = len;
    memcpy(&s_txbuf[1], payload, len);
    NRF_RADIO_S->TXADDRESS = logical_addr;
    NRF_RADIO_S->PACKETPTR = (uint32_t)s_txbuf;
    NRF_RADIO_S->SHORTS = (1u << RADIO_SHORTS_READY_START_Pos) |
                          (1u << RADIO_SHORTS_PHYEND_DISABLE_Pos);
    NRF_RADIO_S->EVENTS_DISABLED = 0;
    __DSB();
    NRF_RADIO_S->TASKS_TXEN = 1;
    uint32_t t0 = millis();
    while (!NRF_RADIO_S->EVENTS_DISABLED) {
        if ((uint32_t)(millis() - t0) > 5) { radio_disable_wait(); break; }
    }
    NRF_RADIO_S->EVENTS_DISABLED = 0;
}

void radio_rx_start(void)
{
    radio_disable_wait();
    NRF_RADIO_S->PACKETPTR = (uint32_t)s_rxbuf;
    NRF_RADIO_S->SHORTS = (1u << RADIO_SHORTS_READY_START_Pos) |
                          (1u << RADIO_SHORTS_ADDRESS_RSSISTART_Pos) |
                          (1u << RADIO_SHORTS_PHYEND_DISABLE_Pos);
    NRF_RADIO_S->EVENTS_DISABLED = 0;
    __DSB();
    NRF_RADIO_S->TASKS_RXEN = 1;
}

int radio_rx_poll(uint8_t *payload, uint32_t maxlen, uint32_t *logical_addr)
{
    if (!NRF_RADIO_S->EVENTS_DISABLED) return 0;
    NRF_RADIO_S->EVENTS_DISABLED = 0;
    int ok = (NRF_RADIO_S->CRCSTATUS & 1u) == RADIO_CRCSTATUS_CRCSTATUS_CRCOk;
    uint32_t len = s_rxbuf[0];
    uint32_t match = NRF_RADIO_S->RXMATCH & RADIO_RXMATCH_RXMATCH_Msk;
    int result = 0;
    if (ok) g_last_rssi = (int8_t)(-(int32_t)(NRF_RADIO_S->RSSISAMPLE & RADIO_RSSISAMPLE_RSSISAMPLE_Msk));
    if (ok && len > 0 && len <= maxlen && len <= 32) {
        memcpy(payload, &s_rxbuf[1], len);
        *logical_addr = match;
        result = (int)len;
    }
    /* re-arm immediately */
    NRF_RADIO_S->EVENTS_DISABLED = 0;
    __DSB();
    NRF_RADIO_S->TASKS_RXEN = 1;
    return result;
}

/* ------------------------------------------------------------------ RRAM */
void rram_write_line(uintptr_t addr, const uint32_t w[4])
{
    while (!(NRF_RRAMC_S->READY & RRAMC_READY_READY_Msk)) { }
    /* WEN + write buffer of one 128-bit line, so the 4 words cost one line write */
    NRF_RRAMC_S->CONFIG = (1u << RRAMC_CONFIG_WEN_Pos) | (1u << RRAMC_CONFIG_WRITEBUFSIZE_Pos);
    volatile uint32_t *p = (volatile uint32_t *)addr;
    for (int i = 0; i < 4; i++) p[i] = w[i];
    __DSB();
    NRF_RRAMC_S->TASKS_COMMITWRITEBUF = 1;
    while (!(NRF_RRAMC_S->READY & RRAMC_READY_READY_Msk)) { }
    NRF_RRAMC_S->CONFIG = 0;
    __DSB(); __ISB();
}

/* ------------------------------------------------------------------ misc */
uint64_t device_id(void)
{
    return (uint64_t)NRF_FICR->INFO.DEVICEID[0] | ((uint64_t)NRF_FICR->INFO.DEVICEID[1] << 32);
}

uint32_t reset_reason_and_clear(void)
{
    uint32_t r = NRF_RESET_S->RESETREAS;
    NRF_RESET_S->RESETREAS = 0xFFFFFFFFu;   /* write 1 to clear */
    return r;
}

void system_off(void)
{
    NRF_RESET_S->RESETREAS = 0xFFFFFFFFu;   /* must be clear or we wake at once */
    __DSB();
    NRF_REGULATORS_S->SYSTEMOFF = 1;        /* REGULATORS_SYSTEMOFF_SYSTEMOFF_Enter */
    __DSB();
    for (;;) { __WFE(); }
}
