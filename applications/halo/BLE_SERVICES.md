# BLE Services Documentation

## Overview

The Frame firmware implements multiple BLE services for device management, firmware updates, and communication.

```
┌────────────────────────────────────────────────────────────────────┐
│                         BLE Manager                                │
│                       (ble_manager.c)                              │
│                                                                    │
│  • Service registration/disconnection                              │
│  • Event dispatch system                                           │
│  • Power management integration                                    │
└────────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┬─────────────────┐
        │                     │                     │                 │
        ▼                     ▼                     ▼                 ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐  ┌───────────────┐
│ Battery Svc   │    │  Lua Service  │    │  OTA Service  │  │ LE Audio Svc  │
│   0x180F      │    │  (Custom)     │    │   (SMP)       │  │   0x1850      │
├───────────────┤    ├───────────────┤    ├───────────────┤  ├───────────────┤
│ Level 0x2A19  │    │ REPL/Data     │    │ DFU/Mgmt      │  │ BAP/PACS      │
│ State 0x2A1A  │    │ Audio TX/RX   │    │ MCUboot       │  │ TMAP/VCS      │
└───────────────┘    └───────────────┘    └───────────────┘  │ LC3 Codec     │
                                                             └───────────────┘
                              

```

---

## Service Summary

| Service | UUID | Type | Purpose |
|---------|------|------|---------|
| Battery | 0x180F | Standard | Battery level & state |
| Lua | 7A230001-... | Custom | Script execution & data |
| OTA | 8D53DC1D-... | Custom (SMP) | Firmware updates (MCUboot) |
| Audio | 0x1850 | Standard | LE Audio streaming |

---

## Battery Service

### Service Definition

```
Battery Service: 0x180F
    ├── Battery Level: 0x2A19 (Read, Notify)
    └── Battery Power State: 0x2A1A (Read, Notify)
```

### Battery Power State Format

```
Bit 7-6: Level              Bit 5-4: Charging
  00 = Unknown                00 = Unknown
  01 = Not Supported          01 = Not Chargeable
  10 = Good                   10 = Not Charging
  11 = Critically Low         11 = Charging

Bit 3-2: Discharging     Bit 1-0: Presence
  00 = Unknown                00 = Unknown
  01 = Not Supported          01 = Not Supported
  10 = Not Discharging        10 = Not Present
  11 = Discharging            11 = Present
```

### State Examples

| State | Hex | Description |
|-------|-----|-------------|
| Present, Discharging, Not Charging, Good | 0x2C | Battery present, discharging, not charging |
| Present, Not Discharging, Charging, Good | 0x34 | Battery present, charging |
| Present, Not Discharging, Not Charging, Critical | 0x3C | Battery present, critically low (<10%) |

### Event Handling

```
Battery Manager Events → BLE Battery Service
        │
        ├── HALO_BATTERY_EVENT_LEVEL_CHANGED → Notify level
        ├── HALO_BATTERY_EVENT_CHARGING_STARTED → Update state
        ├── HALO_BATTERY_EVENT_CHARGING_STOPPED → Update state
        ├── HALO_BATTERY_EVENT_LOW_BATTERY → Notify level
        └── HALO_BATTERY_EVENT_CRITICAL_BATTERY → Notify level
```

### API Reference

```c
int halo_ble_battery_init(bool reset);
int halo_ble_battery_deinit(void);
int halo_ble_battery_notify(uint8_t level);  // 0-100%
bool halo_ble_battery_is_notify_enabled(void);
```

---

## Lua Service

### Service Definition

```
Lua Service: 7A230001-5475-A6A4-654C-8431F6AD49C4
    ├── RX Characteristic: 7A230002-... (Write Without Response)
    ├── TX Characteristic: 7A230003-... (Notify)
    ├── Video Characteristic: 7A230004-... (Notify)
    ├── Audio RX Characteristic: 7A230005-... (Write Without Response)
    └── Audio TX Characteristic: 7A230006-... (Notify)
```

### Characteristic Usage

| Characteristic | UUID | Direction | Purpose |
|----------------|------|-----------|---------|
| RX | 7A230002 | Host → Frame | REPL commands, control codes |
| TX | 7A230003 | Frame → Host | REPL output, print results |
| Video | 7A230004 | Frame → Host | JPEG video streaming |
| Audio RX | 7A230005 | Host → Frame | Audio playback data |
| Audio TX | 7A230006 | Frame → Host | Microphone capture data |

### Control Codes

