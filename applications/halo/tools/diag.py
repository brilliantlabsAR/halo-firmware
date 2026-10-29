# /// script
# requires-python = ">=3.10,<3.14"
# dependencies = ["brilliant-ble>=3.3.0,<4"]
# ///
"""
Read the diagnostic event log of a Halo running a DIAG=1 build, or change its
staged-boot settings. See HALO_DIAG_LOG and HALO_SLOW_BOOT in modules/halo/Kconfig.

    uv run diag.py --name "Halo 28"                      # print /lfs/diag.old + /lfs/diag.txt
    uv run diag.py --name "Halo 28" --out halo28.txt     # ... and save them
    uv run diag.py --name "Halo 28" --set slow=0 gap_ms=5000 --reboot
    uv run diag.py --name "Halo 28" --clear              # remove the log files

--set replaces /lfs/diag.cfg with the given key=value pairs (--set with no pairs
removes it, back to the defaults). Settings take effect at the next boot.

Like verify.py, it sends a break signal after connecting so a running main.lua
doesn't swallow the REPL, and resets the Lua VM afterwards (unless --reboot).
"""
import argparse
import asyncio
import sys

from brilliant_ble import BrilliantBle

CHUNK = 180
EOF_MARK = "<<diag-eof>>"


async def read_file(halo, path):
    """Contents of a file under /lfs (Lua paths are relative to it), or None."""
    ok = await halo.send_lua(
        f'local ok, f = pcall(frame.file.open, "{path}", "r") __d = ok and f or nil '
        f'print(__d and "open" or "none")',
        await_print=True, timeout=10)
    if ok != "open":
        return None
    out = []
    while True:
        chunk = await halo.send_lua(
            f'local s = __d:read({CHUNK}) print(s or "{EOF_MARK}")',
            await_print=True, timeout=10)
        if chunk == EOF_MARK:
            break
        out.append(chunk)
    await halo.send_lua('__d:close() __d = nil print(1)', await_print=True, timeout=10)
    return "".join(out)


async def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--name", required=True, help='exact BLE name, e.g. "Halo 28"')
    parser.add_argument("--out", help="also save the log to this file")
    parser.add_argument("--set", nargs="*", metavar="KEY=VALUE", help="replace /lfs/diag.cfg")
    parser.add_argument("--clear", action="store_true", help="remove /lfs/diag.txt and diag.old")
    parser.add_argument("--reboot", action="store_true", help="reboot the device when done")
    args = parser.parse_args()

    halo = BrilliantBle()
    await halo.connect(name=args.name)
    print(f"# connected to {args.name}", file=sys.stderr, flush=True)
    await halo.send_break_signal()
    await asyncio.sleep(0.3)
    try:
        fw = await halo.send_lua("print(frame.FIRMWARE_VERSION)", await_print=True, timeout=10)
        print(f"# firmware {fw}", file=sys.stderr, flush=True)

        if args.set is not None:
            pairs = [p for p in args.set if p.strip()]
            for p in pairs:
                k, _, v = p.partition("=")
                if not k.isidentifier() or not v.lstrip("-").isdigit():
                    sys.exit(f"bad setting {p!r}: want key=integer")
            if pairs:
                body = "\\n".join(pairs) + "\\n"
                await halo.send_lua(
                    f'local f = frame.file.open("diag.cfg", "w") f:write("{body}") f:close() print(1)',
                    await_print=True, timeout=10)
                print(f"# /lfs/diag.cfg: {' '.join(pairs)}", file=sys.stderr)
            else:
                await halo.send_lua('pcall(frame.file.remove, "diag.cfg") print(1)',
                                    await_print=True, timeout=10)
                print("# /lfs/diag.cfg removed (defaults)", file=sys.stderr)
        elif args.clear:
            await halo.send_lua('pcall(frame.file.remove, "diag.txt") '
                                'pcall(frame.file.remove, "diag.old") print(1)',
                                await_print=True, timeout=10)
            print("# log cleared", file=sys.stderr)
        else:
            cfg = await read_file(halo, "diag.cfg")
            print(f"# /lfs/diag.cfg: {cfg.split() if cfg else '(none: defaults)'}", file=sys.stderr)
            text = ""
            for path in ("diag.old", "diag.txt"):
                part = await read_file(halo, path)
                if part:
                    text += f"===== /lfs/{path}\n{part}"
            if not text:
                print("# no log: is this a DIAG=1 build?", file=sys.stderr)
            print(text, end="")
            if args.out:
                with open(args.out, "w", encoding="utf-8") as f:
                    f.write(text)
                print(f"# saved {args.out}", file=sys.stderr)
    finally:
        if args.reboot:
            try:
                await halo.send_lua("frame.reboot()", await_print=False)
            except Exception:
                pass  # the device drops the link as it reboots
            print("# rebooting", file=sys.stderr)
        else:
            await halo.send_reset_signal()
        try:
            await halo.disconnect()
        except Exception:
            pass


if __name__ == "__main__":
    asyncio.run(main())
