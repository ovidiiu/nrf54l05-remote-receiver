# nRF54L05 2-button remote + receiver (bare metal, simple protocol)

## Files
- `out/remote.hex` – flash into the coin-cell remote
- `out/receiver.hex` – flash into the 3.3 V receiver

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

## Protocol (no encryption)
6-byte packets on 2477 MHz, 1 Mbit, +8 dBm: type, remote ID (32 bit), buttons.
Each packet is sent twice, every 20 ms while a button is held. The radio CRC rejects
corrupted packets and pairing makes the receiver ignore other remotes. There's no
protection against someone deliberately recording and replaying packets.
The previous secure (AES-CMAC + rolling counter) version is in git history / the earlier zip.

## Debug counters (receiver)
`mdw 0x20000004 17`: magic db600002, packets ok, rejected (parse / pair / unpaired),
accepted, last RSSI, min RSSI, max RSSI, HFXO status (3 = ok), packets in last press,
last press duration ms, number of presses, last length, last packet bytes (2 words), last ID.
Delivery rate of a press ≈ packets_in_press / (2 × press_ms / 20).

## Rebuilding
`sh build.sh` (clang + lld, Nordic MDK from nrfx, ARM CMSIS_6 headers; paths at the top of the script).
Host test: `gcc -I common test/test_logic.c`.
