/*
 * Bare-metal blinky for nRF54L05 (QFN48) - LED on P1.13
 *
 * Board wiring (Sonat_suport_mainboard): VCC -> LED30 -> R30 (1k) -> P1.13
 * so the LED is ACTIVE LOW: driving P1.13 low turns it on.
 *
 * Nordic's SystemInit() (system_nrf54l.c from nrfx MDK) runs before main():
 * it applies FICR trims, errata workarounds and keeps the debug port
 * (APPROTECT) open so you can reflash with SWD afterwards.
 */
#include <stdint.h>
#include "nrf.h"

#define LED_PORT   NRF_P1_S
#define LED_PIN    13u

static void delay_ms(uint32_t ms)
{
    /* SystemCoreClock is set by SystemInit (128 MHz by default).
       The loop body takes ~4 cycles; good enough for a blinky. */
    extern uint32_t SystemCoreClock;
    uint32_t loops = (SystemCoreClock / 4000u) * ms;
    while (loops--) {
        __NOP();
    }
}

int main(void)
{
    /* Start with LED off (pin high), then make it an output. */
    LED_PORT->OUTSET = (1u << LED_PIN);
    LED_PORT->PIN_CNF[LED_PIN] =
        (GPIO_PIN_CNF_DIR_Output      << GPIO_PIN_CNF_DIR_Pos)   |
        (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos) |
        (GPIO_PIN_CNF_PULL_Disabled   << GPIO_PIN_CNF_PULL_Pos);

    for (;;) {
        LED_PORT->OUTCLR = (1u << LED_PIN);   /* LED on  */
        delay_ms(250);
        LED_PORT->OUTSET = (1u << LED_PIN);   /* LED off */
        delay_ms(250);
    }
}