| Code | Hex | Name | Description |
|------|-----|------|-------------|
| 0x01 | DATA_MARKER | Data Marker | First byte of binary data transfer |
| 0x02 | CTRL+B | Reboot | Reboot the device |
| 0x03 | CTRL+C | Interrupt | Interrupt current script execution |
| 0x04 | CTRL+D | Restart | Restart Lua runtime |
| 0x05 | CTRL+E | Reset | Reset and remove main.lua |
| 0x06 | CTRL+F | Exit | Exit Lua runtime completely |
| 0x07 | CTRL+G | Remove All | Remove all files/folders (except settings) |

**Note:** CTRL+G (0x07) removes all user files and directories to free up storage space. The settings file (`/lfs/settings`) containing pairing information and configuration is preserved.

### Data Transfer Protocol

```
┌────────────────────────────────────────────────────────────────────┐
│                    Data Transfer Format                            │
└────────────────────────────────────────────────────────────────────┘

Binary Data (RX):
    ┌──────────┬──────────────────────────────────────────┐
    │  0x01    │         Data Payload (N bytes)           │
    │ Marker   │                                          │
    └──────────┴──────────────────────────────────────────┘

REPL Commands (RX):
    ┌──────────────────────────────────────────┐
    │         Lua Code (text)                  │
    └──────────────────────────────────────────┘

Control Commands (RX):
    ┌──────────┐
    │  0x02-07 │  (Single byte control code)
    └──────────┘

Response (TX):
    ┌──────────────────────────────────────────┐
    │         Output/Result (text)             │
    └──────────────────────────────────────────┘
```

### Lua File API

| Function | Description |
|----------|-------------|
| `frame.file.open(path, mode)` | Open file (modes: "r", "w", "a") |
| `frame.file.remove(path)` | Delete file or empty directory |
| `frame.file.remove_all()` | Remove all files/folders (except settings) |
| `frame.file.rename(from, to)` | Rename file or directory |
| `frame.file.listdir(path)` | List directory contents |
| `frame.file.mkdir(path)` | Create directory (recursive) |

### API Reference

```c
// Service initialization
int halo_ble_lua_init(bool reset);
int halo_ble_lua_deinit(void);

// REPL I/O
int32_t halo_ble_lua_repl_read(uint8_t *data, size_t len, k_timeout_t timeout);
int32_t halo_ble_lua_repl_write(const uint8_t *data, size_t len);

// Binary data I/O
int32_t halo_ble_lua_data_read(uint8_t *data, size_t len, k_timeout_t timeout);
int32_t halo_ble_lua_data_write(const uint8_t *data, size_t len);

// Video/Audio transport
int32_t halo_ble_lua_video_write(const uint8_t *data, size_t len);
int32_t halo_ble_lua_audio_read(uint8_t *data, size_t len, k_timeout_t timeout);
int32_t halo_ble_lua_audio_write(const uint8_t *data, size_t len);

// Control handler registration
void halo_ble_lua_register_ctrl_handler(halo_ble_lua_ctrl_handler_t handler);
```

---

## OTA Service

### Service Definition (SMP)

```
SMP Service: 8D53DC1D-1DB7-4CD3-868B-8A527460AA84
    └── SMP Characteristic: DA2E7828-FBCE-4E01-AE9E-261174997C48
            (Write Without Response, Notify)
```

### MCUboot / SMP Integration

```
┌────────────────────────────────────────────────────────────────────┐
│                      OTA Update Flow                               │
└────────────────────────────────────────────────────────────────────┘

    Host Device                 Frame                      MCUboot
         │                          │                          │
         │─── SMP Write ────────────→│                          │
         │   (image data)            │                          │
         │                          │─── Write to flash ──────→│
         │                          │                          │
         │                          │─── Swap request ────────→│
         │                          │                          │
         │←── SMP Notify ───────────│                          │
         │   (response)             │                          │
         │                          │                          │
         │←── Reboot �───────────────│─── Reboot ──────────────→│
         │                          │                          │
         │                          │     [Bootloader]        │
         │                          │       Swap images       │
         │                          │       Verify            │
         │                          │       Boot new fw       │
```

### SMP Operations

| Command | Description |
|---------|-------------|
| Image Upload | Write firmware image in chunks |
| Image Swap | Request MCUboot to swap images |
| Image List | Query available images |
| Image Test | Mark image for testing |

### Transport Implementation

