.. _vga020_sample:

VGA020 MIPI-DSI Display Sample
###############################

Overview
********

This sample demonstrates how to use the VGA020 micro OLED display module
with the Halo board. The VGA020 is a 240×320 resolution color OLED screen 
manufactured by Guozhao Optoelectronics (国照光电), controlled via MIPI-DSI 
interface.

The sample showcases various 2D graphics operations:

* Drawing filled and unfilled rectangles
* Drawing polygons (triangle example)
* Rendering bitmap images  
* Drawing text with custom fonts
* Drawing lines
* Adjusting display brightness

All graphics are rendered using the Canvas API, which provides a simple
drawing interface for embedded displays.

Features
********

**Display Specifications**:
  - **Resolution**: 240×320 pixels (portrait mode)
  - **Interface**: MIPI-DSI (via I2C control)
  - **Color Depth**: 24-bit RGB888
  - **Brightness Control**: 0-100% adjustable
  - **Frame Buffer**: DTCM memory region for optimal performance

**Graphics Capabilities**:
  - 2D shape drawing (rectangles, polygons, lines)
  - Bitmap image rendering
  - Text rendering with TrueType fonts
  - Color support with RGB hex notation
  - Hardware-accelerated display via CDC200 controller

**Hardware Requirements**:
  - MIPI-DSI interface enabled
  - TPS65132 dual voltage regulator (VPOS/VENG supplies)
  - GPIO control for reset pin
  - CDC200 display controller

Building and Running
********************

Build and flash the sample:

.. code-block:: console

   west build -b halo alif/samples/halo/vga020
   west flash

Sample Output
*************

.. code-block:: console

   [00:00:00.150] <inf> disp: Enable Ensemble-DSI Device video mode.
   [00:00:00.155] <inf> disp: Panel Orientation - 0
   [00:00:00.165] <inf> disp: Display sample for cdc200@1b027000
   [00:00:00.165] <inf> disp: Enabling CDC200 Device.
   [00:00:00.166] <inf> disp: Display Capabilities
   [00:00:00.166] <inf> disp: Panel resolution, supported formats - (240, 320), 0
   [00:00:00.166] <inf> disp: CDC200 orientation - 0
   [00:00:00.166] <inf> disp: Display Capabilities layer 1:
   [00:00:00.166] <inf> disp:    layer_enabled - 1
   [00:00:00.166] <inf> disp:    (x_res, y_res) - (240, 320)
   [00:00:00.166] <inf> disp:    curr_pix_fmt - 0
   [00:00:00.167] <inf> disp: Display Capabilities layer 2:
   [00:00:00.167] <inf> disp:    layer_enabled - 0
   [00:00:00.167] <inf> disp:    (x_res, y_res) - (0, 0)
   [00:00:00.167] <inf> disp:    curr_pix_fmt - 0
   [00:00:00.167] <inf> disp: FB0 - 0x20004000, size - 230400
   [00:00:00.171] <inf> disp: draw rect time: 3 ms
   [00:00:00.174] <inf> disp: draw rect time: 3 ms
   [00:00:00.175] <inf> disp: draw triangle time: 1 ms
   [00:00:00.176] <inf> disp: draw image time: 1 ms
   [00:00:00.180] <inf> disp: draw char time: 4 ms
   [00:00:00.184] <inf> disp: draw string time: 3 ms
   [00:00:00.185] <inf> disp: draw line time: 0 ms
   Brightness: 0
   Brightness: 1
   Brightness: 2
   ...
   Brightness: 100
   Brightness: 0

The sample continuously cycles the display brightness from 0% to 100%.

Device Tree Configuration
*************************

The VGA020 display is configured in the device tree:

.. code-block:: devicetree

   &i2c1 {
       status = "okay";
       
       vga020: vga020@54 {
           compatible = "gz,vga020";
           mipi = <&mipi_dsi>;
           reg = <0x54>;
           vpos-supply = <&vpos>;
           veng-supply = <&veng>;
           reset-gpios = <&gpio5 3 GPIO_ACTIVE_LOW>;
           width = <240>;
           height = <320>;
           status = "okay";
       };
   };

   / {
       chosen {
           zephyr,panel = &vga020;
           zephyr,display = &cdc200;
       };
   };

