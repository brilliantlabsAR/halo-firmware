#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = []
# ///
"""Run the Halo device tests as a battery against one dev kit.

These are hardware-in-the-loop scripts, not unit tests: they drive a real
device over BLE and most of them assert nothing. This runner exists to make
them runnable in one go and to record what is safe to run unattended.

A test "passes" here if it exits 0 and prints no failure marker (see
FAILURE_MARKERS). That is a smoke test, not a real assertion — see the
"Future work" section of README.md.

    ./run_tests.py --name "Halo AB"              # the unattended set
    ./run_tests.py --name "Halo AB" --list       # show the manifest, run nothing
    ./run_tests.py --name "Halo AB" --only test_time.py,test_camera.py
    ./run_tests.py --name "Halo AB" --interactive   # include prompts for a human
"""

import argparse
import asyncio
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).parent

# Tags:
#   auto        terminates on its own, safe unattended
#   endless     never returns; run for `timeout` then kill, that is a pass
#   interactive needs a human (button press, tap, noise, or an Enter keypress)
#   hazard      can leave the device in a state needing manual recovery
#   broken      known-failing today; see `note`. Skipped unless --broken.
MANIFEST = {
    "test_version.py":                dict(timeout=45,  tags={"auto"}),
    "test_battery.py":                dict(timeout=60,  tags={"auto"},
                                           note="polls for --duration (default 30 s)"),
    "test_time.py":                   dict(timeout=120, tags={"auto"}),
    "test_compression.py":            dict(timeout=90,  tags={"auto"}),
    "test_camera.py":                 dict(timeout=120, tags={"interactive"},
                                           note="waits on Enter before capture"),
    "test_display.py":                dict(timeout=120, tags={"auto"}),
    "test_display_bitmap.py":         dict(timeout=200, tags={"auto"}),
    "test_text_api.py":               dict(timeout=200, tags={"auto"}),
    "test_speaker_pcm.py":            dict(timeout=120, tags={"auto"}),
    "test_speaker_lc3.py":            dict(timeout=120, tags={"auto"}),
    "test_speaker_lc3_2.py":          dict(timeout=120, tags={"auto"}),
    "test_microphone.py":             dict(timeout=180, tags={"auto"}),
    "test_microphone_lc3.py":         dict(timeout=120, tags={"auto"}),
    "test_bluetooth_callback_api.py": dict(timeout=90,  tags={"auto"}),
    "test_imu_raw.py":                dict(timeout=60,  tags={"auto"},
                                           note="polls for --duration (default 30 s); "
                                                "columns are RAW DEVICE axes"),
    "test_imu_direction.py":          dict(timeout=60,  tags={"auto"},
                                           note="polls for --duration (default 30 s); "
                                                "heading is a stub, always 0.0 (#252)"),

    "test_button.py":                 dict(timeout=90,  tags={"interactive"},
                                           note="press the button; runs for --duration "
                                                "(default 60 s)"),
    "test_taps.py":                   dict(timeout=120, tags={"interactive"},
                                           note="DOUBLE-tap the device (single will not fire)"),
    "test_aad.py":                    dict(timeout=90,  tags={"interactive"},
                                           note="make a loud noise (>=90 dB); runs for "
                                                "--duration (default 60 s)"),
    "test_standby.py":                dict(timeout=180, tags={"interactive"},
                                           note="waits on Enter"),

    "test_file_execution.py":         dict(timeout=120, tags={"auto"},
                                           note="installs its own main.lua, then restores "
                                                "the original in a finally block"),
    "test_file_api.py":               dict(timeout=120, tags={"auto"}),
    "test_bluetooth_throughput.py":   dict(timeout=90,  tags={"auto"},
                                           note="measures for --duration (default 20 s)"),
    "test_light_sleep.py":            dict(timeout=120, tags={"auto"},
                                           note="asserts the VM-restart wake semantics"),
}

# Substrings that mean the run failed even when the exit status is 0 — these
# scripts mostly report problems by printing them.
FAILURE_MARKERS = (
    "Lua error",
    "FAILED:",
    "Traceback (most recent call last)",
    "Not connected to device",
)

GREEN, RED, YELLOW, DIM, RESET = "\033[92m", "\033[91m", "\033[93m", "\033[2m", "\033[0m"


def classify(name, cfg, args):
    """Return None to run, or a string reason to skip."""
    tags = cfg["tags"]
    if args.only:
        return None if name in args.only else "not in --only"
    if name in args.skip:
        return "in --skip"
    if "broken" in tags and not args.broken:
        return "known-broken (--broken to run)"
    if "hazard" in tags and not args.hazard:
        return "hazardous (--hazard to run)"
    if "interactive" in tags and not args.interactive:
        return "interactive (--interactive to run)"
    return None