```
SMP Transport Layer
        │
        ├── transport_out() - Split response into MTU chunks
        ├── transport_get_mtu() - Return current MTU
        └── transport_query_valid_check() - Invalidate pending requests on disconnect
```

### API Reference

```c
int halo_ble_ota_init(bool reset);
int halo_ble_ota_deinit(void);
```

---

## BLE Manager

### Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│                         BLE Manager                                │
│                      (ble_manager.c)                               │
└────────────────────────────────────────────────────────────────────┘
                              │
        ┌─────────────────────┼─────────────────────┐
        │                     │                     │
        ▼                     ▼                     ▼
┌───────────────┐    ┌───────────────┐    ┌───────────────┐
│    Service    │    │   Callback    │    │      PM       │
│ Registration  │    │  Dispatch     │    │   Callback    │
│               │    │               │    │               │
│ halo_ble_*_   │    │ Priority list │    │ Priority 20   │
│ _init(reset)  │    │ execution     │    │               │
└───────────────┘    └───────────────┘    └───────────────┘
```

### Service Initialization Order

```c
halo_ble_init(device_name)
    │
    ├── halo_ble_conn_init(device_name)       // Connection manager
    ├── halo_ble_sec_init()                    // Security (pairing)
    ├── halo_ble_battery_init(reset)           // Battery Service
    ├── halo_ble_lua_init(reset)               // Lua Service
    ├── halo_ble_audio_init(reset)             // Audio Service
    ├── halo_ble_ota_init(reset)               // OTA Service
    └── halo_pm_register_callback(...)         // PM integration
```

### Callback System

```c
// Register callback with priority
int halo_ble_register_callback(
    struct halo_ble_callback *cb,
    halo_ble_event_cb_t callback,
    uint32_t event_mask,    // Events to subscribe
    void *user_data,
    const char *name,
    int priority            // Higher = called first
);
```

### BLE Events

| Event | Trigger | Description |
|-------|---------|-------------|
| `HALO_BLE_EVENT_CONNECTED` | Device connected | New BLE connection |
| `HALO_BLE_EVENT_DISCONNECTED` | Device disconnected | Connection closed |
| `HALO_BLE_EVENT_PAIRED` | Pairing complete | Secure connection established |
| `HALO_BLE_EVENT_ADV_COMPLETE` | Advertising stopped | Adv duration expired |
| `HALO_BLE_EVENT_MTU_UPDATED` | MTU exchange complete | New MTU size |

### API Reference

```c
// Manager
int halo_ble_init(const char *device_name);
int halo_ble_adv_start(const struct halo_ble_adv_params *params);
int halo_ble_adv_stop(void);
int halo_ble_disconnect(void);

// State queries
bool halo_ble_is_connected(void);
bool halo_ble_is_paired(void);
bool halo_ble_is_advertising(void);
uint8_t halo_ble_get_conidx(void);
uint16_t halo_ble_get_mtu(void);
int halo_ble_get_address(uint8_t addr[6]);
int halo_ble_update_conn_params(const struct halo_ble_conn_params *params);

