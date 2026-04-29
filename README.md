# RFlippler

A Flipper Zero app for reading and dumping HiTag2 / Paxton Net2 RFID cards using an [RFIDler LF](http://rfidler.org) connected via GPIO UART.

---

## Hardware Setup

Connect the RFIDler to the Flipper Zero GPIO header:

| RFIDler | Flipper Zero GPIO |
|---------|-------------------|
| TX      | Pin 14 (RX)       |
| RX      | Pin 13 (TX)       |
| GND     | GND               |
| VCC     | External 5V (USB) |

> **Note:** The Flipper's GPIO pins operate at 3.3V logic, which is compatible with the RFIDler's UART. The RFIDler itself requires 5V power — this can be supplied from the Flipper's pin 1 (5V passthrough) when the Flipper is USB-powered, or from a separate supply.

The RFIDler should have `AUTORUN=API` saved in NVM for correct operation. If it's in CLI mode, connect via a terminal at 115200 baud and run:

```
SET AUTORUN API
SAVE
REBOOT
```

---

## Features

- **HiTag2 UID read** — reads the 32-bit UID from any HiTag2 tag
- **Paxton Auto** — sweeps a list of known Paxton Net2 passwords to authenticate and dump all 8 pages
- **Paxton (default)** — dumps using the known Paxton default key (`BDF5E846`)
- **EM4100 / EM4102, HID 26-bit, Indala** — UID reads for common LF tag types
- **Badge number decode** — extracts the Paxton badge number from page 2 of the dump
- **Save & view dumps** — successful dumps are saved to `/ext/apps/rfidler/` on the SD card and can be browsed and reloaded from the app

---

## Navigation

| Button | Action |
|--------|--------|
| Up / Down | Scroll menu / dump / saved list |
| OK | Read selected tag / open saved dump |
| Back | Return to menu / quit |

---

## Building

Requires [ufbt](https://github.com/flipperdevices/flipperzero-ufbt).

```bash
cd rfidler
ufbt        # build
ufbt launch # build and deploy to connected Flipper
```

The compiled `.fap` is placed in `rfidler/dist/`.

---

## Saved Dumps

Dumps are saved automatically after a successful read to:

```
/ext/apps/rfidler/<B0_UID>.txt
```

File format:
```
# Badge: 12345678
# Key: Paxton default
B0: AABBCCDD
B1: ...
B7: XXXXXXXX
```

Select **View Saved** from the main menu to browse and reload saved dumps.

---

## Known Paxton Passwords

The sweep mode tries the following passwords in order:

| Password   | Source                        |
|------------|-------------------------------|
| `BDF5E846` | Paxton Net2 default (Sheldrake analysis) |
| `4D494B52` | NXP/Philips factory default ("MIKR") |
| `00000000` | All zeros                     |
| `FFFFFFFF` | All ones                      |
| `DEADBEEF` | Common test key               |
| `CAFEBABE` | Common test key               |

---

## References

- [Kev Sheldrake — Paxton Net2 analysis](https://github.com/kev-sheldrake/hitag2-crack)
- [RFIDler by Aperture Labs](http://rfidler.org)
- [Verdult et al. — "Gone in 360 Seconds"](https://www.cs.bham.ac.uk/~garciaf/publications/gone.pdf)

---

## Acknowledgements

Built with assistance from [Claude](https://claude.ai) (Anthropic).
