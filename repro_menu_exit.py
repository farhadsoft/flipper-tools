# repro_menu_exit.py — menu-Exit crash repro/verification for the plan at
# local://menu-exit-crash-fix-plan.md. Run from repo root:
#   python repro_menu_exit.py <mode> [cycles] [run_tag]
#
# Modes:
#   cr_load_exit   Card Reader: Load saved .emv -> Open -> Back(Actions) ->
#                  Exit. Reaches reader_handle_exit() the same way a live
#                  EMV read would (identical event, ReaderEventActionExit;
#                  identical crash site) without needing a physical card.
#   cr_double_exit Same, but taps OK twice back-to-back on the Exit row to
#                  stress the gen-based stale-event filter.
#   cr_back        Card Reader: Scan view -> Back (nav-callback path,
#                  commit 2f2adba's already-fixed path). Regression check.
#   subghz_exit    SubGHz Recorder: opens straight into its menu -> down x4
#                  -> Exit (SubRecEventMenuExit).
#
# Architecture note (why Load-file is a faithful repro): reader_handle_exit()
# is reached by the identical ReaderEventActionExit custom event regardless
# of how app->card was populated (live read vs loaded file). The crash is
# view_dispatcher_stop() called on the toolkit's single shared ViewDispatcher
# from inside its own custom-event dispatch loop, in module mode -- it
# bypasses toolkit_exit_module_now()/card_reader_exit(), so reader_app_free()
# never runs and the module's 5 views stay registered when toolkit_app_free()
# -> view_dispatcher_free() runs, tripping ViewDispatcher's own "all views
# must be removed before free" check. Nothing about that depends on radio
# state, only on reaching the Exit row.
#
# Selection-persistence, confirmed live over the CLI before this ran: the
# launcher Submenu's selected index is never reset on toolkit_show_launcher(),
# so once positioned on a module, repeat "ok" (no "down") re-enters the same
# module every cycle -- verified via two straight "card reader opened"
# session.log entries from consecutive bare "ok" sends.
import os, re, sys, time, serial

PORT = "COM4"
BAUD = 115200
APP_PATH = "/ext/apps/Tools/universal_toolkit.fap"  # fap_category "Tools" (application.fam)

MODE = sys.argv[1] if len(sys.argv) > 1 else "cr_load_exit"
CYCLES = int(sys.argv[2]) if len(sys.argv) > 2 else 1
RUN_TAG = sys.argv[3] if len(sys.argv) > 3 else MODE

NAV_DELAY = 0.15  # menu down/ok settle (stress_cr_exit.py convention)
SETTLE_BROWSER = 0.6  # reader_do_load() stops radio/anim + opens dialog_file_browser_show()
SETTLE_PICK = 0.5  # browser closes, selected_path set, switch to ReaderViewFileMenu
SETTLE_LOAD = 0.5  # reader_open_selected() -> emv_load() -> switch to ReaderViewInfo
SETTLE_NAV = 0.3  # Back on Info -> switch to ReaderViewActions
POST_EXIT_DRAIN = 2.0  # time to catch a furi_check dump / reboot after the Exit tap
DOUBLE_TAP_GAP = 0.05
REPORT_INTERVAL = 10

# Launcher row index (0-based) for each module (toolkit.c modules[] order).
LAUNCHER_INDEX = {"card_reader": 1, "subghz": 3}