// Callbacks
int halo_ble_register_callback(...);
int halo_ble_unregister_callback(struct halo_ble_callback *cb);
```

---

## Advertising

### Default Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| Interval Min | 40 (25ms) | Minimum advertising interval |
| Interval Max | 200 (125ms) | Maximum advertising interval |
| Duration | 0 | Advertise indefinitely |
| Channel Map | 0x07 | All channels (37, 38, 39) |

### Advertising Data

```
┌────────────────────────────────────────────────────────────────────┐
│                      Advertising Packet                            │
├─────────────────────┬─────────┬────────────────────────────────────┤
│ Field              │ Length  │ Value                              │
├─────────────────────┼─────────┼────────────────────────────────────┤
│ Complete Local Name│ Variable│ "Halo XX" (device name)             │
│ 128-bit Service UUID│   17    │ 7A230001-5475-A6A4-654C-8431F6AD49C4 │
│                    │         │ (Lua Service)                       │
└─────────────────────┴─────────┴────────────────────────────────────┘
```

**Note:** Default device name is `Halo XX` where XX is the 4th byte (`eui48[3]`) of the device's EUI-48 address.

### Scan Response Data

```
┌────────────────────────────────────────────────────────────────────┐
│                    Scan Response Packet                             │
├─────────────────────┬─────────┬────────────────────────────────────┤
│ Field              │ Length  │ Value                              │
├─────────────────────┼─────────┼────────────────────────────────────┤
│ Appearance         │    3    │ 0x01C0 (Eye-glasses)               │
│ 16-bit Service UUID │    3    │ 0x180F (Battery Service)          │
└─────────────────────┴─────────┴────────────────────────────────────┘
```

### Service UUIDs

| Service | UUID Type | UUID | Broadcast |
|---------|-----------|------|----------|
| Lua | 128-bit | 7A230001-5475-A6A4-654C-8431F6AD49C4 | Yes (Adv Data) |
| Battery | 16-bit | 0x180F | Yes (Scan Response) |
| Audio | 16-bit | 0x1850 | No |
| OTA | 128-bit | 8D53DC1D-1DB7-4CD3-868B-8A527460AA84 | No |

**Note:** Only Lua and Battery services are advertised. Audio and OTA services are discovered after connection.

### Appearance

| Value | Description |
|-------|-------------|
| 0x01C0 | Eye-glasses (Generic Glasses 0x0C01 with customization) |

### AD Types

| Type Value | Type Name |
|------------|-----------|
| 0x01 | Flags |
| 0x02 | Incomplete List of 16-bit Service UUIDs |
| 0x03 | Complete List of 16-bit Service UUIDs |
| 0x08 | Shortened Local Name |
| 0x09 | Complete Local Name |
| 0x19 | Appearance |
| 0x07 | Complete List of 128-bit Service UUIDs |

---

## LE Audio Service

### Overview

LE Audio implements Bluetooth Low Energy Audio using the Basic Audio Profile (BAP) with LC3 codec.

```
┌────────────────────────────────────────────────────────────────────┐
│                      LE Audio Architecture                          │
└────────────────────────────────────────────────────────────────────┘

    Host Device                    Frame                    Hardware
         │                          │                          │
         │─── Codec Config ────────→│                          │
         │   (ASCP)                 │                          │
         │                          │                          │
         │─── QoS Config ──────────→│                          │
         │   (ASCP)                 │                          │
         │                          │                          │
         │─── Enable ──────────────→│                          │
         │   (ASCP)                 │─── Create datapath ─────→│
         │                          │                          │
         │                          │─── LC3 decoder/encoder   │
         │                          │                          │
         │─── CIS Established ─────→│─── Bind ISO datapath ───→│
         │                          │                          │
         │──────── LC3 Audio ───────→│─── ISO SDUs ────────────→│
         │                          │                          │
```

### Services and Profiles

| Component | Description |
|-----------|-------------|
| **BAP** | Basic Audio Profile - Core audio streaming |
| **PACS** | Published Audio Capabilities Server - Device capabilities |
| **TMAP** | Telephony and Media Audio Profile - Role definition |
| **CAP/CAS** | Common Audio Profile Acceptor / Common Audio Service |
| **VCS** | Volume Control Service - Volume management |
| **MICS** | Microphone Control Service - Host mute control of the microphone |
| **AICS** | Audio Input Control Service - Microphone gain (included in MICS) |
| **ASCS** | Audio Stream Control Service - ASE management |
| **ASE** | Audio Stream Endpoint - Per-stream state machine |

### ASE State Machine

```
     ┌─────────┐
     │  IDLE   │
     └────┬────┘
          │ Codec Config (ASCP)
          ▼
  ┌───────────────┐
  │CODEC_        │
  │CONFIGURED    │
  └───────┬───────┘
          │ QoS Config (ASCP)
          ▼
  ┌───────────────┐
  │  QOS_        │
  │CONFIGURED    │
  └───────┬───────┘
          │ Enable (ASCP)
          ▼
  ┌───────────────┐
  │   ENABLING    │
  └───────┬───────┘
          │ CIS Established
          ▼
  ┌───────────────┐
  │  STREAMING    │◄─────┐
  └───────┬───────┘      │
          │               │
          │ Disable/Release
          ▼               │
  ┌───────────────┐      │
  │  RELEASING    │──────┘
  └───────────────┘
          │
          ▼
     ┌─────────┐
     │  IDLE   │
     └─────────┘
```

### Audio Datapath (Sink)

```
┌────────────────────────────────────────────────────────────────────┐
│                      Sink Datapath (BLE → Speaker)                 │
└────────────────────────────────────────────────────────────────────┘

    ISO SDUs               LC3 Decoder              Speaker
        │                        │                      │
        ▼                        ▼                      ▼
   ┌─────────┐           ┌──────────┐          ┌─────────┐
   │SDU Queue│──────────→│LC3 Decode│──────────→│Speaker  │
   │(128 buf)│           │  Thread  │          │ I2S Out │
   └─────────┘           └──────────┘          └─────────┘
        │                        │                      │
        │◄───────────────────────┘                      │
        │   ISO Datapath (CtoH)                        │
        │                                                │
        └──────────── Notify SDU Done ◄─────────────────┘
