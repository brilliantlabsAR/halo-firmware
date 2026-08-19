# Halo device tests

Hardware-in-the-loop scripts that drive a real Halo over BLE. They are **not**
unit tests and are deliberately **not** wired into CI — every one of them needs
a device, and several need a human.

`run_tests.py` runs them as a battery against one dev kit.

## Before you run anything

1. **Use the dev kit.** `--name` is required — pass your Halo Dev Kit's BLE
   name (e.g. `"Halo AB"`). Point these at a real
   device only with a specific reason — several leave state behind.
2. **Flash and verify first**, so a failure means the test rather than a stale
   image:
   ```
   alif/.claude/skills/build/build.sh
   uv run alif/applications/halo/tools/ota_flash.py \
       build/halo/zephyr/zephyr.signed.bin --name "Halo AB" --yes
   uv run alif/applications/halo/tools/verify.py --name "Halo AB"
   ```
3. `uv` must be on `PATH`. Each test declares its own dependencies inline, so
   there is nothing to install — `uv run` resolves them per script.

## Running

```
./run_tests.py --name "Halo AB"            # the unattended set (default)
./run_tests.py --list                      # show the manifest, run nothing
./run_tests.py --only test_time.py         # one test, ignoring tag filters
./run_tests.py --skip test_microphone.py   # everything else
./run_tests.py --interactive               # add the ones needing a human
./run_tests.py -v                          # print the tail of failures
```

Individual scripts still run standalone, which is what you want while
debugging one:

```
uv run test_speaker_lc3.py --name "Halo AB"
```

Exit status is non-zero if any test failed, so it composes with a shell loop.

## What the tags mean

`run_tests.py --list` prints the tag for each test.

| tag | meaning |
|---|---|
| `auto` | terminates on its own; safe to leave running |
| `endless` | never returns — the runner bounds it and treats a clean timeout as a pass. **Nothing carries this tag now**: the polling tests take `--duration` instead. The mechanism is kept for future tests that genuinely cannot self-terminate |
| `interactive` | needs a human: a button press, a double tap, a noise, or Enter |
| `hazard` | can leave the device needing manual recovery — **opt in** with `--hazard`. **Nothing carries this tag now** (`test_light_sleep.py` lost it when light sleep stopped releasing the `SOFT_OFF` lock); kept for the next test that needs it |
| `broken` | known-failing today — **opt in** with `--broken`. **Nothing carries this tag now**; kept for the next test that needs it |

The default run is `auto` + `endless`. `interactive`, `hazard` and `broken` are
each excluded unless you ask for them.

## How a "pass" is decided

A test passes if it exits `0` and prints none of the failure markers
(`Lua error`, `FAILED:`, `Traceback`, `Not connected to device`). Most of these
scripts assert nothing and simply print what the device returned, so **this is a
smoke test**: it catches crashes, Lua errors and dropped connections, not wrong
values. Treat a green run as "nothing exploded", and read the output when you
care whether the values were right.

## Known-broken tests

**None right now.** Everything in the manifest either passes or is gated behind
`interactive`/`hazard`. The `broken` tag and its `--broken` opt-in remain for
the next test that needs them.

## Gotchas worth knowing

- **The two sleep modes wake differently, by design.** `frame.standby()`
  resumes in place — execution continues with the statement after the call.
  `frame.light_sleep()` restarts the VM — on wake a break hook throws
  `"interrupted"`, that propagates out of `require('main')`, and the REPL outer
  loop restarts the VM, so **`main.lua` runs again from the top**
  (`lua_runtime.c`). With light sleep, put nothing after the call that must
  run; read `frame.wakeup_source()` at the top of the script instead.

- **`await_print` returns only the FIRST notification of a reply**, then stops
  listening. Continuation packets of a longer message — a multi-line Lua error,
  say — arrive while the *next* command is already awaiting, and are handed back
  as that command's answer, shifting every result from there on. Most scripts
  here are still exposed to this. `test_file_api.py` and
  `test_file_execution.py` show the fix: end each command by printing its result
  fenced between unique start/end markers and wait for the fence, so a command
  cannot borrow another's output. Fence on *both* sides — a terminator alone
  still swallows whatever a running `main.lua` prints.