Configuration Parameters
************************

**Display Properties**:
  - **compatible**: "gz,vga020" (Guozhao VGA020 display)
  - **reg**: I2C address (0x54)
  - **width/height**: Display resolution (240×320 pixels)

**Interface Configuration**:
  - **mipi**: Reference to MIPI-DSI controller
  - **reset-gpios**: GPIO pin for hardware reset (GPIO5_3, active low)

**Power Supply**:
  - **vpos-supply**: Positive voltage supply (from TPS65132)
  - **veng-supply**: Negative voltage supply (from TPS65132)
  
  See :ref:`tps65132_sample` for voltage regulator configuration.

Canvas API Usage
****************

The sample uses the Canvas API for 2D graphics:

Drawing Shapes
==============

.. code-block:: c

   #include <canvas.h>

   Canvas canvas;
   Color red = COLOR_HEX(0xFF0000);
   Color green = COLOR_HEX(0x00FF00);
   Color blue = COLOR_HEX(0x0000FF);

   /* Initialize canvas with framebuffer */
   canvas_init(&canvas, (uint8_t (*)[240][3])layer.fb_addr);

   /* Clear screen */
   canvas_clear(&canvas, COLOR_HEX(0x000000));

   /* Draw filled rectangle */
   canvas_draw_rect(&canvas, 50, 50, 100, 80, green, false);

   /* Draw outline rectangle */
   canvas_draw_rect(&canvas, 200, 50, 80, 100, blue, true);

   /* Draw polygon (triangle) */
   int triangle[] = {120, 30, 180, 100, 60, 100};
   canvas_draw_polygon(&canvas, triangle, 3, white);

   /* Draw line */
   canvas_draw_line(&canvas, 10, 10, 100, 10, red);

Drawing Images
==============

.. code-block:: c

   /* Draw bitmap image */
   canvas_draw_bitmap(&canvas, 0, 0, 128, 64, gImage_ABC);

Images must be converted to a compatible format. See the ``bitmap.h``
header for format details.

Text Rendering
==============

.. code-block:: c

   /* Set font and scale */
   canvas_set_font(&canvas, &Dogica8px, 2);

   /* Draw single character */
   canvas_draw_char(&canvas, 'A', 0, 100, COLOR_HEX(0xFF0000));

   /* Draw string */
   canvas_draw_string(&canvas, "Hello World", 0, 20, COLOR_HEX(0x00FF00));

Brightness Control
==================

.. code-block:: c

   #include <zephyr/drivers/display.h>

   const struct device *panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_panel));

   /* Set brightness (0-100%) */
   display_set_brightness(panel, 50);

Display API Usage
*****************

Low-level display control:

.. code-block:: c

   #include <zephyr/drivers/display.h>
   #include <zephyr/drivers/display/cdc200.h>

   const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
   const struct device *panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_panel));

   /* Get panel capabilities */
   struct display_capabilities caps;
   display_get_capabilities(panel, &caps);

   /* Turn off blanking (enable display) */
   display_blanking_off(panel);

   /* Enable CDC200 display controller */
   cdc200_set_enable(display_dev, true);

   /* Get framebuffer information */
   struct cdc200_fb_desc layer;
   cdc200_get_framebuffer(display_dev, 0, &layer);

Frame Buffer Layout
*******************

**Memory Region**: DTCM (Data Tightly Coupled Memory)

The framebuffer is allocated in DTCM for optimal access speed:

========== ============ ================
Parameter  Value        Description
========== ============ ================
Resolution 240×320      Total pixels
Format     RGB888       3 bytes per pixel
Size       230,400      Bytes (240×320×3)
Location   0x20004000   DTCM address
========== ============ ================

Pixel format: Each pixel is 3 bytes (R, G, B) in memory.

Performance Considerations
**************************

Drawing Performance
===================

Typical rendering times on Halo (ARM Cortex-M55 @ 160MHz):

