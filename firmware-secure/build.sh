#!/bin/sh
# Build remote.hex and receiver.hex with clang/lld (no Nordic SDK needed).
# Needs: Nordic MDK (nrfx/bsp/stable/mdk) and ARM CMSIS_6 Core headers.
set -e
MDK=${MDK:-/tmp/nrfx/bsp/stable/mdk}
CMSIS=${CMSIS:-/tmp/cmsis/CMSIS/Core/Include}
OUT=${OUT:-out}
mkdir -p "$OUT"

CFLAGS="--target=thumbv8m.main-none-eabihf -mcpu=cortex-m33 -mfloat-abi=hard -mfpu=fpv5-sp-d16 -mthumb
 -Os -g -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections -ffreestanding -nostdlib
 -DNRF54L05_XXAA -DNRF_APPLICATION -D__STARTUP_CLEAR_BSS -D__START=main
 -I common -I $MDK -I $MDK/nrf54l -I $MDK/nrf54l/nrf54l05 -I $MDK/common -I $CMSIS"

clang $CFLAGS -c $MDK/nrf54l/system_nrf54l.c -o $OUT/system.o
clang $CFLAGS -x assembler-with-cpp -c $MDK/nrf54l/nrf54l05/gcc_startup_nrf54l05_application.S -o $OUT/startup.o
clang $CFLAGS -c common/hw.c -o $OUT/hw.o
clang $CFLAGS -c common/crypto.c -o $OUT/crypto.o
clang $CFLAGS -c common/nvlog.c -o $OUT/nvlog.o
clang $CFLAGS -c common/libc_min.c -o $OUT/libc_min.o

for app in remote receiver; do
  clang $CFLAGS -c $app/main.c -o $OUT/$app.o
  ld.lld -T nrf54l05_app.ld -L . -L $MDK/common --gc-sections -e Reset_Handler \
     $OUT/startup.o $OUT/system.o $OUT/hw.o $OUT/crypto.o $OUT/nvlog.o $OUT/libc_min.o $OUT/$app.o \
     -o $OUT/$app.elf -Map $OUT/$app.map
  llvm-objcopy -O ihex $OUT/$app.elf $OUT/$app.hex
  echo "built $OUT/$app.hex"
done
