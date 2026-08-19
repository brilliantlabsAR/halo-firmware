# Halo Button & LED User Guide

## Overview

This document describes the button operations and LED indicator behaviors for the Halo smart glasses.

---

## First Time Use / Device Activation

**When you receive your Halo device, it will be in ship mode (shutdown state) and needs to be activated before first use.**

### How to Activate Your Device

```
┌─────────────┐        ┌─────────────┐        ┌─────────────┐
│ Connect     │   →    │ Wait for    │   →    │ Device      │
│ Charger     │        │ LED to turn │        │ Ready to    │
│             │        │ ON solid    │        │ Pair        │
└─────────────┘        └─────────────┘        └─────────────┘
```

**Steps:**
1. Connect the charger to your Halo device
2. Wait for the LED to turn solid ON (device is booting up)
3. Device is now ready for Bluetooth pairing
4. Use the Halo app on your phone to complete pairing

**LED During Activation:**
```
Step 1: Connect charger
LED:  ____  (OFF - Device in ship mode)
         │
         ▼
Step 2: Booting (2-3 seconds)
LED:  ████  (Solid ON - Booting up)
         │
         ▼
Step 3: Ready
LED:  ████  (Solid ON - Charging, ready to pair)
```

**Note:** The first charge may take a few seconds to initialize. If the LED doesn't turn on immediately, wait 5-10 seconds.

---

## ⚠️ Important Warning

**Ship Mode (15 second hold) will:**
1. **Clear all BLE pairing information** - you will need to re-pair with your device
2. **Completely shut down the device** - **requires charger connection to wake up**

**To wake up from ship mode, you MUST connect the charger** - the device cannot be woken up by button press alone.

**If you only want to clear pairing information**, release the button between 5-15 seconds.

---

---

## Button Operations

### 1. Recovery Mode (Factory Reset)

**Purpose:** Enter recovery mode to clear all configurations and files. Only firmware updates are allowed in this mode.

**How to Enter:**
```
┌─────────────┐        ┌─────────────┐        ┌─────────────┐
│  Hold Button│   +    │ Plug in     │   →    │ Hold 10s    │
│             │        │ Charger     │        │ Until LED    │
└─────────────┘        └─────────────┘        │  Flashes     │
                                               └─────────────┘
```

**LED Pattern:**
```
LED:  ████____████____████____  (1s ON, 1s OFF, repeating)
      ↑----1s----↑----1s----↑
```

**How to Exit Recovery Mode:**

There are two ways to exit recovery mode:

**Method 1: Press Button**
```
┌─────────────┐
│ Press Button│  →  Exit Recovery Mode → Device Reboots
└─────────────┘
```

**Method 2: Auto Timeout (120 seconds)**
```
No BLE Activity ──── 120 seconds ────→ Exit Recovery Mode → Device Reboots
```

**What Happens After Exiting:**
- Device reboots automatically
- All configurations and files have been erased
- Device boots with latest firmware

**Warning:** This will erase all user data and configurations!

---

### 2. Device Restart

**Purpose:** Restart the device manually.

**Operation:**
```
┌─────────────┐        ┌─────────────┐        ┌─────────────┐
│  Hold Button│   +    │ Plug in     │   →    │ Release     │
│             │        │ Charger     │        │ Button      │
└─────────────┘        └─────────────┘        └─────────────┘
       │                                              │
       └──────────────────────────────────────────────┘
                           │
                           ▼
                    Device Reboots
```

---

### 3. Deep Sleep Mode

**Purpose:** Enter power-saving deep sleep mode.

**How to Enter:**
```
┌─────────────┐        ┌─────────────┐
│ Hold Button │   →    │ Release     │
│  (2 sec)    │        │ at the beep │
└─────────────┘        └─────────────┘
                            │
                            ▼
                     Enter Deep Sleep
```

A short blip sounds the moment the hold reaches 2 seconds — that's the cue
that releasing now will power the device down. Keep holding instead to reach
pairing mode (5s) or ship mode (15s).

