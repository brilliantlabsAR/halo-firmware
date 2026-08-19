---
name: build
description: Build the Halo firmware (Zephyr/west) — incremental, pristine, or release. Produces the signed BLE-OTA image build/halo/zephyr/zephyr.signed.bin. Use whenever firmware needs compiling.
---

# Build Halo firmware

Run the bundled script — it works from any directory (it locates the West
workspace root and venv itself):

```
alif/.claude/skills/build/build.sh              # incremental (default — use for iteration)
alif/.claude/skills/build/build.sh pristine     # full rebuild (west build --sysbuild -p)
RELEASE=1 alif/.claude/skills/build/build.sh    # release build (default is debug)
```

Output: the signed app image (the BLE-OTA payload) is
`build/halo/zephyr/zephyr.signed.bin` at the workspace root. Flash it with the
`flash` skill.

## Notes

- **Changing a Kconfig `default` needs a pristine build.** The build directory
  caches `.config`; incremental builds keep the previously generated values, so
  a changed default silently doesn't take (a changed `prj.conf` does trigger
  reconfiguration). After editing defaults in any Kconfig, run `pristine` and
  verify with `grep <SYMBOL> build/halo/zephyr/.config` before flashing.

- **`applications/halo/VERSION` is build noise**: debug builds write
  `EXTRAVERSION = debug`; `RELEASE=1` builds clear it. Never commit that hunk.
- A pristine build takes a few minutes; run it in the background. Incremental
  builds are usually well under a minute.
- Build failures about "CMake is not installed" mean the workspace venv wasn't
  on PATH — the script handles this; if building manually, prepend
  `$WS_ROOT/.venv/bin` to PATH.
- Full toolchain/environment setup (first time on a machine):
  `alif/applications/halo/SETUP.md`.
