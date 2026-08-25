# Validating the ble_lua RX write-handler fix

What changed: `LUA_IDX_RX_VAL` and `LUA_IDX_AUDIO_RX_VAL` now set `NO_OFFSET`,
and `on_att_val_set` refuses offset writes, ignores zero-length writes, no
longer mutates `len`, and appends exactly one `\n` per REPL write. The dropped
continuation state machine (`current_rx_ring`) is gone. This is a behaviour
change on two public characteristics, so the point of this sweep is to prove
the normal REPL, data, and audio write paths still work.

## 1. Host unit test (no hardware)

```sh
cd tests/bluetooth/ble_lua_rx
make run      # production logic: all checks pass
make repro    # pre-fix logic: AddressSanitizer aborts on the underflow
```

## 2. On-device edge cases (Halo AB, unattended)

```sh
cd applications/halo/tests
uv run test_ble_rx_write.py --name "Halo AB"
```

Covers zero-length writes, a zero-length burst, single-line framing, and an
over-MTU (long) write being refused while the device stays responsive.

## 3. SDK regression sweep (Halo AB, unattended)

From the SDK checkout, package dir
`python/packages/brilliant_ble`. Standalone scripts run with `uv run`; the
host-only chunking test runs under pytest.

Data / RX-write path (the paths this change touches most):

```sh
uv run tests/test_file_api.py --name "Halo AB"            # send_data file writes
uv run tests/test_file_execution.py --name "Halo AB"      # upload + execute
uv run tests/test_compression.py --name "Halo AB"         # send_data + compression
uv run tests/test_bluetooth_callback_api.py --name "Halo AB"  # send_data round-trip
uv run --with pytest pytest tests/test_chunk_lua_string.py    # host-only, chunking
```

REPL (`send_lua`) smoke — bounded, judge by exit code:

```sh
uv run tests/test_text_api.py --name "Halo AB"    # argument validation, asserts
uv run tests/test_version.py --name "Halo AB"
uv run tests/test_time.py --name "Halo AB"
uv run tests/test_display.py --name "Halo AB"
uv run tests/test_display_bitmap.py --name "Halo AB"
```

Streaming / monitor tests — these keep the connection open and print until
interrupted, so under a timeout they exit 124 *by design*. Judge them by
whether they connect and stream, not by exit code:

```sh
uv run tests/test_aad.py --name "Halo AB"            # idles waiting for sound
uv run tests/test_imu_direction.py --name "Halo AB"  # streams roll/pitch/heading
uv run tests/test_imu_raw.py --name "Halo AB"        # streams accel/compass
```

## Excluded — need a human, an observer, or a rig

`test_bluetooth_throughput` (waits on Enter), `test_camera` (waits on Enter +
image review), `test_button`, `test_taps` (physical input),
`test_speaker_pcm` / `test_speaker_lc3` / `test_speaker_lc3_2` (need a
listener), `test_microphone` / `test_microphone_lc3` (need sound to verify).
The speaker/mic scripts still make a good manual check that the audio RX/TX
characteristics survive `NO_OFFSET`, since `send_audio` writes to
`LUA_IDX_AUDIO_RX_VAL`.
