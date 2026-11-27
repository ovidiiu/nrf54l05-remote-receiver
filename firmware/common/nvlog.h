#ifndef NVLOG_H
#define NVLOG_H
#include <stdint.h>
/* Provided by hw.c on target (or a mock on the host) */
void rram_write_line(uintptr_t addr, const uint32_t w[4]);
int  nvlog_read(uintptr_t base, uint32_t lines, uint32_t *value);   /* 1 if found */
void nvlog_write(uintptr_t base, uint32_t lines, uint32_t value);
#endif
