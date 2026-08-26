# ble_lua RX write-handler tests

Host-side (macOS/Linux) reproduction and regression test for the Lua RX GATT
write handler in `modules/halo/src/ble_lua.c` (`on_att_val_set`,
`LUA_IDX_RX_VAL` / `LUA_IDX_AUDIO_RX_VAL`).

```sh
make run     # production logic: all checks pass (exit 0)
make repro   # pre-fix logic: AddressSanitizer aborts on the underflow
```

## What it covers

The handler body is transcribed into `main.c` and driven with the
`(offset, data, len)` triples an ATT write hands the callback. Each payload
lives in a heap buffer of **exactly** `len` bytes, and a faithful stand-in for
Zephyr's `ring_buf_put` reproduces its one load-bearing property — it copies
`MIN(size, free)` bytes and reads them from the source pointer. Compiled with
ASan + UBSan, so an over-read is caught rather than silently tolerated.

- Zero-length write (REPL and data current ring) stores nothing.
- A REPL statement arrives as exactly one `\n`-terminated line — no lost byte,
  no spurious interior newline.
- An offset (long-write) fragment is refused with `ATT_ERR_INVALID_OFFSET` and
  leaves no partial state.
- A framed data write stores the payload with the `0x01` marker stripped.
- An unpaired write is rejected.

`make repro` builds the pre-fix logic (`-DVULNERABLE`, `main` @ b4f0dcd): a
zero-length continuation write reaches `ring_buf_put(ring, data, len - 1)` with
`len == 0`, so `len - 1` promotes to `0xFFFFFFFF` and the copy drains the
ring's free space out of a zero-length source. The target aborts under
AddressSanitizer; a clean exit is treated as a test failure.

## Scope

This is a fast, hardware-free guard on the length arithmetic and routing. It
transcribes the handler rather than compiling it (the production function pulls
in the Alif GATT stack, `co_buf`, and `k_sem`), so it does not prove the
transcription stays in step with the source. Whether the Alif ROM actually
forwards a zero-length write or an offset write to the callback in the first
place is an end-to-end question covered on a dev kit by
`applications/halo/tests/test_ble_rx_write.py`.