```

### Audio Datapath (Source)

```
┌────────────────────────────────────────────────────────────────────┐
│                    Source Datapath (Microphone → BLE)               │
└────────────────────────────────────────────────────────────────────┘

    Microphone              LC3 Encoder             ISO SDUs
        │                        │                      │
        ▼                        ▼                      ▼
   ┌─────────┐           ┌──────────┐          ┌─────────┐
   │  Mic    │──────────→│LC3 Encode│──────────→│SDU Queue│
   │  PDM    │           │  Thread  │          │(128 buf) │
   └─────────┘           └──────────┘          └─────────┘
        │                        │                      │
        │                        │                      │
        │                        │                      ▼
        │                        │                  ┌─────────┐
        │                        │                  │ISO Data │
        │                        │                  │ (HtoC)  │
        │                        └──────────────────→└─────────┘
```

### Supported Codec Configurations

| Direction | Sample Rate | Frame Duration | Frame Size |
|-----------|-------------|----------------|------------|
| Sink | 8 kHz | 10 ms | 26 bytes |
| Sink | 8 kHz | 7.5 ms | 26 bytes |
| Sink | 16 kHz | 10 ms | 40 bytes |
| Sink | 16 kHz | 7.5 ms | 30 bytes |
| Source | 8 kHz | 10 ms | 26 bytes |
| Source | 8 kHz | 7.5 ms | 26 bytes |
| Source | 16 kHz | 10 ms | 40 bytes |
| Source | 16 kHz | 7.5 ms | 30 bytes |
| Source¹ | 32 kHz | 10 ms | 60-80 bytes |
| Source¹ | 32 kHz | 7.5 ms | 60 bytes |
| Source¹ | 48 kHz | 10 ms | 75-155 bytes |
| Source¹ | 48 kHz | 7.5 ms | 75-117 bytes |

¹ With `CONFIG_HALO_BLE_AUDIO_SOURCE_HQ=y`. The PDM front-end supports
8/16/32/48 kHz only; 24/44.1 kHz would need resampling and are not advertised.

### Configuration

```c
// Kconfig options
CONFIG_HALO_BLE_AUDIO_SINK_ENABLE=y      // Enable audio playback
CONFIG_HALO_BLE_AUDIO_SOURCE_ENABLE=y    // Enable audio capture
CONFIG_HALO_BLE_AUDIO_OUTPUT_STEREO=y    // Stereo output (else mono)
CONFIG_HALO_BLE_AUDIO_INPUT_STEREO=y     // Stereo input (else mono)
CONFIG_HALO_BLE_AUDIO_MICS=y             // MICS microphone mute control
CONFIG_HALO_BLE_AUDIO_AICS=n             // AICS gain in MICS (costs a GATT user slot; conflicts with ANCS)
CONFIG_HALO_BLE_AUDIO_SOURCE_HQ=y        // 32/48 kHz source capabilities
```

### TMAP Roles

| Role | Description |
|------|-------------|
| **CT** | Call Terminal - Telephony audio |
| **UMR** | Unicast Media Receiver - Media playback |
| **UMS** | Unicast Media Sender - Media capture |
| **BMR** | Broadcast Media Receiver/Sender - Broadcast audio |

### Volume Control (VCS)

```
Volume Range: 0-255 (mapped to 0-100% for speaker)
Step Size: 3
Mute: Supported

VCS Callbacks:
    • volume_control_volume_callback() - Volume/mute change
    • volume_control_flags_callback() - Flags change
    • volume_control_bond_data_callback() - Bond data
```

### Microphone Control (MICS/AICS)

The Microphone Control Service lets the connected host mute/unmute the
microphone through the standard Microphone Control Profile (MICP). An AICS
instance exposing microphone gain can be included with
`CONFIG_HALO_BLE_AUDIO_AICS=y`, but it is **off by default**: each GAF
service costs one GATT user slot from the BLE ROM's fixed pool
(`BLE_GATT_USER_NB` = 12), and the pool is fully subscribed once the ANCS
client is enabled — with both on, whichever registers last (ANCS) fails
with `GAP_ERR_INSUFF_RESOURCES` (0x4B). Without AICS, MICS is mute-only and
gain remains available via `frame.microphone.gain()`.

```
MICS Mute: Not Muted / Muted / Disabled (host writable, notifiable)
AICS Input (optional): "Microphones" (type: Microphone)
AICS Gain: -10..+10 dB, 1 dB units (matches frame.microphone.gain() range;
           persisted to the shared "audio/gain" setting)

