# HiTag2 Protocol Reference

## Physical Layer

- **Carrier:** 125 kHz, passive inductive coupling
- **Reader → Tag:** Binary Pulse Length Modulation (BPLM)
  - Short pulse (T_0 = 20×T0 = 160µs) = bit 0
  - Long pulse  (T_1 = 30×T0 = 240µs) = bit 1
- **Tag → Reader:** Manchester encoding
  - Rising edge at mid-period = bit 0
  - Falling edge at mid-period = bit 1
  - RF/64 clock = 64×T0 = 512µs per bit

## Command Set

Reader commands are 10 bits, MSB first. Address bits use complement for error detection.

```
READ_PAGE(n):  1 1 n1 n0 x 0 0 ~n1 ~n0 ~x    (x = parity of n)
WRITE_PAGE(n): 1 0 n1 n0 x 0 1 ~n1 ~n0 ~x
START_AUTH:    0 0 0 0 0 0 0 0 0 0
HALT:          (same as START_AUTH)
```

Address complement encoding from Proxmark3:
```c
tx[0] = 0xC0 | (page << 3) | ((page ^ 7) >> 2);
tx[1] = ((page ^ 7) << 6);
```

## Communication Sequence (Public Mode)

```
Reader                          Tag
  |--- START_AUTH (10 bits) ---->|
  |<--- UID (32 bits) -----------|   Page 0
  |--- READ_PAGE(1) (10 bits) -->|
  |<--- RWD Password (32 bits) --|   Page 1
  |--- READ_PAGE(3) (10 bits) -->|
  |<--- Config + Tag PW (32b) ---|   Page 3
  |--- READ_PAGE(4..7) ----------|
  |<--- User data (32 bits each)-|
```

## Communication Sequence (Password Mode)

```
Reader                          Tag
  |--- START_AUTH (10 bits) ---->|
  |<--- UID (32 bits) -----------|
  |--- RWD Password (32 bits) -->|   Reader sends page 1 value
  |<--- Config byte (8 bits) ----|   Tag confirms match
  |--- READ/WRITE commands ------>|   Authenticated session
```

## Communication Sequence (Crypto Mode)

```
Reader                          Tag
  |--- START_AUTH (10 bits) ---->|
  |<--- UID (32 bits) -----------|   Plaintext UID
  |--- nonce IV (32 bits) ------>|   Reader sends random nonce
  |                              |   Both init LFSR(uid, key, nonce)
  |<--- auth response (32 bits) -|   Encrypted with keystream[0:31]
  |--- encrypted commands ------->|  All subsequent traffic encrypted
```

## Memory Map Detail

```
Page 0: [UID bit 31..0]
  - Factory programmed, read-only
  - Broadcast in public mode without auth

Page 1: [RWD_PWD bit 31..0]
  - Reader/writer password for password mode
  - Readable in public mode (security note: plaintext!)
  - Default: manufacturer-set

Page 2: [KEY_HIGH bit 15..0 | RESERVED bit 15..0]
  - Upper 16 bits of 48-bit crypto key
  - Lower 32 bits are in... (implementation detail: derived during auth)

Page 3: [CONFIG(8) | TAG_PWD(24)]
  CONFIG byte bits:
    [7:6] = OTP/block lock flags
    [5:4] = reserved
    [3]   = auth mode: 0=password, 1=crypto
    [2]   = crypto on: 0=public, 1=auth required
    [1:0] = data rate: 00=RF/64, 01=RF/32, 10=RF/16

  Default factory config = 0x06 (crypto mode, RF/64)

Pages 4-7: [USER_DATA bit 31..0] each
  - Freely readable/writable after auth
  - 16 bytes total user storage
```

## Timing Tolerances

| Parameter | Nominal | Min | Max |
|---|---|---|---|
| T0 (period) | 8 µs | 7.8 µs | 8.2 µs |
| T_0 (zero bit) | 160 µs | 144 µs | 176 µs |
| T_1 (one bit) | 240 µs | 208 µs | 264 µs |
| T_WAIT | 1592 µs | 1592 µs | — |
| T_PROG | 4912 µs | 4000 µs | 6000 µs |

±10% recommended tolerance for field conditions; Proxmark3 uses ±20%.

## Response Codes

| Code | Meaning |
|---|---|
| 0xC0 | No tag / command not understood |
| 0xC4 | Password check failure |
| 0xD6 | Password check success |
