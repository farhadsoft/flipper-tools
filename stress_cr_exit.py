# stress_cr_exit.py — run from repo root: python stress_cr_exit.py
#
# Adapted from the plan's inline script. Two bugs fixed against the real
# firmware behavior (confirmed live over the CLI before this ran):
#
#   1. The plan's cmd list never sent "input send back short" -- it opened
#      the app, navigated to Card Reader, and just idled. The idle-exit
#      Back path (the actual bug under test) was never exercised.
#   2. The plan assumed Back-exit closes the whole FAP (so `loader open` /
#      `uptime` would work again next cycle). It doesn't: card_reader_exit
#      -> toolkit_exit_module only switches to the *toolkit's own* internal
#      launcher and frees the module; universal_toolkit.fap stays resident.
#      A repeat `loader open` on it then fails with "Loader is locked".
#      `uptime` also refuses ("cannot be run while an application is open")
#      as long as the toolkit is open, even sitting idle at its launcher.
#
# Corrected design: open the toolkit once, then loop down -> ok -> dwell ->
# back entirely within the toolkit (no re-launch needed/possible between
# cycles -- this is also a more faithful stress of reader_app_alloc/free
# back-to-back). Liveness is checked from the transcript itself (serial
# exceptions, "furi_check failed" text, missing prompt) every cycle, plus a
# full loader-close + uptime checkpoint every REPORT_INTERVAL cycles and on
# any suspected crash.
import os, sys, time, serial

PORT = "COM4"
BAUD = 115200
APP_PATH = "/ext/apps/Tools/universal_toolkit.fap"

# Args: cycles, comma-separated dwells (seconds), run tag (for distinct log names).
# Defaults reproduce the original sub-second sweep.
CYCLES = int(sys.argv[1]) if len(sys.argv) > 1 else 100
DWELLS = (
    [float(x) for x in sys.argv[2].split(",")]
    if len(sys.argv) > 2
    else [0.01, 0.05, 0.1, 0.2, 0.5]
)  # seconds -- idle dwell at the scan view
RUN_TAG = sys.argv[3] if len(sys.argv) > 3 else "cr_stress"
NAV_DELAY = 0.15  # settle time for down/ok menu navigation (not safety-critical)
POST_BACK_DRAIN = 1.5  # time to catch a furi_check dump / reboot after Back
REPORT_INTERVAL = 10

os.makedirs("logs", exist_ok=True)


def open_port():
    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    time.sleep(0.3)
    ser.reset_input_buffer()
    return ser


def drain(ser, buf, t):
    end = time.time() + t
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)


def send(ser, buf, cmd, delay):
    ser.write(cmd.encode() + b"\r\n")
    drain(ser, buf, delay)


def full_liveness_check(tag):
    """Fresh connection: close the app, confirm uptime answers. Returns
    (alive: bool, uptime_text: str, transcript: str)."""
    try:
        ser = open_port()
        buf = bytearray()
        send(ser, buf, "loader close", 0.4)
        send(ser, buf, "uptime", 0.4)
        ser.close()
    except Exception as e:
        return False, f"EXCEPTION: {e}", ""
    text = bytes(buf).decode(errors="replace")
    out = f"logs/{RUN_TAG}_{tag}_alive.log"
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    if "cannot be run" in text or "Uptime" not in text:
        return False, text, out
    return True, text, out


def reopen_app():
    ser = open_port()
    buf = bytearray()
    send(ser, buf, f"loader open {APP_PATH}", 0.6)
    ser.close()
    return bytes(buf).decode(errors="replace")


# ---- preflight: confirm idle, then open the toolkit once ----
alive, text, _ = full_liveness_check("preflight")
if not alive:
    print("PREFLIGHT FAILED: device not idle/alive before starting.")
    print(text)
    sys.exit(1)
print("Preflight OK:", text.strip().splitlines()[-1] if text.strip() else text)

boot = reopen_app()
if "cannot be run" in boot or "Loader is locked" in boot:
    print("PREFLIGHT FAILED: could not open universal_toolkit.fap")
    print(boot)
    sys.exit(1)

last_good_uptime_s = None


def parse_uptime_seconds(text):
    # "Uptime: 1h14m16s" (h/m may be absent at low uptime)
    import re

    m = re.search(r"Uptime:\s*(?:(\d+)h)?(?:(\d+)m)?(?:(\d+)s)?", text)
    if not m:
        return None
    h, mnt, s = (int(x) if x else 0 for x in m.groups())
    return h * 3600 + mnt * 60 + s


crashed = False
for i in range(CYCLES):
    dwell = DWELLS[i % len(DWELLS)]
    out = f"logs/{RUN_TAG}_{i:03d}.log"
    buf = bytearray()
    cycle_ok = True
    try:
        ser = open_port()
        send(ser, buf, "input send down short", NAV_DELAY)
        send(ser, buf, "input send ok short", dwell)
        send(ser, buf, "input send back short", POST_BACK_DRAIN)
        ser.close()
    except Exception as e:
        buf.extend(f"\n[EXCEPTION during cycle: {e}]\n".encode())
        cycle_ok = False

    text = bytes(buf).decode(errors="replace")
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)

    if "furi_check failed" in text:
        cycle_ok = False
    # The prompt echoes "\n>: " after every command; three sends -> expect
    # the prompt at least 3 times back. Fewer means the device stopped
    # responding partway through (reboot in flight).
    if text.count(">:") < 3:
        cycle_ok = False

    if not cycle_ok:
        print(f"CYCLE {i}: SUSPECTED CRASH (dwell={dwell}s) — log at {out}")
        print("--- tail ---")
        print(text[-800:])
        alive, atext, apath = full_liveness_check(f"{i:03d}_crash")
        print(f"Liveness re-check after suspected crash: alive={alive}")
        print(atext)
        if not alive:
            print(f"CONFIRMED CRASH at cycle {i}, dwell={dwell}s.")
            crashed = True
            break
        else:
            # False alarm (e.g. slow redraw) -- device answered uptime fine.
            # Re-open the app and continue, but keep the flagged log.
            print("Device is alive; treating as false alarm, resuming.")
            reopen_app()

    if (i + 1) % REPORT_INTERVAL == 0 and not crashed:
        alive, atext, apath = full_liveness_check(f"{i:03d}_checkpoint")
        u = parse_uptime_seconds(atext)
        mono_ok = (last_good_uptime_s is None) or (u is not None and u >= last_good_uptime_s)
        if u is not None:
            last_good_uptime_s = u
        print(
            f"  ... {i + 1}/{CYCLES} cycles completed, no crash. "
            f"alive={alive} uptime={u}s monotonic={mono_ok}"
        )
        if not alive or not mono_ok:
            print(f"CHECKPOINT FAILURE at cycle {i}: alive={alive} monotonic={mono_ok}")
            crashed = True
            break
        reopen_app()

    time.sleep(0.1)

if not crashed:
    alive, atext, _ = full_liveness_check("final")
    print(f"All {CYCLES} cycles completed without crash. Final check: alive={alive}")
    print(atext)
    sys.exit(0)
else:
    sys.exit(1)