**LED in Deep Sleep:**
```
LED:  ____  (OFF - Device is sleeping)
```

**How to Wake from Deep Sleep:**
```
┌─────────────┐
│ Press Button │  →  Wake Up → Device Resumes
└─────────────┘
     (Brief press)
```

**What Happens in Deep Sleep:**
- LED is OFF
- Most functions are suspended
- Button press is the only way to wake
- Device resumes to previous state after waking

---

### 4. Pairing Mode

**Purpose:** Enter Bluetooth pairing/discoverable mode.

**Operation:**
```
┌─────────────┐
│ Hold Button │   →   Auto-enters Pairing Mode after 5s
│  (5 sec)    │        (No need to release button)
└─────────────┘
     │
     │─── 5 seconds ───┤
                        │ Release here to stay in pairing mode
                        │
                        ▼
                 Enter Pairing Mode
```

**LED Pattern:**
```
LED:  ██__██__██__██__  (0.5s ON, 0.5s OFF, repeating)
      ↑0.5s↑↑0.5s↑↑0.5s↑
```

A coin "buh-ding" plays as the pairing window opens — hold through the short
power-off blip at 2s and keep holding until you hear it at 5s.

**Important:** To stay in pairing mode, **release the button between 5-15 seconds**. If you continue holding past 15 seconds, the device will enter ship mode (shutdown).

**Note:** If device is already in pairing mode (not paired), holding 5s will only update the LED indicator without clearing any bond data.

---

### 5. Ship Mode (Shutdown)

**Purpose:** Completely shut down the device for long-term storage or shipping. Requires hardware reset (plug in charger) to wake up.

**How to Enter:**
```
┌─────────────┐
│ Hold Button │   →   5s: Clear Bond → 15s: Ship Mode
│  (15 sec)   │        (Clears pairing information first)
└─────────────┘
     │
     │─── 5s ───┤────── 15s ───┤
                  │               │
                  ▼               ▼
           Pairing Mode    Device Shuts Down
        (Bond Cleared)      (LED turns OFF)
           LED Blinks
```

**Warning:**
- **Ship mode will clear all BLE pairing information** - you will need to re-pair with your device after waking up
- Device will pass through pairing mode at 5 seconds (bond is cleared, LED will start blinking)
- Device will shut down completely at 15 seconds
- **⚠️ To wake up from ship mode, you MUST connect the charger** - button press will NOT wake the device
- All unsaved data will be lost

**How to Wake Up from Ship Mode:**
- Connect the charger to the device
- Wait for LED to turn solid ON (2-3 seconds)
- Device is now ready to use (see "First Time Use / Device Activation" section above)

**To Cancel Ship Mode:** Release the button between 5-15 seconds to stay in pairing mode.

**To Clear Pairing Without Shutdown:** Hold 5 seconds, then release before 15 seconds.

---

## LED Indicator Summary

| State | LED Pattern | Description |
|-------|-------------|-------------|
| Charging | `████` (Constant ON) | Device is charging |
| Booting | `████` (Constant ON) | Device is starting up (2-3s) |
| Recovery Mode | `████____████____` | 1s blink - Factory reset mode |
| Pairing Mode | `██__██__██__` | 0.5s blink - Pairing mode |
| Deep Sleep | `____` (OFF) | Device in deep sleep |
| Ship Mode | `____` (OFF) | Device shut down (charger required to wake) |

---

## Quick Reference

| Button Action | Result |
|---------------|--------|
| **First time use** | **Connect charger to activate device** |
| Hold + Plug charger → Hold 10s | Recovery Mode (Factory Reset) |
| Hold + Plug charger → Release | Restart |
| Hold 2s → Release at the beep | Deep Sleep |
| Hold 5s, then release (5-15s) | Pairing Mode (clears bond if paired) |
| Hold 15s (continue holding) | **Ship Mode (clears bond + shutdown)** |
| **Connect charger** (in ship mode) | **Wake up from Ship Mode** |
| Press (in sleep) | Wake from Deep Sleep |
| Press (in recovery) | Exit Recovery Mode |

