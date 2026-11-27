/* nRF54L05 memory map (from Nordic nrf54l05_xxaa_application.ld, minus newlib GROUP) */
MEMORY
{
  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 0x7D000
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 0x18000
}
INCLUDE "nrf_common.ld"
