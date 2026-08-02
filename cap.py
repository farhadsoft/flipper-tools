import serial, time, sys, argparse

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="COM3")
ap.add_argument("--baud", type=int, default=115200)
ap.add_argument("--cmd", action="append", default=[])
ap.add_argument("--cmd-delay", type=float, default=0.3)
ap.add_argument("--deadline", type=float, default=15.0)
ap.add_argument("--out", default=None)
args = ap.parse_args()

ser = serial.Serial(args.port, args.baud, timeout=0.2)
time.sleep(0.3)
ser.reset_input_buffer()

buf = bytearray()


def drain(t):
    end = time.time() + t
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)


# wake prompt
ser.write(b"\r\n")
drain(0.3)

for c in args.cmd:
    ser.write(c.encode() + b"\r\n")
    drain(args.cmd_delay)

start = time.time()
while time.time() - start < args.deadline:
    chunk = ser.read(4096)
    if chunk:
        buf.extend(chunk)

ser.close()
text = bytes(buf).decode(errors="replace")
sys.stdout.write(text)
if args.out:
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)
