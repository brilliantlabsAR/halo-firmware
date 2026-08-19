.. _button_sample:


GPIO Button Event Detection Sample
###################################

Overview
********

This sample demonstrates the GPIO button driver with comprehensive event detection:

* Single click detection
* Double click detection  
* Multi-level long press detection (dynamically configurable)
* Auto-callback when reaching maximum long press duration

The driver supports dynamic configuration of long press levels through device tree.
When the button is held for the maximum configured time, the callback is triggered
automatically without waiting for button release.

Features
********

**Click Detection:**
  - Single click: Quick press and release
  - Double click: Two clicks within configurable interval (default 400ms)

**Long Press Detection:**
  - Up to 4 configurable long press levels
  - Dynamic array length support (1-4 levels)
  - Auto-callback on maximum duration (no release needed)
  - Each level triggers its own callback

**Debouncing:**
  - Software debouncing (configurable, default 30ms)
  - Filters out mechanical switch noise

Building and Running
********************

Build and flash:

.. code-block:: console

   west build -b halo samples/halo/button
   west flash

Sample Output
*************

.. code-block:: console

   =============================================
     Button Event Test Application
   =============================================
   Button device: button

   Button Events Configuration:
     - Single Click:     Quick press and release
     - Double Click:     Two quick clicks within 400ms
     - Long Press:       Hold for 1 second
     - Long Press Lv1:   Hold for 3 seconds
     - Long Press Lv2:   Hold for 8 seconds
     - Long Press Lv3:   Hold for 10 seconds

   Mode: Unified callback for all events

   >>> Button test started. Press the button to test events...

   [1234 ms] Event #1: Single Click
   [5678 ms] Event #1: Double Click
   [10234 ms] Event #1: Long Press (1 second)
   [25678 ms] Event #1: Long Press Level 1 (3 seconds)
   [38900 ms] Event #1: Long Press Level 2 (8 seconds)
   [52100 ms] Event #1: Long Press Level 3 (10 seconds)

   ========== Button Event Statistics ==========
   Single Click:           1 times
   Double Click:           1 times
   Long Press (1s):        1 times
   Long Press Level 1 (3s): 1 times
   Long Press Level 2 (8s): 1 times
   Long Press Level 3 (10s): 1 times
   =============================================

Configuration
*************

Device Tree Configuration
=========================

Configure the button in your board's device tree:

.. code-block:: devicetree

   button: button {
       compatible = "gpio-button";
       gpios = <&lpgpio 1 GPIO_ACTIVE_LOW>;
       debounce-ms = <30>;
       long-press-ms = <1000 3000 8000 10000>;  /* 4 levels */
       double-click-ms = <400>;
       status = "okay";
   };

   aliases {
       sw0 = &button;
   };

Dynamic Long Press Configuration
---------------------------------

The ``long-press-ms`` array can have 1-4 elements:

**4 levels (default):**

.. code-block:: devicetree

   long-press-ms = <1000 3000 8000 10000>;

**3 levels:**

.. code-block:: devicetree

   long-press-ms = <1000 3000 8000>;

**2 levels:**

.. code-block:: devicetree

   long-press-ms = <2000 5000>;

**1 level:**

.. code-block:: devicetree

   long-press-ms = <3000>;

When the maximum long press duration is reached, the callback is triggered
automatically without waiting for button release.

Project Configuration
=====================

Enable required options in ``prj.conf``:

.. code-block:: kconfig

   CONFIG_GPIO=y
   CONFIG_INPUT=y
   CONFIG_INPUT_GPIO_BUTTON=y
   CONFIG_INPUT_GPIO_BUTTON_GLOBAL_THREAD=y

Testing
*******

Test Procedures
===============

1. **Single Click**
   - Press and release quickly
   - Should trigger within 1 second

2. **Double Click**
   - Press-release-press-release rapidly
   - Must complete within 400ms

3. **Long Press (1s)**
   - Hold button for at least 1 second
   - Release before 3 seconds

4. **Long Press Level 1 (3s)**
   - Hold button for at least 3 seconds
   - Release before 8 seconds

5. **Long Press Level 2 (8s)**
   - Hold button for at least 8 seconds
   - Release before 10 seconds

6. **Long Press Level 3 (10s)**
   - Hold button for 10 seconds or more
   - Auto-callback triggered at 10s (no release needed)

Statistics Display
==================

Event statistics are automatically displayed every 30 seconds, showing:

- Total count of each event type
- Helps verify all events are working correctly

Implementation Details
**********************

Event Processing
================

The driver processes button events in this priority order:

1. Debouncing check (filters noise)
2. Long press detection (checked continuously while pressed)
3. Double click detection (timed sequence)
4. Single click detection (fallback)

Auto-Callback Feature
======================

When the button is held for the maximum configured long press duration:

- Callback is triggered automatically
- No need to wait for button release
- Prevents accidental release from canceling the event
- Useful for critical actions like shutdown/reset

Callback Modes
==============

The sample supports two callback registration modes:

**Mode 1: Unified Callback (Default)**

.. code-block:: c

   button_callback_register(button, button_event_cb);

All events handled by single callback - simpler and more efficient.

**Mode 2: Individual Callbacks**

.. code-block:: c

   button_event_callback_register(button, callback, BUTTON_LONG_PRESS);
   button_event_callback_register(button, callback, BUTTON_SINGLE_CLICK);
   // ...

Each event type has its own callback - useful for complex applications.

API Reference
*************

.. code-block:: c

   /* Register callback for all events */
   int button_callback_register(const struct device *dev, 
                                 button_event_cb_t cb);

   /* Register callback for specific event */
   int button_event_callback_register(const struct device *dev,
                                       button_event_cb_t cb,
                                       enum button_action action);

   /* Callback function type */
   typedef void (*button_event_cb_t)(const struct device *dev,
                                     enum button_action action);

   /* Button event types */
   enum button_action {
       BUTTON_LONG_PRESS,           /* Level 0: First long press threshold */
       BUTTON_LONG_PRESS_LEVEL_1,   /* Level 1: Second threshold */
       BUTTON_LONG_PRESS_LEVEL_2,   /* Level 2: Third threshold */
       BUTTON_LONG_PRESS_LEVEL_3,   /* Level 3: Fourth threshold */
       BUTTON_SINGLE_CLICK,         /* Single click event */
       BUTTON_DOUBLE_CLICK,         /* Double click event */
   };
