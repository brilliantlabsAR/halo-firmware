# Halo pairing & bonding design

This document is the spec for Halo's multi-bond pairing model: the constraints it
must satisfy, the design, and the reasoning behind each decision.

## 1. Problem

Until this change Halo stored **exactly one bond**. The consequences:

- Entering pairing mode via the button hold *deleted* the stored bond, so
  switching between two hosts (e.g. a Mac for development and an iPhone for
  ANCS testing) required a full re-pair every time — including the peer-side "Forget This
  Device" dance and, on macOS, GATT-cache staleness after repeated re-bonds.
- A user with a phone and a laptop cannot use both without repeatedly re-pairing.

Goal: support several bonded devices with semantics a consumer can predict,
**without** adding a companion-app device-management UI.

## 2. Constraints

| # | Constraint | Source |
|---|-----------|--------|
| C1 | One button. Its BLE gestures are: 5 s hold = pairing, 15 s hold (refused while charging) = factory reset/ship mode. 2 s = sleep, short presses belong to the Lua app. | hardware / `lua_button.c` |
| C2 | No companion-app or API surface for add/remove device. Consumers manage bonds with the button and the peer OS only. | product |
| C3 | Single BLE connection at a time. The firmware (`ble_connection.c`) tracks one `conidx`; we do not mediate concurrent centrals. | architecture |
| C4 | Alif BLE ROM stack v1.2 (`gapm`/`gapc` APIs), **not** Zephyr's `bt_*` host. Bond storage is our own (`ble_security.c` + littlefs settings), so there is no `CONFIG_BT_MAX_PAIRED` to turn up — we own the table. | platform |
| C5 | iOS/macOS peers use Resolvable Private Addresses; identifying a returning peer requires resolving the RPA against stored peer IRKs (`gapm_le_resolve_address`, which accepts an IRK *array* — verified in v1.2 headers). | BLE reality |
| C6 | `sec_ctx` lives in `noinit` RAM guarded by a magic word so bonds survive warm reboots (OTA swap, `sys_reboot` recovery paths) without a settings reload. Any layout change **must** change the magic. | `ble_security.c` |
| C7 | LE Secure Connections, Just Works (no display/keypad on device: `GAP_IO_CAP_NO_INPUT_NO_OUTPUT`). | `ble_security.c` |
| C8 | The pairing LED state (`HALO_LED_STATE_PAIRING`) is the only on-device pairing UI. | `led_manager` |

## 3. Design summary

**Five bond slots, LRU-evicted. The 5-second hold opens a ~60 s pairing
*window* instead of deleting anything. One connection at a time; first bonded
device to connect wins exclusive access.**

### 3.1 Bond table

- `CONFIG_HALO_BLE_MAX_BONDS` slots (default **5**). Each slot stores the peer
  identity (identity address when the peer distributes an IRK, else the
  connection address), pairing keys (LTK/IRK/CSRK), GAP bond data, and a
  monotonic `last_used` sequence number for LRU ordering.
- Persistence: settings keys `/lfs/ble/bond<i>_{keys,data,addr,meta}` per slot.
  `meta` holds `{last_used, valid}` and is written last, so a slot is only
  considered valid once fully persisted.
- **Migration:** on first boot after update, legacy `/lfs/ble/bond_{keys,data,addr}`
  (the old single bond) is imported into slot 0 and the legacy keys deleted.
  Your existing phone stays paired across the OTA.
- The `noinit` magic changes (`'SEC2'`) so a warm reboot across the OTA boundary
  falls back to a clean settings load instead of misreading the old layout.

Why 5: storage is ~130 bytes/slot (trivial on littlefs), RPA resolution cost is
linear and negligible at this scale, and 5 gives real headroom (phone, laptop,
tablet, dev host, spare) so LRU eviction — the one "surprising" behaviour in
this model — almost never fires. 3 is exactly a developer's working set with
zero margin.

### 3.2 Who may connect, and when

Connection acceptance depends on two things: whether the connecting device
matches a bond slot, and whether the **pairing window** is open.