async def run_one(name, cfg, device, timeout):
    """Run one test as a subprocess; return (ok, seconds, detail)."""
    endless = "endless" in cfg["tags"]
    started = time.monotonic()
    proc = await asyncio.create_subprocess_exec(
        "uv", "run", str(HERE / name), "--name", device,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT,
    )
    timed_out = False
    try:
        out, _ = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    except asyncio.TimeoutError:
        timed_out = True
        proc.kill()
        out, _ = await proc.communicate()
    elapsed = time.monotonic() - started
    text = (out or b"").decode("utf-8", "replace")

    hits = sorted({m for m in FAILURE_MARKERS if m in text})
    if hits:
        return False, elapsed, f"printed {', '.join(repr(h) for h in hits)}", text
    if timed_out:
        # Reaching the time limit is the expected end for an endless test.
        if endless:
            return True, elapsed, f"ran {elapsed:.0f}s, no errors", text
        return False, elapsed, f"timed out after {timeout}s", text
    if proc.returncode != 0:
        return False, elapsed, f"exit {proc.returncode}", text
    return True, elapsed, "", text


async def main_async(args):
    if not shutil.which("uv"):
        sys.exit("uv not found on PATH - see README.md")

    selected, skipped = [], []
    for name, cfg in MANIFEST.items():
        reason = classify(name, cfg, args)
        (skipped if reason else selected).append((name, cfg, reason))

    if args.list:
        for name, cfg in MANIFEST.items():
            tags = ",".join(sorted(cfg["tags"]))
            note = cfg.get("note", "")
            print(f"  {name:32s} {tags:28s} {note}")
        return 0

    print(f"Device: {args.device}   running {len(selected)}, "
          f"skipping {len(skipped)}\n")

    results = []
    for name, cfg, _ in selected:
        timeout = args.timeout or cfg["timeout"]
        print(f"{DIM}-- {name} (limit {timeout}s){RESET}")
        ok, secs, detail, text = await run_one(name, cfg, args.device, timeout)
        results.append((name, ok, secs, detail))
        mark = f"{GREEN}PASS{RESET}" if ok else f"{RED}FAIL{RESET}"
        print(f"   {mark} {secs:5.1f}s {detail}")
        if not ok and args.verbose:
            for line in text.splitlines()[-25:]:
                print(f"      {DIM}{line}{RESET}")
        # let the device settle and finish advertising again
        await asyncio.sleep(args.settle)

    print("\n" + "=" * 62)
    failed = [r for r in results if not r[1]]
    for name, ok, secs, detail in results:
        mark = f"{GREEN}PASS{RESET}" if ok else f"{RED}FAIL{RESET}"
        print(f"  {mark}  {name:34s} {secs:5.1f}s  {detail}")
    for name, cfg, reason in skipped:
        print(f"  {YELLOW}SKIP{RESET}  {name:34s}        {reason}")
    print(f"\n{len(results) - len(failed)}/{len(results)} passed"
          f"{', ' + str(len(failed)) + ' failed' if failed else ''}")
    return 1 if failed else 0


def parse_args():
    p = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Flash and verify the dev kit before running - see README.md.",
    )
    p.add_argument("--name", dest="device", required=True,
                   help='exact BLE device name, e.g. "Halo AB" — point it at your dev kit')
    p.add_argument("--list", action="store_true",
                   help="print the manifest and exit")
    p.add_argument("--only", default="",
                   help="comma-separated tests to run, ignoring tag filters")
    p.add_argument("--skip", default="",
                   help="comma-separated tests to skip")
    p.add_argument("--interactive", action="store_true",
                   help="include tests needing a human")
    p.add_argument("--hazard", action="store_true",
                   help="include tests that can leave the device needing recovery")
    p.add_argument("--broken", action="store_true",
                   help="include tests known to fail today")
    p.add_argument("--timeout", type=int, default=None,
                   help="override every per-test time limit (seconds)")
    p.add_argument("--settle", type=float, default=3.0,
                   help="seconds to wait between tests (default: 3)")
    p.add_argument("-v", "--verbose", action="store_true",
                   help="print the tail of a failing test's output")
    args = p.parse_args()
    args.only = {s.strip() for s in args.only.split(",") if s.strip()}
    args.skip = {s.strip() for s in args.skip.split(",") if s.strip()}
    unknown = (args.only | args.skip) - set(MANIFEST)
    if unknown:
        p.error(f"unknown test(s): {', '.join(sorted(unknown))}")
    return args


if __name__ == "__main__":
    sys.exit(asyncio.run(main_async(parse_args())))
