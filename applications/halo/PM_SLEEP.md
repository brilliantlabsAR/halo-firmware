# Power Management & Sleep Module

## Table of Contents

1. [Architecture](#architecture)
2. [Sleep Modes](#sleep-modes)
3. [Wakeup Sources](#wakeup-sources)
4. [State Machine](#state-machine)
5. [API Reference](#api-reference)
6. [Callback Mechanism](#callback-mechanism)
7. [Configuration](#configuration)
8. [Usage Examples](#usage-examples)

---

## Architecture

### Component Overview

```
Main Application (main.c)
    |
    | halo_pm_init()
    v
+-------------------------------+
|       Power Manager           |
|  (pm_manager.c / .h)          |
|                               |
|  +-----------------------+    |
|  | Callback System        |    |
|  +-----------------------+    |
|                               |
|  - Sleep state tracking        |
|  - Wakeup source detection     |
|  - Statistics counters         |
+-------------------------------+
    |
    | halo_pm_sleep_*()
    v
Hardware (power_mgr API)
```

### Key Features

- **Multiple Sleep Modes**: Standby, Light, Deep sleep for different power scenarios
- **Wakeup Source Tracking**: Records what woke the device
- **Callback System**: Priority-based suspend/resume notifications
- **Sleep Locking**: Prevent/allow sleep with reference counting
- **Statistics**: Track sleep counts and last mode used

---

## Sleep Modes

### Mode Comparison

| Mode | BLE Status | CPU State | Wake Time | Power Use | Use Case |
|------|------------|-----------|-----------|-----------|----------|
| **STANDBY** | On | Active (semaphore wait) | ~µs | Medium | Short idle, instant response |
| **LIGHT** | On | Sleeping | Fast | Low | BLE connected, quick wake |
| **DEEP** | Off | Sleeping | Slow | Lowest | Long idle, no BLE needed |
| **NONE** | On | Active | N/A | Highest | Normal operation |

### Standby Mode (`HALO_PM_SLEEP_STANDBY`)

CPU remains active but peripherals are powered down. Uses semaphore-based wait loop.

**Characteristics:**
- BLE remains connected
- Instant wakeup (microseconds)
- Uses `k_sem_take()` for blocking
- Can be scheduled from interrupt context (async version)

**Best for:** Very short idle periods when BLE responsiveness is critical

```lua
frame.standby(30)  -- 30 seconds
frame.standby()    -- indefinite
```

### Light Sleep (`HALO_PM_SLEEP_LIGHT`)

CPU enters low-power state while maintaining BLE connection.

**Characteristics:**
- BLE stack remains active
- Fast wakeup time
- Uses Zephyr `k_sleep()` with optional RTC timeout
- RTC wakeup automatically configured for timed sleep

**Best for:** Short to medium idle periods with BLE connection active

```lua
frame.light_sleep(60)  -- 60 seconds
frame.light_sleep()    -- indefinite
```

### Deep Sleep (`HALO_PM_SLEEP_DEEP`)

Maximum power savings - BLE is completely disabled.

**Characteristics:**
- BLE stack suspended
- Longest wakeup time
- Only button press can wake (when indefinite)
- RTC wakeup configured for timed sleep

**Best for:** Extended idle periods, battery conservation

```lua
frame.sleep(300)  -- 5 minutes
frame.sleep()     -- indefinite deep sleep
```

---

## Wakeup Sources

### Source Types

| Source | Enum | Description | Available Modes |
|--------|------|-------------|-----------------|
| Unknown | `HALO_PM_WAKEUP_UNKNOWN` | Unclear wake cause | All |
| Timeout | `HALO_PM_WAKEUP_TIMEOUT` | RTC/timeout expired | All |
| Button | `HALO_PM_WAKEUP_BUTTON` | Button press | All |
| BLE | `HALO_PM_WAKEUP_BLE` | BLE data received | Standby, Light |
| IMU | `HALO_PM_WAKEUP_IMU` | IMU interrupt | Standby, Light |
| Microphone | `HALO_PM_WAKEUP_MICROPHONE` | Mic interrupt | Standby, Light |
| External | `HALO_PM_WAKEUP_EXTERNAL` | GPIO interrupt | Standby, Light |
| Watchdog | `HALO_PM_WAKEUP_WATCHDOG` | Watchdog timeout | Standby, Light |

### Hardware Wakeup Mapping

```
Halo Wakeup Source    Hardware Source
------------------    ----------------
BUTTON          →    PM_WAKEUP_LPGPIO1
MICROPHONE      →    PM_WAKEUP_LPGPIO0
BLE             →    PM_WAKEUP_BLE
TIMEOUT         →    PM_WAKEUP_RTC
```

---

## LED Indicators

### LED States

| State | Pattern | Description |
|-------|---------|-------------|
| `HALO_LED_STATE_OFF` | Off | LED off |
| `HALO_LED_STATE_ON` | On | Solid on |
| `HALO_LED_STATE_BLINK_SLOW` | 1Hz | 500ms on, 500ms off |
| `HALO_LED_STATE_BLINK_FAST` | 2Hz | 250ms on, 250ms off |
| `HALO_LED_STATE_CHARGING` | Solid on | Charging indicator |
| `HALO_LED_STATE_PAIRING` | 1Hz | Pairing mode (same as BLINK_SLOW) |

### LED Priority System

Higher priority (lower number) overrides lower priority states:

| Priority | Level | Use Cases |
|----------|-------|-----------|
| 1 (HIGH) | High | Pairing, errors |
| 5 (MEDIUM) | Medium | Charging, system events |
| 10 (LOW) | Low | Status indicators |

### LED During Sleep

- **Before sleep:** Current state is saved
- **During sleep:** LED is turned off
- **After wake:** Previous state is restored

**Implementation:** `led_manager.c:led_pm_callback_handler()`

### LED Pattern Summary

```
Operation              LED Pattern
--------------------   ----------------------------------------------
Normal                 Off or application controlled
Charging               █████ (Solid ON)
Recovery Mode          ████____████____ (1s ON, 1s OFF)
Pairing Mode           ██__██__ (0.5s ON, 0.5s OFF)
Deep Sleep             ____ (OFF)
```

---

## State Machine

### Power Management State Transitions

```
                              ┌─────────────────────────────────────────────┐
                              │              PM Transitions                  │
                              └─────────────────────────────────────────────┘

                              ┌──────────────────┐
                              │     ACTIVE       │ ◄──┐
                              │   (HALO_PM_     │    │  Button/IMU/Mic/
                              │    SLEEP_NONE)  │    │  BLE/Wake → Resume
                              │                  │    │
                              │  • CPU Full Power │    │
                              │  • All Peripherals│    │
                              └────────┬─────────┘    │
                                       │              │
          ┌────────────────────────────┼─────────────────────────────┐
          │            API Call         │                             │
          │                            │                             │
          ▼                            ▼                             ▼
   ┌───────────────┐          ┌───────────────┐             ┌───────────────┐
   │   STANDBY     │          │    LIGHT      │             │     DEEP      │
   │ (HALO_PM_     │          │ (HALO_PM_     │             │ (HALO_PM_     │
   │  SLEEP_       │          │  SLEEP_       │             │  SLEEP_       │
   │   STANDBY)    │          │   LIGHT)      │             │   DEEP)       │
   ├───────────────┤          ├───────────────┤             ├───────────────┤
   │ Semaphore     │          │ BLE ON        │             │ BLE OFF       │
   │ wait only     │          │ HP Core OFF   │             │ HP Core OFF   │
   │               │          │ HE Core OFF   │             │ HE Core OFF   │
   │ CPU ON        │          │ CPU OFF       │             │ CPU OFF       │
   │               │          │               │             │               │
   │ Wake Sources: │          │ Wake Sources: │             │ Wake Sources: │
   │ • Timeout     │          │ • Button      │             │ • Button      │
   │ • Semaphore   │          │ • BLE Data    │             │ • RTC         │
   │               │          │ • IMU         │             │               │
   └───────┬───────┘          │ • Microphone  │             └───────┬───────┘
           │                  └───────┬───────┘                     │
           │                          │                             │
           └──────────────────────────┴─────────────────────────────┘
                                       │
                              Wake Event Occurs
                                       │
                                       ▼
                              ┌──────────────────┐
                              │    RESUME        │
                              │  (Return to      │
                              │   ACTIVE)        │
                              └──────────────────┘

    ┌──────────────────────────────────────────────────────────────────┐
    │                     Wakeup Source Mapping                         │
    ├──────────────────────────────────────────────────────────────────┤
    │ Source          │ Available In         │ Hardware               │
    ├──────────────────┼──────────────────────┼───────────────────────┤
    │ RTC (Timeout)   │ STANDBY, LIGHT, DEEP │ PM_WAKEUP_RTC         │
    │ BUTTON          │ STANDBY, LIGHT, DEEP │ PM_WAKEUP_LPGPIO1     │
    │ BLE             │ STANDBY, LIGHT       │ PM_WAKEUP_BLE         │
    │ IMU             │ STANDBY, LIGHT       │ PM_WAKEUP_LPGPIO2     │
    │ MICROPHONE      │ STANDBY, LIGHT       │ PM_WAKEUP_LPGPIO0     │
    └─────────────────┴──────────────────────┴───────────────────────┘
```

### Callback Execution Order

**On Suspend (entering sleep):**
```
Priority 100  ──┐
Priority 90   ──┼─► Called FIRST
Priority 80   ──┤
...
Priority 10   ──┤
Priority 1    ──┘
```

**On Resume (waking from sleep):**
```
Priority 1    ──┐
...
Priority 90   ──┼─► Called FIRST
Priority 100  ──┘
```

Only callbacks that returned 0 (suspended) are called on resume.

---

## API Reference

### C API

#### `int halo_pm_init(void)`
Initialize the power management system. Call once during boot.

**Returns:** 0 on success, negative error code on failure

---

#### `int halo_pm_sleep_standby(uint32_t timeout_ms)`
Enter standby sleep mode.

**Parameters:**
- `timeout_ms`: Maximum sleep duration in milliseconds (0 = indefinite)

**Returns:** 0 on success, negative error code on failure

---

#### `int halo_pm_sleep_standby_async(uint32_t timeout_ms)`
Schedule standby sleep from interrupt context.

**Parameters:**
- `timeout_ms`: Maximum sleep duration in milliseconds (0 = indefinite)

**Returns:** 0 on success, negative error code on failure

---

#### `int halo_pm_sleep_light(uint32_t timeout_ms)`
Enter light sleep mode (BLE stays on).

**Parameters:**
- `timeout_ms`: Maximum sleep duration in milliseconds (0 = indefinite)

**Returns:** 0 on success, negative error code on failure

---

#### `int halo_pm_sleep_deep(uint32_t timeout_ms)`
Enter deep sleep mode (BLE off).

**Parameters:**
- `timeout_ms`: Maximum sleep duration in milliseconds (0 = indefinite)

**Returns:** 0 on success, negative error code on failure

---

#### `int halo_pm_wakeup(halo_pm_wakeup_source_t source)`
Wake up from sleep. Can be called from interrupt context.

**Parameters:**
- `source`: What caused the wakeup

**Returns:** 0 on success, negative error code on failure

---

#### `halo_pm_wakeup_source_t halo_pm_get_wakeup_source(void)`
Get the last wakeup source.

**Returns:** Last wakeup source enum

---

#### `bool halo_pm_is_sleeping(void)`
Check if system is currently in sleep mode.

**Returns:** true if sleeping, false otherwise

---

#### `int halo_pm_prevent_sleep(void)`
Increment sleep lock counter. Prevents system from entering sleep.

**Returns:** 0 on success, negative error code on failure

---

#### `int halo_pm_allow_sleep(void)`
Decrement sleep lock counter. Allows sleep when counter reaches zero.

**Returns:** 0 on success, negative error code on failure

---

### Lua API

#### `frame.sleep([seconds])`
Deep sleep or sleep for specified time.

```lua
frame.sleep(5)  -- 5 seconds
frame.sleep()   -- indefinite deep sleep
```

**Note:** Sleep is broken into 100ms chunks to allow Ctrl+C interruption.

---

#### `frame.light_sleep([seconds])`
Light sleep with BLE maintained.

```lua
frame.light_sleep(10)  -- 10 seconds
frame.light_sleep()    -- indefinite
```

On wakeup the Lua VM restarts and `main.lua` runs from the top — nothing
after the `light_sleep()` call executes. Read `frame.wakeup_source()` at the
top of the script to learn why it is running. (`frame.standby()` is the mode
that resumes in place.)

---

#### `frame.standby([seconds])`
Standby mode with fast wakeup.

```lua
frame.standby(30)  -- 30 seconds
frame.standby()    -- indefinite
```

---

#### `frame.ship_mode()`
Enter ultra-low power mode (requires hardware reset to wake).

**WARNING:** Device will be completely powered down!

---

#### `frame.stay_awake([enabled])`
Prevent/allow automatic sleep.

```lua
frame.stay_awake(true)   -- prevent sleep
frame.stay_awake(false)  -- allow sleep
local awake = frame.stay_awake()  -- get state
```

---

#### `frame.wakeup_source()`
Get the source that woke the device from sleep.

```lua
local source = frame.wakeup_source()
print("Woke by:", source)  -- "button", "ble", "timeout", etc.
```

---

### Button Lua API

#### `frame.button.single([function])`
Register or clear single click callback.

```lua
frame.button.single(function()
    print("Single clicked!")
end)

-- Clear callback
frame.button.single(nil)
```

---

#### `frame.button.double([function])`
Register or clear double click callback.

```lua
frame.button.double(function()
    print("Double clicked!")
end)
```

---

#### `frame.button.long([function])`
Register or clear long press (1 second) callback.

```lua
frame.button.long(function()
    print("Long pressed!")
end)
```

---

## Callback Mechanism

### Registering a PM Callback

```c
#include <halo/pm_manager.h>

static int my_pm_callback(halo_pm_event_t event,
                          halo_pm_sleep_mode_t mode,
                          void *user_data)
{
    switch (event) {
    case HALO_PM_EVENT_SUSPEND:
        /* Prepare for sleep */
        /* Return 0 = suspended (will be resumed) */
        /* Return 1 = skipped (won't be resumed) */
        /* Return <0 = error */
        return 0;

    case HALO_PM_EVENT_RESUME:
        /* Restore from sleep */
        return 0;
    }
}

/* Static registration */
HALO_PM_CALLBACK_DEFINE(my_cb, my_pm_callback, NULL, "my_driver", 50);

/* During init */
int my_init(void)
{
    return halo_pm_register_callback(&my_cb, my_pm_callback,
                                     NULL, "my_driver", 50);
}
```

### Callback Return Values

**For SUSPEND event:**
- `0` = Successfully suspended, WILL be called on resume
- `1` = Skipped (already suspended), will NOT be called on resume
- `<0` = Error, stops suspend sequence

**For RESUME event:**
- `0` = Success
- `<0` = Error (logged but doesn't stop other callbacks)

### Priority Guidelines

| Priority | Use Case |
|----------|----------|
| 0-10 | Critical hardware (display, camera) |
| 11-30 | Important peripherals (BLE, audio) |
| 31-60 | Standard services (LED, file system) |
| 61-100 | Optional features (debugging, stats) |

---

## Configuration

### Kconfig Options

```
# Core PM
CONFIG_PM=y
CONFIG_PM_DEVICE=y
CONFIG_HALO_PM_MANAGER=y

# Sleep timeout (auto-sleep when idle)
CONFIG_HALO_PM_IDLE_TIMEOUT_MS=5000

# Logging
CONFIG_HALO_LOG_LEVEL=3
```

### Device Tree

Power state configuration in `halo.overlay`:

```dts
power-states {
    off: off {
        /* 2s reserved for OS wakeup and sleep */
        min-residency-us = <2000000>;
    };
};
```

---

## Usage Examples

### Example 1: Simple Sleep

```lua
-- Sleep for 30 seconds
frame.sleep(30)

-- Enter deep sleep indefinitely
-- (wake with button press)
frame.sleep()
```

### Example 2: Light Sleep and Wakeup Source

```lua
-- main.lua runs from the top after every light-sleep wake:
-- branch on the wakeup source first
local source = frame.wakeup_source()
if source == "button" then
    print("User pressed button")
elseif source == "ble" then
    print("Received BLE data")
end
-- source: "unknown", "timeout", "button", "ble",
--         "imu", "microphone", "external", "watchdog"

-- Enter light sleep for 1 minute; nothing after this call runs
frame.light_sleep(60)
```

### Example 3: Preventing Sleep During Critical Work

```lua
-- Prevent auto-sleep during file operation
frame.stay_awake(true)

local file = io.open("/lfs/data.txt", "w")
file:write("important data")
file:close()

-- Re-enable sleep
frame.stay_awake(false)
```

### Example 4: Button Callbacks

```lua
-- Single click - take photo
frame.button.single(function()
    frame.camera.snap()
end)

-- Double click - toggle recording
frame.button.double(function()
    if frame.camera.is_recording() then
        frame.camera.stop()
    else
        frame.camera.start()
    end
end)

-- Long press - enter pairing mode manually
frame.button.long(function()
    print("Long press detected")
end)
```

### Example 5: C Driver with PM Callback

```c
#include <halo/pm_manager.h>

static struct my_device {
    struct halo_pm_callback pm_cb;
    /* ... device specific state ... */
} my_dev;

static int my_pm_cb(halo_pm_event_t event,
                    halo_pm_sleep_mode_t mode,
                    void *user_data)
{
    struct my_device *dev = user_data;

    if (event == HALO_PM_EVENT_SUSPEND) {
        /* Save device state */
        dev->saved_reg = read_reg(dev);
        /* Power down device */
        power_down(dev);
        return 0;  /* Will be resumed */
    }

    if (event == HALO_PM_EVENT_RESUME) {
        /* Restore device */
        power_up(dev);
        write_reg(dev, dev->saved_reg);
        return 0;
    }

    return 0;
}

int my_device_init(struct my_device *dev)
{
    /* Initialize hardware */
    init_hardware(dev);

    /* Register PM callback */
    return halo_pm_register_callback(&dev->pm_cb,
                                      my_pm_cb,
                                      dev,
                                      "my_device",
                                      50);  /* Medium priority */
}
```

---

## File Index

| File | Description |
|------|-------------|
| `modules/halo/include/halo/pm_manager.h` | Public API header |
| `modules/halo/src/pm_manager.c` | Implementation |
| `modules/halo/src/lua_system.c` | Lua bindings for sleep API |
| `modules/halo/src/lua_button.c` | Button Lua API |
| `modules/halo/src/led_manager.c` | LED control with PM support |
| `applications/halo/boards/halo.overlay` | Device tree overlay |
| `applications/halo/boards/halo.conf` | Board configuration |
| `applications/halo/PM_SLEEP.md` | This document |
| `applications/halo/BUTTON_LED_GUIDE.md` | User-facing button guide |

---

## See Also

- [BUTTON_LED_GUIDE.md](BUTTON_LED_GUIDE.md) - User-facing button operations
- [PROTOCOL.md](PROTOCOL.md) - Bluetooth protocol specification
- [LUA_RUNTIME.md](LUA_RUNTIME.md) - Lua runtime documentation