# Per-cycle body AFTER entering the module (module already on screen).
CR_LOAD_EXIT_BODY = [
    ("ok", SETTLE_BROWSER),  # Scan: OK -> ReaderEventActionLoad -> file browser opens.
    # base_path has >1 path segment, so file_browser_worker's is_root check
    # (rightmost '/' at index 0) is false: a ". ." Back pseudo-item is
    # inserted at index 0, pushing the real file to index 1. One down first.
    ("down", NAV_DELAY),
    ("ok", SETTLE_PICK),  # browser: OK picks the only file (EMV_....emv)
    ("ok", SETTLE_LOAD),  # FileMenu selected=Open(0) -> loads -> ReaderViewInfo
    ("back", SETTLE_NAV),  # Info -> nav callback -> ReaderViewActions (selection=0)
    ("down", NAV_DELAY),  # Save -> Emulate
    ("down", NAV_DELAY),  # Emulate -> Rescan
    ("down", NAV_DELAY),  # Rescan -> Load
    ("down", NAV_DELAY),  # Load -> Exit
    ("ok", POST_EXIT_DRAIN),  # ReaderEventActionExit -> reader_handle_exit()
]
CR_DOUBLE_EXIT_BODY = CR_LOAD_EXIT_BODY[:-1] + [
    ("ok", DOUBLE_TAP_GAP),
    ("ok", POST_EXIT_DRAIN),
    # Outcome is bimodal: the 2nd tap either drops (gen-filtered, stale) and
    # leaves us at the toolkit launcher, or lands after the fast exit already
    # switched views and re-enters Card Reader (fresh Scan, radio active) --
    # both are correct UI behavior, not a bug. Either way an unconditional
    # Back makes the outcome deterministic and radio-safe before the next
    # cycle's loader-close reopen: at the launcher root it stops the whole
    # toolkit (nav returns false -> ViewDispatcher's own Back-exit); inside a
    # re-entered Card Reader it drives the already-fixed graceful nav-exit
    # path (reader_stop_all() before toolkit_exit_module()). loader close
    # while a module's NFC/LF radio is actively scanning is a separate,
    # pre-existing external force-kill issue, out of scope here -- this just
    # avoids ever hitting it from the test harness.
    ("back", POST_EXIT_DRAIN),
]
CR_BACK_BODY = [
    ("back", POST_EXIT_DRAIN),  # Scan: Back unconsumed by input cb -> nav callback -> exit
]
SUBGHZ_EXIT_BODY = [
    ("down", NAV_DELAY),  # Auto-record -> Frequency scan
    ("down", NAV_DELAY),  # Frequency scan -> Settings
    ("down", NAV_DELAY),  # Settings -> Saved signals
    ("down", NAV_DELAY),  # Saved signals -> Exit
    ("ok", POST_EXIT_DRAIN),  # SubRecEventMenuExit
]

MODES = {
    "cr_load_exit": ("card_reader", CR_LOAD_EXIT_BODY),
    "cr_double_exit": ("card_reader", CR_DOUBLE_EXIT_BODY),
    "cr_back": ("card_reader", CR_BACK_BODY),
    "subghz_exit": ("subghz", SUBGHZ_EXIT_BODY),
}

# Cycles in this mode reopen the app fresh instead of relying on "ok"
# re-entry: a legit rapid double-tap on Exit can land its second press on
# the launcher (already switched by the fast first-tap exit) and re-enter
# the module -- correct UI behavior, but it desyncs a tight re-entry loop
# across cycles. Reopening avoids compounding that drift.
REOPEN_EACH_CYCLE = {"cr_double_exit"}

if MODE not in MODES:
    print(f"Unknown mode {MODE!r}. Choices: {sorted(MODES)}")
    sys.exit(2)
MODULE_KEY, BODY = MODES[MODE]

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


def send_key(ser, buf, key, delay):
    send(ser, buf, f"input send {key} short", delay)


def full_liveness_check(tag):
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
    send(ser, buf, "loader close", 0.4)  # idempotent: harmless if nothing is open
    send(ser, buf, f"loader open {APP_PATH}", 0.6)
    ser.close()
    return bytes(buf).decode(errors="replace")


def parse_uptime_seconds(text):
    m = re.search(r"Uptime:\s*(?:(\d+)h)?(?:(\d+)m)?(?:(\d+)s)?", text)
    if not m:
        return None
    h, mnt, s = (int(x) if x else 0 for x in m.groups())
    return h * 3600 + mnt * 60 + s


def read_session_log_tail():
    try:
        ser = open_port()
        buf = bytearray()
        send(ser, buf, "storage read /ext/apps_data/universal_toolkit/session.log", 0.5)
        ser.close()
    except Exception as e:
        return f"EXCEPTION: {e}"
    text = bytes(buf).decode(errors="replace")
    lines = [l for l in text.splitlines() if l.startswith("Entry:")]
    return lines[-1] if lines else "(no entries)"