| Peer | Steady state (window closed) | Pairing window open |
|------|------------------------------|---------------------|
| Matches a bond slot | Accept, encrypt, bump LRU | **Reject** (disconnect) |
| Unknown | Reject (disconnect) | Accept, pair, bond into free/LRU slot |
| Any, when **zero bonds** exist | Accept + pair (perpetual pairing mode, as today out-of-box) | n/a (same) |

Rationale for rejecting *known* peers during the window: the device has a single
connection (C3). A bonded Mac with a pending connect request would otherwise
grab the link the instant the window opens and the new phone could never get
in. Known devices lose nothing — they are refused for at most the window
duration and reconnect freely afterwards. This makes the button deterministic:
**"hold 5 s → the next *new* device to arrive gets paired."**

Contention between two bonded devices in range never arises: connectable
advertising stops while a connection is up (link-layer behaviour) and restarts
on disconnect (`ble_connection.c` disconnect handler). The second device simply
does not see Halo until the first releases it. First to connect wins; no
mediation, no shared access.

### 3.3 The pairing window

- Opened by the 5 s hold (`lua_button.c` level-2 handler). If a peer is
  connected it is disconnected first — same as today.
- Duration `CONFIG_HALO_BLE_PAIRING_WINDOW_SEC` (default **60 s**), enforced by
  a delayable work item. Holding 5 s again restarts the window.
- Closes on: successful new pairing, timeout, or reboot (window state is
  deliberately *not* in `noinit` RAM).
- LED: `HALO_LED_STATE_PAIRING` is shown exactly when the device is pairable,
  i.e. `window open OR zero bonds`.

### 3.4 New pairing lifecycle (staging + commit)

In-progress pairings are staged in a scratch `pending` bond, **not** written to
a slot:

1. Unknown peer accepted (window open) → `pending.peer_addr` = connection
   address; security requested.
2. Keys exchanged → accumulated in `pending`. If the peer distributes an IRK,
   `pending.peer_addr` is upgraded to the peer's **identity address** (the v1.2
   `gapc_irk_t` carries it), giving a stable identity for RPA peers instead of
   the rotating address we stored before.
3. Pairing completes (`on_paired`) → commit: pick a free slot, else evict the
   lowest `last_used`; persist keys/data/addr, then meta; close the window.
4. Pairing fails → discard `pending`. **No existing bond is harmed** by a failed
   attempt (the old code deleted partially-written keys from the single slot;
   staging makes that class of cleanup unnecessary).

LRU is bumped (and persisted) when a bonded peer is identified at connection
time and when a bond is committed.

### 3.5 Per-connection bond identity

At connection time the firmware determines *which* bond the connection belongs
to and records it as `active_slot` (single connection ⇒ single variable):

- Public / static address: linear `memcmp` against slot addresses.
- RPA: `gapm_le_resolve_address(addr, nb_irk, irk_array, cb)` with **all**
  bonded peers' IRKs; the callback returns the matching IRK, which is mapped
  back to its slot by key comparison.

`active_slot` then drives everything bond-specific:

- Encryption request → reply with *that slot's* LTK.
- Encryption failure (peer deleted its keys) → clear *that slot only*, then
  disconnect. Other bonds untouched.
- Re-pairing by a known peer → overwrites *its own slot* (see §3.6).

### 3.6 Removing devices without an app (C2)

- **Peer-side "Forget This Device"** — the self-healing path. The forgotten
  peer's next connection still resolves to its slot (iOS keeps its IRK stable
  across forget/re-pair), our encryption request finds the peer keyless, the
  peer initiates fresh pairing, and the commit overwrites its own slot.
  No button press, no window needed: a known device may always re-pair itself.
- **Implicit** — LRU eviction when pairing a 6th device.
- **Nuclear** — 15 s hold (refused while charging): factory reset (filesystem
  format) wipes everything, unchanged.

### 3.7 Semantic change to the 5 s hold (deliberate)

The 5 s hold is no longer destructive. A user who wants Halo to *stop trusting*
a specific device must use "Forget This Device" **on that peer** or factory-reset
the Halo. The old "hold = trust nobody" privacy action is gone; factory reset
covers the "sell/hand over the device" case. Accepted trade-off — the hold's
user-facing meaning ("I want to pair something new") is unchanged.

