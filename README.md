# nRF54L05 two-button remote + receiver

Coin-cell (CR2032) remote with two buttons and a 3.3 V receiver, both on Nordic **nRF54L05 (QFN48)**,
written bare-metal (no SDK) against Nordic's MDK headers. Holding a remote button drives the matching receiver
output high, which is used to turn a servo left/right.

| Folder | What |
|---|---|
| `firmware/` | **Current firmware** (simple 6-byte protocol). `prebuilt/remote.hex`, `prebuilt/receiver.hex` |
| `firmware-secure/` | Archived AES-CMAC + rolling-counter variant |
| `blinky/` | First bring-up test: LED blink on P1.13 |
| `hardware/` | EasyEDA/Altium ASCII schematics: `remote_P1.schdoc`, `receiver_P1.schdoc` |
| `tools/schnet.py` | Netlist extractor for these .schdoc files (`python3 -I tools/schnet.py file.schdoc`) |

## Behaviour
- **Remote:** sleeps in System OFF and wakes on a button press. While one button is held, it sends a packet every 20 ms
  (each one twice, +8 dBm, 2477 MHz, 1 Mbit). LED on while sending. Hold both buttons 3 s to pair.
- **Receiver:** always listening. OUT1 (P1.05, H30 pin 3) follows remote button SW2, and OUT2 (P1.06, H30 pin 2)
  follows SW1. Outputs drop on release, or 300 ms after the signal is lost.
  SW30: hold 3 s = pair mode (30 s), hold 10 s = forget all remotes. LED on while an output is active.

## Flashing (Raspberry Pi Pico as debug probe)
1. Pico: flash `debugprobe_on_pico.uf2` from github.com/raspberrypi/debugprobe (BOOTSEL + drag & drop).
2. Wiring: GP2 (pin 4) → SWDCLK, GP3 (pin 5) → SWDIO, GND → GND, 3V3 OUT (pin 36) → VCC_IN.
   Headers: receiver H31 / remote H3 = 1 nRST, 2 SWDCLK, 3 SWDIO, 4 VCC_IN, 5 GND.
3. OpenOCD: use a build newer than 0.12.0 (for example the `latest` snapshot on github.com/openocd-org/openocd/releases),
   which includes `target/nordic/nrf54l.cfg`. nRF54L uses RRAM, so `program` doesn't work. Use (Windows cmd):
```
bin\openocd.exe -f interface/cmsis-dap.cfg -f target/nordic/nrf54l.cfg -c "init" -c "reset halt" -c "mww 0x5004b500 0x101" -c "load_image receiver.hex" -c "mww 0x5004b008 1" -c "verify_image receiver.hex" -c "reset run" -c "exit"
```
4. **After flashing, disconnect SWD and power-cycle the board.** While in debug mode, the nRF54L only simulates
   System OFF, and button wake-up doesn't work properly.

Pairing and counters live in RRAM at 0x7B000–0x7CFFF, outside the program image, so they survive reflashing.

## Building
`cd firmware && sh build.sh`. Needs clang + ld.lld + llvm-objcopy, Nordic MDK from
github.com/NordicSemiconductor/nrfx (`bsp/stable/mdk`) and ARM CMSIS_6 `CMSIS/Core/Include`.
Set `MDK=` and `CMSIS=` if they aren't in `/tmp/nrfx` and `/tmp/cmsis`.

## Lessons from bring-up
- Reflashing the Pico with the official debugprobe firmware fixed "cannot read IDR".
- `SysTick` stops while the CPU sleeps (`WFI`), so delays must busy-wait.
- HFXO: start the PLL first (errata 39), then wait for **XOTUNED**, not just XOSTARTED. Internal load caps are
  set from the FICR trim (12 pF crystal).
- Radio logical address 0 (`0xE7` + `0xA5C35A3C`) was almost never received, while address 1 (`0x96` + `0x3C5AA5C3`)
  got ~96 %. Everything now uses address 1.
- The link margin is low (about −81 dBm at 30 cm), so the PCB antenna is the weak point.

## Hardware to-do for the next revision
- **Remote BT1 (BK-912, CR2032 retainer):** both tabs were wired to GND. Tabs are **+** and go to VCC_IN; the negative
  contact is a GND pad under the cell.
- **Q2 / Q30 (DMG2305UX):** drain and source are swapped, so there's no reverse-polarity protection.
  Drain goes to the input and source to the load.
- **DECA / DECD:** currently tied together directly. Nordic's reference puts a ferrite bead between them.
- **Antenna:** consider a chip antenna (e.g. Johanson 2450AT18A100) or u.FL, with a proper keep-out.
- If a servo should be driven directly, the receiver can generate 50 Hz PWM itself (firmware change).
