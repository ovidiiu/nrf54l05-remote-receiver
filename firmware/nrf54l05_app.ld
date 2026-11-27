/* nRF54L05: program RRAM ends at 0x7B000; 0x7B000-0x7CFFF is reserved for
   pairing table + counter logs (see common/hw.h). */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 0x7B000
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 0x18000
}
INCLUDE "nrf_common.ld"
