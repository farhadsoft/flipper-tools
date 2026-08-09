# uart_capture.py — continuous hardware-UART log capture, run via hub as a
# background process (this channel is physically separate from the USB-CDC
# CLI on COM4, so it can stream freely while COM4 is used for input
# injection -- that's the whole point, see CLAUDE.md 2026-08-09 entry).
#
# Usage: python uart_capture.py --port COM<N> --out logs/uart_trace.log
#
# Flipper pin 13 (TX) -> adapter RX, GND -> GND, 230400 8N1. Read-only: RX
# is never opened/driven, so there's no way this script can inject input.
import argparse, os, sys, time, serial

ap = argparse.ArgumentParser()
ap.add_argument("--port", required=True)
ap.add_argument("--baud", type=int, default=230400)
ap.add_argument("--out", default="logs/uart_trace.log")
args = ap.parse_args()

os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
print(f"[uart_capture] {args.port} @ {args.baud} 8N1 -> {args.out}", flush=True)

with open(args.out, "ab", buffering=0) as f:
    while True:
        try:
            ser = serial.Serial(args.port, args.baud, timeout=0.5)
            print(f"[uart_capture] connected", flush=True)
            while True:
                chunk = ser.read(4096)
                if chunk:
                    f.write(chunk)
                    # Mirror to stdout too (hub logs), decoded best-effort.
                    sys.stdout.write(chunk.decode(errors="replace"))
                    sys.stdout.flush()
        except serial.SerialException as e:
            print(f"[uart_capture] disconnected: {e!r} -- retrying in 1s", flush=True)
            time.sleep(1)
