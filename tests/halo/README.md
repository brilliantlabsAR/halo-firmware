# 📋 Zephyr Shell Command Reference Manual

This document provides a complete reference for all shell commands available in the system, covering audio, sensors, display, LED, and camera control. All commands are case-sensitive and require proper initialization before use.

---

## 🔊 Speaker Commands

Control PWM-based audio playback.

| Command | Syntax | Description |
|--------|--------|-------------|
| `get_device` | `speaker get_device` | Show speaker device name |
| `init` | `speaker init` | Initialize speaker (8kHz, 16-bit mono) |
| `deinit` | `speaker deinit` | Stop and deinitialize speaker |
| `play` | `speaker play <id> [loop]` | Play built-in sample (`0`: voice, `1`: tone) |
| `stop` | `speaker stop` | Stop playback |
| `volume` | `speaker volume <level>` | Set volume (0–100) |

### ✅ Example
```sh
speaker init
speaker volume 80
speaker play 0           # Play voice
speaker play 1 loop      # Loop tone
speaker stop
```

> 📌 Must call `init` first. `loop` enables continuous playback.

---

## 🎤 Microphone (DMIC) Commands

Control PDM digital microphone recording.

| Command | Syntax | Description |
|--------|--------|-------------|
| `get_device` | `micphone get_device` | Show microphone device name |
| `init` | `micphone init` | Initialize microphone (8kHz, 16-bit, stereo) |
| `deinit` | `micphone deinit` | Stop and deinitialize mic |
| `record` | `micphone record` | Start recording (prints PDM samples) |
| `stop` | `micphone stop` | Stop recording |

### ✅ Example
```sh
micphone init
micphone record
# r: 0123 4567 89AB CDEF 0001
micphone stop
```

> 📌 `record` prints raw 16-bit PDM data (5 samples per line). Uses memory slab for streaming.

---

## 📡 Sensor Commands

Read data from on-board sensors.

| Command | Syntax | Description |
|--------|--------|-------------|
| `sensor get qmc6308@2c` | `sensor get qmc6308@2c` | Read magnetometer (I2C 0x2C) |
| `sensor get bma580@18` | `sensor get bma580@18` | Read accelerometer (I2C 0x18) |
| `sensor get vbat` | `sensor get vbat` | Read battery voltage |

### ✅ Example
```sh
sensor get qmc6308@2c    # Output: x: 12.3, y: -4.5, z: 48.2 [µT]
sensor get bma580@18     # Output: x: 0.1, y: 0.0, z: 9.8 [m/s²]
sensor get vbat          # Output: 3.78 V
```

> 📌 All values are in physical units (µT, m/s², V).

---

## 🖥️ Display Commands

Control MIPI DSI display with canvas drawing support.

| Command | Syntax | Description |
|--------|--------|-------------|
| `get_device` | `display get_device` | Show display device name |
| `init` | `display init` | Initialize display (DSI, panel, canvas) |
| `deinit` | `display deinit` | Turn off display |
| `fill` | `display fill <color_hex>` | Fill screen with color (e.g., `FF0000` = red) |
| `brightness` | `display brightness <level>` | Set backlight (0–255) |
| `draw` | `display draw` | Cycle through colors or draw grayscale pyramid |

### ✅ Example
```sh
display init
display fill FF0000           # Red
display brightness 128        # 50% brightness
display draw                  # Switch to next color
display draw                  # ... until grayscale pyramid
display deinit
```

> 📌 `draw` is **stateful**: each call cycles to next mode (6 colors + pyramid).

---

## 💡 LED Commands

Control PWM-driven LED brightness.

| Command | Syntax | Description |
|--------|--------|-------------|
| `get_device` | `led get_device` | Show LED device name |
| `init` | `led init` | Initialize LED driver |
| `brightness` | `led brightness <level>` | Set brightness (0–255) |

### ✅ Example
```sh
led init
led brightness 200    # ~78% brightness
led brightness 30     # Dim glow
```

> 📌 Only supports single-channel LED brightness control.

---

## 📷 Video (Camera) Commands

Capture frames from image sensor and optionally convert to JPEG.

| Command | Syntax | Description |
|--------|--------|-------------|
| `get_device` | `video get_device` | Show video device name |
| `init` | `video init` | Initialize camera and pipeline |
| `deinit` | `video deinit` | Stop stream and release buffers |
| `take` | `video take <n> <show>` | Capture `n` frames; `show=1` → output JPEG via base64 |

### Parameters
- `<n>`: number of frames to capture
  - `1` → take one photo
  - `-1` → continuous capture (infinite)
  - `0` → stop capture
- `<show>`: whether to convert and print JPEG
  - `0` → only print frame info
  - `1` → convert to JPEG and output as base64

### ✅ Example
```sh
video init
video take 1 1          # Take 1 photo and output as base64 JPEG
video take 3 0          # Take 3 photos, only show metadata
video take -1 1         # Continuous capture with JPEG output
video take 0 0          # Stop capture
video deinit
```

### Output Example
```sh
Got frame 1! size: 6400; timestamp 1234 ms
JPEG FILE:
/9j/4AAQSkZJRgABAQE... (base64-encoded JPEG)
JPEG END
```

> 📌 Requires `VIDEO_SW_PIPELINE` for ISP and JPEG encoding.  
> 📌 Uses `tiny_isp` and `base64_encode` for on-the-fly JPEG generation.

---

## 🧩 General Notes

- ✅ **Initialization Required**: Always call `xxx init` before using any module.
- ❌ **Thread Safety**: Do not run multiple `record`/`play`/`take` commands concurrently.
- 🔋 **Power Management**: Use `deinit` to reduce power when idle.
- 📦 **Memory**: Video and audio use dynamic buffers — ensure heap size is sufficient.
- 🛠️ **Devicetree**: All devices must be properly defined in `.dts`.

---

## 🚀 Quick Start Example

```sh
# Initialize key devices
speaker init
micphone init
display init
video init

# Test audio
speaker play 0

# Capture photo
video take 1 1

# Show sensor data
sensor get vbat
sensor get bma580@18

# Light up LED
led init
led brightness 200

# Clean up
speaker stop
micphone stop
video take 0 0
speaker deinit
micphone deinit
video deinit
display deinit
```
