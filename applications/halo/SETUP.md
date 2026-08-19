# Building Halo firmware

This guide takes you from a clean machine to a signed firmware image you can
flash to a Halo **over the air** — no cable or dev kit required. Flashing
itself is covered in [`FLASHING.md`](FLASHING.md).

If you only want to *run* the latest firmware (not modify it), skip the
toolchain entirely: download a `halo-firmware-<version>-release.signed.bin`
from the GitHub Releases page and go straight to `FLASHING.md`.

## 1. How the workspace fits together

This repository is a [west](https://docs.zephyrproject.org/latest/develop/west/index.html)
**manifest repository**. You don't clone it directly — `west init` does — and
it ends up as the `alif/` directory inside a larger workspace that west
assembles around it:

```
halo-firmware/            ← workspace root (created by you; not a git repo)
├── .venv/                ← Python venv with west (created by you, step 2)
├── alif/                 ← THIS repo: the Halo app + west manifest
├── zephyr/               ← Zephyr RTOS (Brilliant Labs fork, pinned)
├── modules/              ← HALs and libraries (pinned by the manifest)
├── bootloader/mcuboot/   ← MCUboot (Brilliant Labs fork, pinned)
└── build/                ← build output (created by the first build)
```

Everything outside `alif/` is fetched and pinned by `west update` from
`alif/west.yml`. You never commit to those trees; to change them, change the
manifest.

## 2. Prerequisites

You need: git, Python 3.10+, CMake, Ninja, and the Zephyr SDK toolchain.

### Linux (Ubuntu 22.04+ or similar)

```bash
sudo apt update
sudo apt install -y --no-install-recommends git python3-dev python3-venv \
    cmake ninja-build build-essential wget xz-utils file
```

### macOS

```bash
brew install python cmake ninja wget
```

### Python environment (all platforms)

Create a venv at the **workspace root** (the build script expects it at
`<workspace>/.venv`):

```bash
mkdir halo-firmware && cd halo-firmware
python3 -m venv .venv
source .venv/bin/activate
pip install west pyelftools intelhex cryptography click cbor2 'cmake<4.4' ninja
```

(CMake is pinned below 4.4: this Zephyr 3.6-era tree trips a `FindZephyr-sdk`
parse error on newer CMake unless `ZEPHYR_TOOLCHAIN_VARIANT` is exported —
see the build step. And use a venv rather than `pip install --user` —
modern distros refuse system-wide pip installs, per PEP 668.)

### Zephyr SDK 0.16.5 (the cross-compiler)

Download the **minimal** SDK for your OS/arch from the
[Zephyr SDK 0.16.5 release](https://github.com/zephyrproject-rtos/sdk-ng/releases/tag/v0.16.5),
e.g. on Linux x86-64:

```bash
cd ~/.local
wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.16.5/zephyr-sdk-0.16.5_linux-x86_64_minimal.tar.xz
tar -xf zephyr-sdk-0.16.5_linux-x86_64_minimal.tar.xz
cd zephyr-sdk-0.16.5
./setup.sh
```

(macOS: use the `macos-aarch64_minimal` or `macos-x86_64_minimal` tarball.)
When `setup.sh` asks which toolchains to install, you only need
**`arm-zephyr-eabi`**.

## 3. Get the source

From the workspace root created above, with the venv active:

```bash
west init -m https://github.com/brilliantlabsAR/halo-firmware
west update
```

The first `west update` fetches a few gigabytes (Zephyr and its modules);
subsequent runs are incremental. That's it — no patch steps, no extra
configuration: the manifest pins pre-patched forks of everything Halo needs.

## 4. Build

From the workspace root:

```bash
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr   # harmless everywhere; required on CMake ≥ 4.4
west build --sysbuild -b halo alif/applications/halo -p
```

This builds the app **and** the MCUboot bootloader, producing:

- `build/halo/zephyr/zephyr.signed.bin` — the signed app image. **This is
  the file you flash over the air.**
- `build/mcuboot/zephyr/zephyr.bin` — the bootloader (wired factory
  flashing only; you almost certainly don't need it).

Variants:

| Mode | Command | Log level |
|------|---------|-----------|
| Debug (default) | `west build --sysbuild -b halo alif/applications/halo -p` | DBG |
| Release | `RELEASE=1 west build --sysbuild -b halo alif/applications/halo -p` | INF |

Omit `-p` for fast incremental rebuilds. Note the build writes
`EXTRAVERSION` into `alif/applications/halo/VERSION` as a side effect —
don't commit that hunk.

### Image signing

Images are signed with the MCUboot key configured by the build
(`root-rsa-2048.pem` from the mcuboot tree — the standard MCUboot
development key). Halo devices accept images signed with this key, which is
what makes owner-built firmware flashable. The corollary: firmware signing
is **not** an anti-tamper control on this device; the BLE pairing/bonding
layer is the access control (see [`../../PAIRING.md`](../../PAIRING.md)).

### Building in the CI container (no local toolchain)

The CI image `ghcr.io/brilliantlabsar/halo-firmware-ci` carries the whole
build environment — SDK, tools, and a prebaked west workspace — so Docker
can replace sections 2–3 entirely. Clone this repo (plain `git clone` is
fine here; the container's workspace already exists), overlay it into the
workspace, and build:

```bash
git clone https://github.com/brilliantlabsAR/halo-firmware
docker run --rm -v "$PWD/halo-firmware:/host_code" \
  ghcr.io/brilliantlabsar/halo-firmware-ci:latest \
  /bin/bash -c '
    rsync -a /host_code/ /opt/workspace/project/ &&
    cd /opt/workspace/project &&
    west update &&
    export ZEPHYR_TOOLCHAIN_VARIANT=zephyr &&
    west build --sysbuild -b halo applications/halo -p &&
    cp build/halo/zephyr/zephyr.signed.bin /host_code/'
```

The signed OTA image ends up back in your clone as `zephyr.signed.bin`.
This is exactly what CI does on every PR (see
[`.github/CI.md`](../../.github/CI.md)). The image is linux/amd64; on Apple
silicon Docker runs it under emulation — it works, just slower than a
native toolchain.

## 5. Flash it

Over the air, in a couple of minutes, with automatic rollback if the new
image doesn't boot: see [`FLASHING.md`](FLASHING.md).

## 6. Where to go next

- [`PROTOCOL.md`](PROTOCOL.md) — the BLE protocol (Lua REPL + data channels).
- [`LUA_RUNTIME.md`](LUA_RUNTIME.md) — how the Lua app runtime works.
- [`BLE_SERVICES.md`](BLE_SERVICES.md) — GATT services reference.
- [`BUTTON_LED_GUIDE.md`](BUTTON_LED_GUIDE.md) — buttons, LEDs, ship mode.
- [`PM_SLEEP.md`](PM_SLEEP.md) — power management.
- `alif/samples/halo/` — small single-peripheral sample programs
  (`west build -b halo alif/samples/halo/t5838 -p` etc.).

---

## Appendix: wired flashing (dev kits only)

Normal Halo devices are sealed units — everything above this line is done
over the air. The wired path exists for development boards with the SE-UART
debug connector broken out (first-time factory programming, bootloader
updates, unbricking).

It uses the **Alif Security Toolkit** (SETOOLS), downloaded from Alif's
[Ensemble software & tools page](https://alifsemi.com/support/software-tools/ensemble/)
(the Security Toolkit User Guide is available from the same page).

One-time tool configuration:

```bash
cd app-release-exec-linux
cp <workspace>/alif/applications/halo/configs/* build/config/
./tools-config       # interactive: Part# → Balletto → B1 (AB1C1F4M51820PH), SE-UART interface
```

Flash app + bootloader (app **first**, bootloader second):

```bash
cp <workspace>/build/halo/zephyr/zephyr.signed.bin ./build/images/zephyr-app.signed.bin
./app-gen-toc -f build/config/zephyr-app-cfg.json
./app-write-mram -p -nr

cp <workspace>/build/mcuboot/zephyr/zephyr.bin ./build/images/
./app-gen-toc -f build/config/zephyr-cfg.json
./app-write-mram -p -nr
```

`./maintenance -d` selects the serial port (SE-UART, typically
`/dev/ttyUSB0`). The second UART carries console logs at 115200 8N1
(`minicom -D /dev/ttyUSB1 -w`). To reboot a dev board: hold the button and
connect USB-C, or short RST to GND on the debug board.