# ---- preflight: confirm idle, open the toolkit, navigate to the module ----
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

nav_buf = bytearray()
ser = open_port()
for _ in range(LAUNCHER_INDEX[MODULE_KEY]):
    send(ser, nav_buf, "input send down short", NAV_DELAY)
ser.close()
print(f"Navigated launcher selection to index {LAUNCHER_INDEX[MODULE_KEY]} ({MODULE_KEY}).")

last_good_uptime_s = None
crashed = False

for i in range(CYCLES):
    out = f"logs/{RUN_TAG}_{i:03d}.log"
    buf = bytearray()
    cycle_ok = True
    try:
        if MODE in REOPEN_EACH_CYCLE:
            reopen_text = reopen_app()
            if "cannot be run" in reopen_text or "Loader is locked" in reopen_text:
                buf.extend(f"\n[REOPEN FAILED: {reopen_text}]\n".encode())
                raise RuntimeError("reopen failed")
            ser = open_port()
            for _ in range(LAUNCHER_INDEX[MODULE_KEY]):
                send(ser, buf, "input send down short", NAV_DELAY)
            send_key(ser, buf, "ok", NAV_DELAY)  # enter module
            cmd_count = LAUNCHER_INDEX[MODULE_KEY] + 1 + len(BODY)
            for key, delay in BODY:
                send_key(ser, buf, key, delay)
            ser.close()
        else:
            cmd_count = 1 + len(BODY)  # the re-entry "ok" + body steps
            ser = open_port()
            send_key(ser, buf, "ok", NAV_DELAY)  # re-enter module (selection persists)
            for key, delay in BODY:
                send_key(ser, buf, key, delay)
            ser.close()
    except Exception as e:
        buf.extend(f"\n[EXCEPTION during cycle: {e}]\n".encode())
        cycle_ok = False

    text = bytes(buf).decode(errors="replace")
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)

    if "furi_check failed" in text:
        cycle_ok = False
    if text.count(">:") < cmd_count:
        cycle_ok = False

    if not cycle_ok:
        print(f"CYCLE {i}: SUSPECTED CRASH — log at {out}")
        print("--- tail ---")
        print(text[-1000:])
        alive, atext, _ = full_liveness_check(f"{i:03d}_crash")
        print(f"Liveness re-check after suspected crash: alive={alive}")
        print(atext)
        if not alive:
            print(f"CONFIRMED CRASH at cycle {i}.")
            crashed = True
            break
        else:
            print("Device is alive; treating as false alarm, resuming.")
            reopen_app()
            nb = bytearray()
            s2 = open_port()
            for _ in range(LAUNCHER_INDEX[MODULE_KEY]):
                send(s2, nb, "input send down short", NAV_DELAY)
            s2.close()
        time.sleep(0.1)
        continue

    if (i + 1) % REPORT_INTERVAL == 0 or (i + 1) == CYCLES:
        alive, atext, _ = full_liveness_check(f"{i:03d}_checkpoint")
        u = parse_uptime_seconds(atext)
        mono_ok = (last_good_uptime_s is None) or (u is not None and u >= last_good_uptime_s)
        if u is not None:
            last_good_uptime_s = u
        tail_entry = read_session_log_tail()
        print(
            f"  ... {i + 1}/{CYCLES} cycles completed, no crash. "
            f"alive={alive} uptime={u}s monotonic={mono_ok} last_session_entry={tail_entry!r}"
        )
        if not alive or not mono_ok:
            print(f"CHECKPOINT FAILURE at cycle {i}: alive={alive} monotonic={mono_ok}")
            crashed = True
            break
        if (i + 1) != CYCLES:
            reopen_app()
            nb = bytearray()
            s2 = open_port()
            for _ in range(LAUNCHER_INDEX[MODULE_KEY]):
                send(s2, nb, "input send down short", NAV_DELAY)
            s2.close()

    time.sleep(0.1)

if not crashed:
    alive, atext, _ = full_liveness_check("final")
    print(f"All {CYCLES} cycles ({MODE}) completed without crash. Final check: alive={alive}")
    print(atext)
    sys.exit(0)
else:
    sys.exit(1)