==================== ========== ===================
Operation            Time (ms)  Notes
==================== ========== ===================
Rectangle (outline)  3-4        Depends on size
Rectangle (filled)   3-4        Depends on size
Triangle (outline)   1          Simple polygon
Bitmap (128×64)      1          Small image
Single character     4          With 2× scaling
String (11 chars)    3-4        "Hello World"
Line                 <1         Short line
==================== ========== ===================

Optimization Tips
=================

**Frame Buffer Access**:
  - Use DTCM for framebuffer (``CONFIG_FB_USES_DTCM_REGION=y``)
  - Avoid frequent small updates
  - Batch drawing operations when possible

**Memory Management**:
  - Heap size: 64KB minimum (``CONFIG_HEAP_MEM_POOL_SIZE=65536``)
  - Required for Canvas and display operations
  - Increase if running out of memory

**Display Updates**:
  - CDC200 automatically refreshes from framebuffer
  - No explicit flush/update needed
  - Changes appear immediately on screen

Troubleshooting
***************

**Display shows nothing**:
  - Verify VPOS/VENG voltage supplies are enabled (TPS65132)
  - Check reset GPIO is properly configured
  - Ensure MIPI-DSI is in video mode
  - Confirm CDC200 is enabled

**Distorted or incorrect colors**:
  - Check framebuffer pixel format (should be RGB888)
  - Verify framebuffer size matches resolution
  - Ensure proper byte order in canvas operations

**Low brightness or dim display**:
  - Check brightness setting (0-100%)
  - Verify voltage supplies are at correct levels
  - Review power supply connections

**Memory allocation failures**:
  - Increase ``CONFIG_HEAP_MEM_POOL_SIZE``
  - Check DTCM region is properly configured
  - Verify framebuffer fits in DTCM

**Text rendering issues**:
  - Ensure font is properly included
  - Check font scale factor
  - Verify text coordinates are within display bounds

Configuration Options
*********************

Display Configuration
=====================

.. code-block:: kconfig

   # Display drivers
   CONFIG_DISPLAY=y
   CONFIG_MIPI_DSI=y

   # Frame buffer location
   CONFIG_FB_USES_DTCM_REGION=y

   # Canvas 2D graphics library
   CONFIG_CANVAS=y

Memory Configuration
====================

.. code-block:: kconfig

   # Heap size for display operations
   CONFIG_HEAP_MEM_POOL_SIZE=65536

   # Log buffer for debug output
   CONFIG_LOG_BUFFER_SIZE=4096

Logging Configuration
=====================

.. code-block:: kconfig

   CONFIG_LOG=y
   CONFIG_DISPLAY_LOG_LEVEL_DBG=y
   CONFIG_MIPI_DSI_LOG_LEVEL_DBG=y

   # Floating point support for printf
   CONFIG_CBPRINTF_FP_SUPPORT=y

Hardware Setup
**************

Connections
===========

**VGA020 Display**:
  - **I2C**: Connected to I2C1 bus (address 0x54)
  - **MIPI-DSI**: 4-lane DSI interface
  - **RESET**: GPIO5_3 (active low)
  - **VPOS**: Positive LCD bias voltage (+5.5V from TPS65132)
  - **VENG**: Negative LCD bias voltage (-5.5V from TPS65132)

**Power Requirements**:
  - Display requires dual voltage supply (VPOS/VENG)
  - TPS65132 regulator must be enabled before display initialization
  - Reset pin must be controlled properly during power-up sequence

Initialization Sequence
=======================

1. Enable TPS65132 voltage regulators (VPOS/VENG)
2. Wait for voltages to stabilize
3. Release reset (GPIO high)
4. Configure MIPI-DSI to video mode
5. Initialize display panel
6. Enable CDC200 display controller
7. Begin framebuffer operations

Related Samples
***************

* :ref:`tps65132_sample`: TPS65132 voltage regulator control
* :ref:`cdc200_sample`: CDC200 display controller (if available)
* Display API samples in Zephyr tree

References
**********

* VGA020 Display Datasheet (Guozhao Optoelectronics)
* MIPI DSI Specification
* CDC200 Display Controller Documentation
* Zephyr Display Driver API
* Canvas 2D Graphics Library
