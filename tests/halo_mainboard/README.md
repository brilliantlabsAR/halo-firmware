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
| `imu` | powers `sen_1v8`, polls the BMA580 chip ID (0xC4 at I2C0 0x18) until it answers, takes one sample; `ready_ms` is the power-up to first-ACK time | no ACK within 50 ms, wrong ID, sample error, or \|a\| outside 6.8–12.8 m/s² |
| `mag` | the same for the QMC6308 (0x80 at I2C0 0x2c) | no ACK within 50 ms, wrong ID, sample error, an all-zero reading, or an axis at 29 G or more (the ±30 G range edge). No magnitude window: a bare board's hard-iron offset can be many gauss |
| `mic` | 300 ms of PDM audio at gain 0 (the app's default) after 200 ms settle; AC RMS in raw counts | read error, or RMS outside 1–16000 (provisional). A missing mic reads a constant; a floating data line reads near full scale |
| `i2c1` | ACK from the display/camera flex parts: TPS65132 0x3e, PAG7982 0x40, VGA020 0x54 | never. SKIP when none answers, which is expected on a bare board |

The limits marked provisional need measuring on known-good boards before the
station relies on them.

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