---

## Timing Diagram

```
Recovery Mode Timing:

Enter Recovery Mode:
Button:    ██████████████████████████████████
                              │
Charger:        ──────────────┼───────────────►
                              │
                              │─── 10 seconds ───┤
                                                   │
LED:              ─────────────────────────────────┼───█─█─█─█─█─
                                                   │
                                                   ▼
                                          Recovery Mode Entered


Exit Recovery Mode (Button Press):
Button:                                            █
                                                   │
LED:              █─█─█─█─█─█─█─█─█─────────────────┼───────────►
                                                   │
                                                   ▼
                                          Device Reboots


Exit Recovery Mode (Timeout):
No BLE:      ███████████████████████████████████████
                                                   │
              │──── 120 seconds ────│              │
                                         │         │
LED:              █─█─█─█─█─█─█─█─█─█─█─────────────┼───────────►
                                         │         │
                                         ▼         ▼
                                   Timeout    Device Reboots


Pairing Mode Timing:

Button:    ████████████████████████████████
                                    │
                                    │─── 5 seconds ───┤
                                                      │ Release here
LED:              ────────────────────────────────────┼──█─█─█─█─█─█─█─█─█─█─►
                                                      │
                                                      ▼
                                                 Pairing Mode Entered
                                        (Release between 5-15s to stay in pairing)


Ship Mode Timing:

Button:    ████████████████████████████████████████████████████
                                    │                        │
                                    │─── 5s ───┤             │─── 15s ───┤
                                    │          │                          │
Event:             ──────────────────┼──────────┼──────────────────────────►
                                    │          │
                                    ▼          ▼
                               Bond Cleared  Ship Mode
                               Pairing Mode  (Shutdown)
                               (LED blinks)

To wake up from ship mode: Connect charger (see "First Time Use / Device Activation" section)
To stay in pairing mode: Release button between 5-15 seconds
To enter ship mode: Continue holding past 15 seconds
**Note: Both actions clear BLE pairing information**


Deep Sleep Timing:

Enter Deep Sleep:
Button:    ████████████████
                       │
                       │─── 2 seconds ───┤ ♪ beep
                                          │ Release button
LED:              █████████████           ━━━━━━━━━━━━━━━━━━►
                                          │
                                          ▼
                                    Deep Sleep Entered
                                    (Release triggers action)


Wake from Deep Sleep:
Button:                                         █
                                               │
LED:              ━━━━━━━━━━━━━━━━━━           ████████████►
                                               │
                                               ▼
                                        Device Wakes Up
```

---

## Notes

- All timing values are approximate and may vary slightly
- Recovery mode automatically exits after 120 seconds of inactivity
- Deep sleep preserves minimal state to allow button wake
- **The 2s deep-sleep hold beeps at the threshold** — release at the beep to power down; keep holding for the higher levels
- **The 5s pairing hold plays a coin chime** as the pairing window opens (the window auto-closes after ~60 seconds; hold 5s again to reopen it)
- In recovery mode, only OTA firmware updates are possible - normal Bluetooth services are disabled
- **5s and 15s button holds are auto-triggers** - the action occurs immediately when the time threshold is reached
- **Ship mode passes through pairing mode** - holding to 15s will first trigger pairing mode at 5s (LED blinks), then continue to ship mode at 15s
- **⚠️ IMPORTANT: Ship mode (15s) clears BLE pairing information** - you will need to re-pair with your device after waking up from ship mode
- **To stay in pairing mode**: Release the button between 5-15 seconds after pressing (bond is still cleared)
- **To enter ship mode**: Continue holding the button past 15 seconds (bond is cleared + device shuts down)
- **To wake up from ship mode**: Connect charger - button press will NOT work (see "First Time Use / Device Activation" section)
- 5s hold clears existing BLE bond and enters pairing mode; if already unpaired, only updates LED indicator
- 15s hold triggers ship mode (complete shutdown) - requires charger connection to wake up
