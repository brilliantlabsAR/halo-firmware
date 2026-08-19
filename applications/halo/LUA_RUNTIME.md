# Lua Runtime Documentation

## Architecture

### Component Overview

```
┌────────────────────────────────────────────────────────────────────┐
│                        Lua Runtime System                          │
└────────────────────────────────────────────────────────────────────┘

                              ┌──────────────┐
                              │  main.c      │
                              └──────┬───────┘
                                     │
                                     ▼
                    ┌────────────────────────────────┐
                    │     Lua Runtime Core           │
                    │     (lua_runtime.c)            │
                    │                                │
                    │  • VM lifecycle                │
                    │  • Thread management           │
                    │  • Memory allocator            │
                    │  • Service event dispatcher    │
                    └────────────┬───────────────────┘
                                 │
           ┌─────────────────────┼─────────────────────┐
           │                     │                     │
           ▼                     ▼                     ▼
  ┌────────────────┐   ┌────────────────┐   ┌────────────────┐
  │  REPL Thread   │   │  Data Thread   │   │ Lua Services   │
  │  (Priority 7)  │   │  (Priority 7)  │   │  (Lifecycle)   │
  ├────────────────┤   ├────────────────┤   ├────────────────┤
  │ • Execute Lua  │   │ • BLE binary   │   │ • INIT         │
  │ • REPL loop    │   │   data         │   │ • DEINIT       │
  │ • Stack mgmt   │   │ • Interrupt    │   │ • INTERRUPT    │
  │ • 128KB stack  │   │ • 8KB stack    │   │ • SUSPEND      │
  └────────┬───────┘   └────────┬───────┘   │ • RESUME       │
           │                    │           └────────────────┘
           │                    │                     │
           └────────────────────┴─────────────────────┘
                                     │
                                     ▼
                              ┌────────────────┐
                              │  BLE Lua       │
                              │  Service       │
                              │  (ble_lua.c)   │
                              ├────────────────┤
                              │ • REPL channel │
                              │ • Data channel │
                              │ • Video        │
                              │ • Audio        │
                              └────────────────┘
```

### Memory Management

Lua runtime uses a custom allocator that routes all allocations through the Halo memory manager:

```
Lua VM Allocation
        │
        ▼
lua_halo_alloc()
        │
        ▼
halo_malloc() / halo_realloc() / halo_free()
        │
        ▼
┌─────────────────────────────────┐
│  Internal SRAM  (1.5MB)         │
│  External SRAM  (7.5MB)         │
│  (Auto-selected by allocator)  │
└─────────────────────────────────┘
```

**Benefits:**
- Unified memory statistics
- External SRAM support for large allocations
- Better control over memory regions

---

## Control Commands

Control commands are sent via BLE Lua service:

| Command | Code | Action | Usage |
|---------|------|--------|-------|
| Ctrl+B | 0x02 | Reboot device | System reboot |
| Ctrl+C | 0x03 | Interrupt script | Stop running script |
| Ctrl+D | 0x04 | Restart runtime | Reload Lua VM |
| Ctrl+E | 0x05 | Remove main.lua + restart | Factory reset script |
| Ctrl+F | 0x06 | Exit runtime | Shutdown Lua VM |

---

## Lua Service System

The Lua service system provides lifecycle event management for all Lua modules.

### Service Events

| Event | Trigger | Purpose |
|-------|---------|---------|
| `INIT` | VM initialized | Initialize resources |
| `DEINIT` | VM shutting down | Cleanup resources |
| `INTERRUPT` | Ctrl+C pressed | Reset state |
| `SUSPEND` | System suspend | Prepare for sleep |
| `RESUME` | System resume | Restore after sleep |

### Service Definition

```c
// Define a service
static int my_service_event_handler(halo_lua_event_t event, void *user_data)
{
    switch (event) {
    case HALO_LUA_EVENT_INIT:
        // Initialize resources
        break;
    case HALO_LUA_EVENT_DEINIT:
        // Cleanup resources
        break;
    case HALO_LUA_EVENT_INTERRUPT:
        // Handle Ctrl+C
        break;
    default:
        break;
    }
    return 0;
}

// Register service (always-on = false)
HALO_LUA_SERVICE_DEFINE(my_service,
                        my_service_event_handler,
                        NULL,
                        false);
```

### Registered Services

| Service | File | Purpose |
|---------|------|---------|
| system | `lua_system.c` | System API, battery, PM integration |
| button | `lua_button.c` | Button event callbacks |
| bluetooth | `lua_bluetooth.c` | BLE data transmission |
| imu | `lua_imu.c` | IMU sensor data |
| display | `lua_display.c` | Display control |
| camera | `lua_camera.c` | Camera capture |
| microphone | `lua_microphone.c` | Audio recording |
| speaker | `lua_speaker.c` | Audio playback |
| time | `lua_time.c` | Time/date functions |
| file | `lua_file.c` | File system operations |
| compression | `lua_compression.c` | LZ4 decompression |

