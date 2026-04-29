#!/usr/bin/env python3
"""
HiTag2 software simulation — generates synthetic pulse sequences for decoder testing.

Usage:
  python3 hitag2_sim.py --uid 0x12345678 --mode public
  python3 hitag2_sim.py --uid 0x12345678 --mode public --dump-pulses

Outputs a stream of (level, duration_us) pairs that a HiTag2 tag would produce
in response to a START_AUTH + READ_PAGE(0) command sequence.
"""

import argparse
import struct

T0_US = 8
MANCHESTER_HALF_US = 32 * T0_US  # 256us

def manchester_encode_word(word: int) -> list[tuple[bool, int]]:
    """Encode 32-bit word as Manchester pulses (MSB first).
    Returns list of (level, duration_us) pairs."""
    pulses = []
    for bit_pos in range(31, -1, -1):
        bit = (word >> bit_pos) & 1
        if bit == 1:
            pulses.append((True, MANCHESTER_HALF_US))
            pulses.append((False, MANCHESTER_HALF_US))
        else:
            pulses.append((False, MANCHESTER_HALF_US))
            pulses.append((True, MANCHESTER_HALF_US))
    return pulses


def simulate_public_mode(uid: int, user_data: bytes = None) -> list[tuple[bool, int]]:
    """Simulate a HiTag2 tag in public mode responding to START_AUTH then page reads."""
    if user_data is None:
        user_data = b'\x00' * 16

    # Memory layout
    pages = [
        uid.to_bytes(4, 'big'),                  # Page 0: UID
        b'\x00\x00\x00\x00',                     # Page 1: RWD password (shown in public mode)
        b'\x00\x00\x00\x00',                     # Page 2: Key high / reserved
        bytes([0x06, 0x00, 0x00, 0x00]),          # Page 3: Config (crypto mode) + tag pw
        user_data[0:4],                           # Pages 4-7: user data
        user_data[4:8],
        user_data[8:12],
        user_data[12:16],
    ]

    pulses = []
    # Tag responds to START_AUTH with UID, then expects READ_PAGE commands
    # Here we simulate the full response sequence: UID + pages 1-7
    for i, page in enumerate(pages):
        word = struct.unpack('>I', page)[0]
        pulses.extend(manchester_encode_word(word))
        # Inter-frame gap: ~1 bit low (reader re-arms)
        pulses.append((False, MANCHESTER_HALF_US * 2))

    return pulses


def main():
    parser = argparse.ArgumentParser(description='HiTag2 tag simulator')
    parser.add_argument('--uid', type=lambda x: int(x, 0), default=0x12345678,
                        help='32-bit UID (hex or decimal)')
    parser.add_argument('--mode', choices=['public'], default='public')
    parser.add_argument('--dump-pulses', action='store_true',
                        help='Print (level, duration_us) pairs to stdout')
    args = parser.parse_args()

    pulses = simulate_public_mode(args.uid)

    if args.dump_pulses:
        for level, dur in pulses:
            print(f"{'H' if level else 'L'} {dur:6d}us")
    else:
        total_us = sum(d for _, d in pulses)
        print(f"UID:       0x{args.uid:08X}")
        print(f"Mode:      {args.mode}")
        print(f"Pulses:    {len(pulses)}")
        print(f"Duration:  {total_us / 1000:.1f} ms")
        print()
        print("Feed these pulses to hitag2_decoder_feed() to test the decoder.")
        print("Run with --dump-pulses to get raw (level, duration) output.")


if __name__ == '__main__':
    main()
