#ifndef HW_H
#define HW_H
#include <stdint.h>
#include "nrf.h"

/* ---- Board pins (both boards use port P1) ---- */
#define PIN_LED     13u     /* active low: VCC -> LED -> 1k -> P1.13 */
#define PIN_BTN1     9u     /* to GND when pressed ("BTN1/PAIR") */
#define PIN_BTN2    10u     /* to GND when pressed ("BTN2"), remote only */
#define PIN_OUT1     5u     /* receiver: P1.05 -> R35 -> H30 pin 3 */
#define PIN_OUT2     6u     /* receiver: P1.06 -> R34 -> H30 pin 2 */

/* ---- Non-volatile area (top of the 500 KB RRAM, kept out of the linker map) ---- */
#define NV_PAIR_BASE   0x0007B000u   /* receiver pairing table, 1 line per slot */
#define NV_LOG_BASE    0x0007C000u   /* 4 KB of 16-byte log lines              */
#define NV_LOG_LINES   256u

void     hw_init(void);              /* SysTick 1 ms, DC/DC, HFXO load caps */
uint32_t millis(void);
void     delay_ms(uint32_t ms);
void     delay_us(uint32_t us);

/* GPIO on P1 */
void gpio_output(uint32_t pin, int level);
void gpio_set(uint32_t pin, int level);
void gpio_input_pullup(uint32_t pin, int sense_low);
void gpio_disconnect(uint32_t pin);
int  gpio_read(uint32_t pin);
static inline void led(int on) { gpio_set(PIN_LED, !on); }

/* Radio (proprietary 1 Mbit, see proto.h for addresses/channel) */
void radio_on(void);                 /* start HFXO (+tune) + configure radio */
int8_t radio_last_rssi(void);        /* dBm of last good packet */
extern volatile uint32_t g_hfxo_status;
void radio_off(void);                /* disable radio + stop HFXO */
void radio_tx(const uint8_t *payload, uint8_t len, uint32_t logical_addr);
void radio_rx_start(void);
/* Returns payload length (>0) when a packet with good CRC arrived; radio is
   re-armed automatically. 0 = nothing yet. */
int  radio_rx_poll(uint8_t *payload, uint32_t maxlen, uint32_t *logical_addr);

/* RRAM line write + wear-levelled log */
#include "nvlog.h"

uint64_t device_id(void);
uint32_t reset_reason_and_clear(void);
void     system_off(void) __attribute__((noreturn));

#endif
