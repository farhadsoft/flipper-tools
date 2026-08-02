import os, serial, time, sys, argparse

ap = argparse.ArgumentParser()
ap.add_argument("--port", default="COM3")
ap.add_argument("--baud", type=int, default=115200)
ap.add_argument("--cmd", action="append", default=[])
ap.add_argument("--cmd-delay", type=float, default=0.3)
ap.add_argument("--deadline", type=float, default=15.0)
ap.add_argument("--out", default=None)
args = ap.parse_args()

# Capture transcripts always land in logs/, never at the repo root
# (CLAUDE.md "Log capture"). --out omitted -> logs/cap_<timestamp>.log;
# a bare --out filename with no directory part is redirected into logs/;
# an --out that carries a path separator (relative or absolute) is used
# verbatim. logs/ is relative to the CWD, and cap.py is run from the repo
# root; running it from elsewhere just makes a logs/ there, still gitignored.
out_path = args.out or time.strftime("cap_%Y%m%d_%H%M%S.log")
if not os.path.dirname(out_path):
    out_path = os.path.join("logs", out_path)
os.makedirs(os.path.dirname(out_path), exist_ok=True)
sys.stderr.write("[cap] transcript -> %s\n" % out_path)

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
with open(out_path, "w", encoding="utf-8") as f:
    f.write(text)
