> **Archived variant.** AES-CMAC authenticated + rolling-counter version. It works, but it was replaced by the
> simpler protocol in `../firmware`. Flash both boards with the same `secret.h` build.

# nRF54L05 secure 2-button remote + receiver (bare metal)

## Files
- `out/remote.hex` – flash into the coin-cell remote
- `out/receiver.hex` – flash into the 3.3 V receiver
- `common/secret.h`: the **master secret shared by both** boards. It's not in git; create it with
  `python3 make_secret.py`, then build *both* remote and receiver with the same file.

## Flashing (same as the blinky)
```
.\bin\openocd.exe -f interface/cmsis-dap.cfg -f target/nordic/nrf54l.cfg `
  -c "init" -c "reset halt" -c "mww 0x5004b500 0x101" `
  -c "load_image receiver.hex" -c "mww 0x5004b008 1" `
  -c "verify_image receiver.hex" -c "reset run" -c "exit"
```
Use `remote.hex` for the remote. Flashing doesn't touch the pairing/counter area
(0x7B000–0x7CFFF), so pairings survive reflashing.

## Pin mapping
| Remote | → | Receiver output |
|---|---|---|
| SW2 (P1.09, "BTN1/PAIR") | → | OUT1 = P1.05 → R35 → H30 pin 3 |
| SW1 (P1.10, "BTN2")      | → | OUT2 = P1.06 → R34 → H30 pin 2 |

## Using it
**Pairing**
1. Receiver: hold SW30 for **3 s** (LED goes solid), then release → LED blinks (pair mode, 30 s).
2. Remote: hold **both buttons for 3 s** (LED blinks slowly while waiting, then fast = sending pairing data for 5 s).
3. Receiver LED on for 2 s = paired. Up to 4 remotes; a 5th replaces slot 0.

**Forget all remotes:** hold receiver SW30 **10 s** (LED flashes fast), release.

**Normal use:** hold a remote button → matching output high (receiver LED on). Release → output low.
If the signal is lost, the output drops after 300 ms. Pressing both buttons doesn't drive
the outputs (that combination is reserved for pairing).

**Power:** the remote is in System OFF whenever no button is pressed, and a button press wakes it.
A fresh battery gives one short LED blink.
Note: with the SWD probe attached, the chip can't reach true System OFF. Measure sleep current
with the probe disconnected.

## Security
- Each packet carries the remote ID, a rolling 32-bit counter and an 8-byte AES-CMAC.
- Per-remote key = AES(master secret, remote ID). The receiver derives it during pairing.
- The receiver only accepts counters newer than the last one seen, and stores them in RRAM
  at the end of each press, so recorded packets can't be replayed, even after a power cycle.
- The remote reserves counters in blocks of 1024 in RRAM before using them, so a battery
  change never reuses a counter. The log is wear-levelled: ~2.5 million wake-ups.
- For production: give each product line its own `secret.h`, and enable APPROTECT so the
  secret can't be read out over SWD. It's left disabled now so you can keep reflashing.

## Rebuilding
`python3 make_secret.py && sh build.sh` (clang + lld, Nordic MDK from nrfx, ARM CMSIS_6 headers; paths at the top of the script).
Host tests: `gcc -I common test/test_crypto.c common/crypto.c` and
`gcc -I common test/test_logic.c common/nvlog.c common/crypto.c`.
