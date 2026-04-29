#!/usr/bin/env python3
"""Live monitor for HiTag2 Flipper debug logs via USB serial."""
import serial, time, sys, re

PORT  = "/dev/ttyACM0"
BAUD  = 230400
SECS  = 300   # run for 5 minutes

def main():
    print(f"Opening {PORT} ...", flush=True)
    try:
        s = serial.Serial(PORT, BAUD, timeout=0.1)
    except Exception as e:
        print(f"ERROR: {e}")
        sys.exit(1)

    # Wake CLI
    s.write(b"\r\n")
    time.sleep(0.3)
    s.read(4096)  # discard banner

    # Set log level to info, then start streaming
    s.write(b"log info\r\n")
    print("Sent 'log'. Navigate to Read Tag on the Flipper now.\n", flush=True)

    deadline = time.time() + SECS
    buf = b""
    while time.time() < deadline:
        chunk = s.read(256)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                # Always print HiTag2 lines; dim everything else
                if "HiTag2" in text:
                    print(f"\033[92m{text}\033[0m", flush=True)
                else:
                    # show non-empty non-noise lines faintly
                    if text.strip() and not text.startswith(">"):
                        print(f"\033[2m{text}\033[0m", flush=True)

    s.write(b"\r\n")  # exit log mode
    s.close()
    print("\nDone.")

if __name__ == "__main__":
    main()