Mute semantics:
    • While muted (MICS mute or AICS input mute), the LE Audio source
      stream keeps its ISO cadence but transmits LC3-encoded silence.
    • Mute state applies to the LE Audio stream; the custom Lua
      microphone channel is governed by its own API (and cannot run
      concurrently with an LE Audio capture stream anyway).
```

### Microphone Arbitration (LE Audio vs frame.microphone)

The microphone hardware is a singleton with owner-ranked arbitration
(`audio_stream.c`): LE Audio preempts any other owner, and non-LE-Audio
callers are rejected while LE Audio holds the device.

```
Host enables source ASE while frame.microphone is streaming:
    1. LE Audio preempts the mic singleton (Lua capture loses ownership)
    2. The Lua capture thread detects the ownership loss and stops cleanly
    3. The next frame.microphone.read() raises
       "Microphone taken over by LE Audio"
    4. frame.microphone.start() fails until the host releases the stream
    5. frame.microphone.status() reports "le_audio" for app-level recovery
```

### LE Audio Announcements (Scan Response)

The legacy advertising PDU is fully occupied by the device name and the
128-bit Halo Lua service UUID, so LE Audio announcements are carried in the
scan response (Android merges ADV_IND + SCAN_RSP into one scan record for
its discovery filters):

| AD Structure | Content |
|--------------|---------|
| Service Data 0x184E (ASCS) | General Announcement + available sink/source contexts |
| Service Data 0x1853 (CAS) | General Announcement (CAP Acceptor) |
| Service Data 0x1855 (TMAS) | TMAP role bit field (CT, UMR, UMS, BMR) |

The scan response is also only 31 bytes, and the ANCS service solicitation
(18 bytes, `CONFIG_HALO_BLE_ANCS_SOLICIT_ADV=y`) takes priority over the LE
Audio announcements because iOS requires it to offer notification access.
Entries are appended in order base → ANCS → ASCS → CAS → TMAS, skipping any
that no longer fit: with ANCS solicitation enabled (the default) only the
CAS announcement fits alongside it; disable ANCS solicitation and all three
LE Audio announcements are carried. Hosts read the actual ASCS/PACS/TMAS
services over GATT after connecting either way.

### API Reference

```c
// Service initialization
int halo_ble_audio_init(bool reset);
int halo_ble_audio_deinit(void);

// Advertising support (used by ble_connection.c scan response)
uint16_t halo_ble_audio_get_tmap_roles(void);
void halo_ble_audio_get_context_types(uint16_t *sink_context_bf, uint16_t *src_context_bf);
```

---

## Power Management Integration

### PM Callback (Priority 20)

```
PM Event: SUSPEND (Light/Deep Sleep)
        │
        ├── Deinit Battery Service
        ├── Deinit Lua Service
        ├── Deinit Audio Service
        ├── Deinit OTA Service
        └── Deinit Connection Manager
                │
                ▼
        [BLE stack suspended]

PM Event: RESUME
        │
        └── Reinitialize services
```

### Sleep Mode Behavior

| Mode | BLE State | Connection |
|------|-----------|------------|
| Standby | Active | Maintained |
| Light | Suspended | Maintained (wakeup source) |
| Deep | Suspended | Dropped |

---

## File Index

| File | Description |
|------|-------------|
| `modules/halo/src/ble_manager.c` | BLE manager (event dispatch, service init) |
| `modules/halo/src/ble_connection.c` | Connection manager (advertising, params) |
| `modules/halo/src/ble_security.c` | Security manager (pairing, bonding) |
| `modules/halo/src/ble_service.c` | Service registration framework |
| `modules/halo/src/ble_battery.c` | Battery Service (0x180F) |
| `modules/halo/src/ble_lua.c` | Lua Service (Custom) |
| `modules/halo/src/ble_audio.c` | LE Audio Service |
| `modules/halo/src/ble_ota.c` | OTA Service (SMP/MCUboot) |
| `modules/halo/src/lua_runtime.c` | Lua runtime and control handling |
| `modules/halo/src/lua_file.c` | Lua file API bindings |
| `modules/halo/src/file_manager.c` | File system management |