- **A newline terminates a REPL command.** Device-side helper functions have to
  be written on one line, or they arrive truncated.
- **`frame.bluetooth.send('')` transmits nothing.** `send()` chunks with
  `while (remaining > 0)`, so a zero-length payload never enters the loop — and
  still returns success. An end-of-stream marker has to be a non-empty payload
  (`test_bluetooth_throughput.py` uses a single `\0` byte).
- **A long-running loop can be driven from the REPL** rather than uploaded as
  `main.lua`, then stopped with a break signal. That avoids touching the
  device's own application entirely — preferable to the save/restore dance when
  the test does not specifically need `main.lua`.
- **Leftover `/lfs` files.** The microphone and AEC harnesses write recordings
  to the device. They accumulate and break anything asserting on directory
  contents. Assert on deltas and look entries up by name rather than by
  `listdir()` position; clear them between runs if a file test misbehaves.
- **Never overwrite or delete `main.lua` without putting it back.** The reset
  path runs whatever is there, so a test that needs a script at boot has to
  install its own — but that file is the device's *application*, and on a real
  device it is the user's. Use `preserve_main_lua()` from `halo_device_file.py`:

  ```python
  async with preserve_main_lua(b):
      await b.upload_file_from_string(my_script, "main.lua")
      await b.send_reset_signal()
      ...
  ```

  It restores the original even if the body raises, and cleans up with
  `send_remove_signal()` rather than a Lua-level `frame.file.remove` — the
  firmware handles that one, so it still works when a runaway loop has wedged
  the REPL. Deleting `main.lua` instead of restoring it is what made every
  battery run wipe the installed application.
- **Three tap gestures.** `frame.imu.tap_callback` arms single, double and
  triple tap; the callback receives the kind (`'single'|'double'|'triple'`).
  The detector is tunable via `frame.imu.tap_config` (resets on reboot).
  Clear the callback with `frame.imu.tap_callback(nil)` when done — that also
  disarms the hardware trigger, which otherwise keeps LPGPIO0 asserting into
  whatever runs next. The button and AAD callbacks take `nil` the same way.
- **`imu.raw()` and `imu.direction()` are in different frames.** `raw()` reports
  the raw device axes; `direction()` remaps to a host (right, forward, up)
  convention before computing tilt — `host_X = -dev.z`, `host_Y = dev.y`,
  `host_Z = dev.x` (`lua_imu.c`). Which `raw()` column carries gravity therefore
  depends on the device's orientation in its *own* frame, and comparing `raw()`
  against `direction()` (or against the Frame convention) misleads unless the
  remap is applied first.
- **Do not calibrate orientation against a dev kit.** Its IMU and magnetometer
  are mounted differently from a production unit, so the axis carrying gravity
  in a given physical pose differs. Both lying flat on a table:

  | | `imu.raw()` accel | `imu.direction()` |
  |---|---|---|
  | dev kit | `ax 17, ay -45, az +1007` | `roll ~ -89` |
  | production unit | `ax +981, ay +8, az +11` | `roll ~ -0.8, pitch ~ +0.8` |

  `direction()`'s remap targets the production mounting, so production-flat
  reads level and dev-kit-flat reads ~89° off. **That is expected, not a
  fault.** Check anything orientation- or heading-related on real hardware.
- **`imu.direction().heading` is a firmware stub** — a literal `0.0` pending
  magnetometer support (#252). Do not read anything into that column.
- **Interactive tests fail immediately under the runner** if you forget
  `--interactive`: they call `ainput()`, which raises `EOFError` when stdin is
  not a terminal.

## Future work

- **Real assertions.** The obvious next step is pytest: a session-scoped
  fixture owning the BLE connection, tests parametrised over device names, and
  `assert` instead of `print`. That is a rewrite of the scripts rather than a
  change to this runner, and it is deliberately out of scope here.
- **Machine-readable output** (JUnit XML or JSON) from `run_tests.py`, once
  there is something meaningful to report per test.
- **Parallelism is not possible** as things stand — one device, one BLE
  connection. Multiple dev kits would need the runner to shard by device name.
