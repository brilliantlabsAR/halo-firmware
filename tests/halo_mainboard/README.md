# Halo main-board factory test

A lean test image for the flashing station. It runs on a bare Halo main PCB
(no display, camera, speaker or button), tests what the main board carries,
and reports machine-parseable results on the console. The station writes it
raw at `0x80000000` over SE-UART, the same way as `tests/halo` and MCUboot:
it has no MCUboot header and no imgtool signature. Afterwards the station
writes the production MCUboot and app.

```
west build -b halo alif/tests/halo_mainboard -d build-mainboard-test -p
# build-mainboard-test/zephyr/zephyr.bin
```

The first cut is about 110 KB, against 658 KB for `tests/halo`. SE-UART writes
about 15 KB/s.

## Output

Console UART at 115200 8N1 (the board default). The tests run once at boot:

```
FT BEGIN halo-mainboard-test <version> <commit>
FT <test> PASS|FAIL|SKIP <detail>
...
FT DONE <passed>/<total> skip=<skipped>
```

- `<total>` counts PASS and FAIL. SKIP means not fitted or not applicable, and
  never fails the board.
- `<detail>` is `key=value` tokens. The one quoted value is the BLE name.
- The shell prompt is empty, so FT lines start at column 0. Driver logs never
  start with `FT `. Parse only lines that match `^FT `.
- The shell stays up: `factory run` repeats every test, and `factory <test>`
  repeats one test (within a BEGIN/DONE pair).

## Tests, in order

| Test | Checks | FAIL when |
|---|---|---|
| `se` | Secure Enclave answers: part number and SE firmware revision | a service call fails |
| `eui` | the per-unit EUI-48 extension from OTP, and the BLE address the app will use | the call fails or the extension is 000000 (the app would then fall back to a shared address) |
| `clocks` | CPU cycles (HFXO-derived) counted across 0.5 s of LFXO-driven RTC ticks | the LF clock is not running, or the ratio is off by 500 ppm or more (provisional) |
| `ble_adv` | the BLE stack comes up and advertises `name="Halo XXYYZZ"` from the unit's static address, where XXYYZZ is the EUI extension; `adv=on` once started | any stack step fails or times out (3 s), or advertising is not running |
| `ram` | 256 KB of SRAM with address, inverted-address, 0x55 and 0xAA patterns | any word reads back wrong |
| `mram_image` | CRC-32 (IEEE, as zlib) over the image as it sits in MRAM; `len` equals the `.bin` size | never. **The station must compare `crc32` with the CRC-32 of the `.bin` it wrote** |
| `mram_write` | writes, reads back and erases 1 KB at the start of slot1 (free at this stage) | an I/O error or a mismatch |
| `vbat` | battery ADC voltage, state of charge, charger state pin | voltage outside 3000–4500 mV |
| `imu` | powers `sen_1v8`, polls the BMA580 chip ID (0xC4 at I2C0 0x18) until it answers, then waits for the first real sample (the chip reads 0x8000, i.e. −2 g on every axis, until then); units are mg; `ready_ms` is the power-up to first-ACK time | no ACK within 50 ms, wrong ID, sample error, no data within 100 ms, or \|a\| outside 700–1300 mg, or three bit-identical samples (stuck) |
| `mag` | the same for the QMC6308 (0x80 at I2C0 0x2c) | no ACK within 50 ms, wrong ID, sample error, an all-zero reading, an axis at 29 G or more (the ±30 G range edge), or three bit-identical samples (stuck). No magnitude window: a bare board's hard-iron offset can be many gauss |
| `mic` | both PDM channels (ch2 is the app's mono channel, ch3 the other clock edge), 300 ms at 16 kHz and gain 0 after 200 ms settle, in 20 ms blocks as the app uses (the driver's DMA fills at most 512 frames per block). Per channel: `rms`, `lp` (4-tap moving average), `tone` (per mille of AC power in 900–1150 Hz; a loud 1 kHz tone at the mic gives ~700 on board #5, quiet ~1), `min`, `max`, `zero` (exact-zero samples; few in a real stream) and `alive` (rms 20–4000, not stuck). Also `pair` (`identical` on a bare board: the absent second mic's edge is undriven, so it mirrors the main one) and `rate_hz` (delivered sample rate, ~16000) | ch2 not alive, or a capture error. With a 1 kHz stimulus at the station, `ch2_tone` ≥ 300 also proves the mic hears |
| `i2c1` | ACK from the display/camera flex parts: TPS65132 0x3e, PAG7982 0x40, VGA020 0x54 | never. SKIP when none answers, which is expected on a bare board |

The limits marked provisional need measuring on known-good boards before the
station relies on them.

### Mic baselines (cut 7, board F4-18-AB, 2026-10-01)

| condition | ch2 rms | ch2 tone | notes |
|---|---|---|---|
| quiet | 152–168 | 0–1 | raw PCM sits ~+850 DC, never crosses zero |
| speech | 176–737 | 0–30 | |
| 1 kHz tone at the board | 901–1232 | 949–973 | |

- `rate_hz` = 16000 and `zero` = 0 when quiet; `pair` = identical (second
  mic not fitted).
- The DC offset is expected: the board DTS sets `iir-bypass`, which bypasses
  the PDM's DC-blocking filter. `rms` is mean-removed, so it is unaffected.
- Every small-signal sample is a multiple of 22. That is the gain, not a
  fault: gain 0 writes `((0+1)*22)<<4` to the channel gain register.
- Alive window 20–4000 has wide margin. A station with a 1 kHz source can
  additionally require `ch2_tone >= 500` (quiet 0–1, speech ≤ 30, tone ≥ 949).

## Clock stream

`factory clockstream [seconds]` streams one line per LF second (0 or no argument
means until reset). `factory clockstream stop` stops it, and `factory run` /
`factory <test>` stop it too. The boot run does not start it:

```
FT clk seq=<n> cyc=<u64> lf=<u64> lf_src=lfxo cpu_hz=160000000 gap=<cycles>
```

- `cyc` is the DWT cycle counter. The core clock is PLL clk1 locked to the
  38.4 MHz HFXO, so its rate carries the HFXO's ppm. `lf` is the LPRTC
  (32.768 kHz LFXO). Both are extended to 64 bits in software.
- Lines are taken on an RTC tick edge, so `lf` is exact and `cyc` lands within
  `gap` cycles after it. Fit both against host time to get each crystal's
  absolute error.
- While streaming, the core never sleeps (CYCCNT stops in WFI), so do not
  judge current draw from a streaming board.
- `FT clk` lines match `^FT ` but carry no PASS/FAIL. A station that parses
  test output should stop at `FT DONE`.

## Station BLE check

After `FT ble_adv PASS`, the DUT advertises indefinitely: connectable, 100 ms
interval, 0 dBm, complete local name only. The station scans for the exact
name from the `ble_adv` line (or the address, where its BLE stack exposes
addresses) and applies its own RSSI threshold. The advertiser keeps running
after `FT DONE`, so the scan can happen after the DUT has finished.

The `ble_adv` line ends in `adv=on` once the stack has confirmed advertising
started. From then on the DUT prints a heartbeat every 5 s:

```
BLE adv alive uptime=<s>s adv=on|off ctrl=ok|no-reply hci=<ver>.<subver>
```

Each beat round-trips to the BLE controller (an HCI version read), so
`ctrl=ok` shows the whole stack is still up while the station or a phone
scans. `adv=off` means the stack reported the advertising set stopped. The
heartbeat is not an FT line.