---

## API Modules

> **`halo` alias:** The entire API is also exposed under a `halo` global that
> references the same table as `frame` — `halo.standby()`, `halo.display.text()`,
> etc. are identical to their `frame.*` counterparts (`halo == frame` is `true`).
> All namespaces below are reachable through either name.

### System (`frame`)

```lua
-- Execution control
frame.sleep([seconds])
frame.yield()
frame.reboot()

-- Battery
frame.battery_level()        -- 0-100
frame.battery_voltage()      -- millivolts
frame.battery_charging()     -- true/false
frame.charge(enable)         -- Enable/disable charging

-- Device info
frame.get_eui()              -- EUI-64 hex string
frame.get_se_revision()      -- Secure Enclave revision

-- Power management
frame.stay_awake([enabled])
frame.light_sleep([seconds])
frame.standby([seconds])
frame.wakeup_source()
frame.ship_mode()
```

### Time (`frame.time`)

```lua
frame.time.utc()             -- UTC timestamp
frame.time.zone(offset)      -- Set timezone
frame.time.date()            -- Date table
```

### File (`frame.file`)

```lua
frame.file.open(path, mode)
frame.file.listdir(path)
frame.file.remove(path)
```

### Bluetooth (`frame.bluetooth`)

```lua
frame.bluetooth.send(data)
frame.bluetooth.receive_callback(callback)
```

### Display (`frame.display`)

```lua
frame.display.text(text, x, y, color)
frame.display.clear(color)
frame.display.brightness(0-100)
```

### Camera (`frame.camera`)

```lua
frame.camera.capture({resolution = 640, quality = "HIGH"})
local ready = frame.camera.image_ready()
local jpeg = frame.camera.read(1024)
```

### IMU (`frame.imu`)

```lua
frame.imu.accelerometer_callback(callback)
frame.imu.gyroscope_callback(callback)
```

### Microphone (`frame.microphone`)

```lua
frame.microphone.start({encoder = "pcm", sample_rate = 8000, channels = 2, gain = 0, aec = false, voice = false})
frame.microphone.stop()
frame.microphone.read(size)   -- non-blocking; partial OK (see below)
frame.microphone.gain([value])
frame.microphone.aad_callback(func, [threshold], [silent_period])

-- Halo only (no-op on hardware without the AEC); getters with no arg, setters with a bool:
frame.microphone.aec([enable])    -- echo cancellation on/off (toggle live)
frame.microphone.voice([enable])  -- voice-band mode: band-pass the mic output to ~300-3400Hz
frame.microphone.diag(cmd)        -- diagnostics: 'stats' -> table, 'zero' -> reset counters
```

**AEC / voice mode (Halo only):** `aec()` cancels speaker echo from the mic feed; `voice()` band-passes the mic output to a speech band (~300–3400 Hz). They are two **independent, opt-in** stages on the mic path (`mic → [aec] → [voice] → encode`), **both off by default** — pair them so out-of-band echo can't reach a server VAD (the self-interruption fix); `voice` alone just band-limits the raw mic. Set the initial state declaratively at `start{aec=, voice=}` (the reset point each session) or toggle live via the setters, which mirror `gain()`. `diag('stats'|'zero')` exposes canceller instrumentation for test scripts.

**Startup latency:** After `start()`, the first useful `read()` may return `""` while PDM/DMIC settles and the first DMA block is captured. This applies after standby resume too (mic is stopped and the ring buffer is cleared). `read()` is **non-blocking**: no data yet → `""`; partial data → returns up to `size` bytes (LC3 is byte-accurate FIFO). Scripts should poll in the main loop or use a one-off delay/pump where first-packet timing matters.

**UART timing logs:** `Mic Lua start`, `Mic first ring put`, and `Mic first Lua read` (see firmware log) correlate with Lua `MIC_LAT` on the phone.

---

## File Index

| File | Description |
|------|-------------|
| `modules/halo/src/lua_runtime.c` | Core runtime (threads, VM lifecycle) |
| `modules/halo/src/lua_service.c` | Service lifecycle management |
| `modules/halo/include/halo/lua_runtime.h` | Runtime API header |
| `modules/halo/include/halo/lua_service.h` | Service API header |
| `modules/halo/src/ble_lua.c` | BLE Lua service (transport) |
| `modules/halo/src/lua_*.c` | API bindings |
| `applications/halo/PROTOCOL.md` | Full API documentation |