## 4. Out of scope / explicitly rejected

- **Multiple simultaneous connections** — rejected (C3). No arbitration
  complexity, and the ANCS client, audio profiles and the Lua data channel all
  assume one central.
- **Companion-app bond management** (list/remove UI, GATT management service) —
  rejected (C2). Nothing prevents adding a read-only "bond count" later.
- **Controller resolving list / directed advertising & LL-level filtering** —
  today unknown devices are rejected *after* the link comes up, at GAP level.
  Filter-accept-list + controller-side RPA resolution would reject at the link
  layer, but support in the Alif ROM stack is unproven and the current approach
  is functionally equivalent for our threat model.
- **Per-slot Lua API** — `frame.ble` keeps its existing paired/bonded booleans.

## 5. Touched code

| Area | Change |
|------|--------|
| `modules/halo/src/ble_security.c` | Single `bond` → `bonds[N]` + `pending`; slot persistence + legacy migration; pairing-window state machine; multi-IRK resolution; `active_slot` tracking; new `noinit` magic. |
| `modules/halo/include/halo/ble_security.h` | Window API (`halo_ble_sec_pairing_window_open/is_open`); `bond_save`/`bond_load` removed from the public surface (no external callers); `bond_clear` = clear **all** slots. |
| `modules/halo/src/lua_button.c` | Level-2 (5 s) handler: disconnect + open window instead of `bond_clear`. |
| `modules/halo/Kconfig` | `HALO_BLE_MAX_BONDS` (default 5), `HALO_BLE_PAIRING_WINDOW_SEC` (default 60). |

Unchanged by design: `ble_connection.c` connection/advertising flow, ANCS
(keys off `HALO_BLE_EVENT_PAIRED` + connection events only), `ble_manager`
Lua surface, factory reset.

## 6. Validation plan

Validated on a dev-kit unit via BLE OTA (recoverable):

1. **Migration** — PASS. OTA onto a device bonded the old way: the bond was
   imported to slot 0 (verified in `/lfs/settings`, legacy keys tombstoned)
   and the Mac reconnected without re-pairing.
2. **Second bond** — PASS. Mac, iPhone and Android all bonded concurrently and
   reconnect alternately without re-pairing (the original pain case).
3. **`nb_irk > 1` resolution** — PASS. With multiple RPA peers bonded, each is
   correctly identified on reconnect — this was the one Alif-stack behaviour
   the design assumed but the old code never exercised.
4. **Window exclusivity** — PASS. Bonded peers are rejected during the window;
   a new device pairs while others are in range.
5. **Forget-heal** — PASS. "Forget This Device" on the peer → peer re-pairs
   into its own slot without the button, on every OS tested.
6. **Encrypt-fail isolation** — PASS (exercised by the forget-heal cases;
   only the affected slot is cleared).
7. **Eviction** — PASS. Tested with a `CONFIG_HALO_BLE_MAX_BONDS=2` build
   (avoids needing 6 devices): with Mac + iPhone bonded and the iPhone more
   recently connected, pairing the Android through the window evicted slot 0
   (the Mac, LRU) and the Mac was subsequently refused as unknown. The
   layout-dependent noinit magic also forced the expected settings reload
   across the 5-slot -> 2-slot OTA.

Tested every combination up to 3 simultaneous bonds (Mac public address,
iPhone RPA, Android RPA).

## 7. Risks / open questions

- **`gapm_le_resolve_address` with `nb_irk > 1`** is documented in the v1.2
  headers but this stack has surprised us before — validation item 3 is the
  gate. Fallback if broken: sequential single-IRK resolution attempts (slower,
  same result).
- **Resolution latency vs. connection setup**: resolving against 5 IRKs happens
  before `gapc_le_connection_cfm`; if measurably slow it delays service setup
  by that much. Expected to be milliseconds (AES-128 per IRK).
- **Peers with identical public addresses re-pairing** (dev boards): overwrite
  their own slot — correct by construction.
- iOS IRK stability across "Forget" is empirically true today; if an iOS
  version rotates IRKs on forget, the forgotten phone shows up as *unknown* and
  needs the 5 s window — the model degrades gracefully.
