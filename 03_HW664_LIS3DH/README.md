[한국어](./README_kr.md) | **English**

# Lab 03: HW-664 (actually an LIS3DH) 3-Axis Accelerometer (Dual-Core, IPM, SSD1306)

> Third lab in the `zephyr_sensor` series. Built and verified on real
> hardware with the ESP32-S3-DevKitC-1.
> Issues hit during bring-up and how they were solved: [Troubleshooting](./doc/03_HW664_LIS3DH_TROUBLESHOOTING_en.md)

## 1. Overview

core0 (procpu) reads 3-axis acceleration from the HW-664 module (actual
chip: LIS3DH). core1 (appcpu) displays the values on an SSD1306 OLED.
Just like Lab 01/02, the two cores are completely independent Zephyr
images (AMP, built with `west build --sysbuild`), and values are handed
off over IPM.

```
core0 (procpu)                              core1 (appcpu)
────────────────                            ────────────────
[lis3dh_thread] 100ms (fetch accel XYZ, standard Zephyr sensor API)
   -> push immediately if accel vector magnitude moves >=0.15g

[heartbeat_thread] 1000ms - push unconditionally (link keep-alive)

   --(both paths above)--> push_snapshot_to_core1()
                              always sends the FULL snapshot
                              (3 values: accel X/Y/Z)
                                  |
                          ipm_send(channel=2)
                                  |
                                  v  (physical mailbox)
                                        [ipm_callback] (ISR context)
                                                |
                                        k_msgq_put (K_NO_WAIT)
                                                |
                                                v
                                      [display_thread] (k_msgq_get, 2s timeout)
                                                |
                                SSD1306 (I2C1) redraw: LINK, AX/AY/AZ
```

**Push trigger**: pushes immediately when the accel 3-axis vector
magnitude moves by >=0.15g since the last value actually sent to core1
(same threshold as Lab 02's MPU6050 accelerometer). If it doesn't trip,
a 1s heartbeat pushes the full snapshot unconditionally anyway. core1
considers the link down if no message has arrived in 2 seconds.

**Structural similarity to Lab 02**: this lab also has only one sensor,
so no bus-serializing mutex is needed. The address auto-detect (0x18 vs
0x19) uses exactly the same pattern as Lab 02's MPU6050 0x68/0x69
auto-detect.

**Why split the cores, and why split the I2C buses too**: (same
reasoning as Lab 01/02) on ESP32-S3, Zephyr's core0/core1 is AMP, not
SMP - two fully independent images. Letting both cores drive the same
physical I2C controller at once is unsafe, so the sensor lives on I2C0
(owned exclusively by core0) and the display lives on I2C1 (owned
exclusively by core1).

## 2. Sensors

### HW-664 (actual chip: LIS3DH)
- A 3-axis MEMS accelerometer made by STMicroelectronics. **The
  product listing and the module's silkscreen both say LIS3DSH, but
  reading the WHO_AM_I register on real hardware returned `0x33`,
  confirming the actual chip is from the LIS3DH family (LIS3DH/LIS2DH/
  LIS2DH12/LSM303DLHC/LSM303AGR - all register-compatible)** - a
  genuine LIS3DSH would have returned `0x3F`. A good reminder that
  cheap breakout modules sometimes ship with a different part than
  what's printed or listed.
- I2C address: **confirmed on real hardware - SDO/SA0 unconnected or
  HIGH gives `0x19`, LOW gives `0x18`**. The overlay declares both
  addresses as devicetree nodes, and at boot the firmware picks
  whichever one actually reports `device_is_ready()` (`lis3dh_select()`,
  run from the thread's own startup rather than `main()` - a
  `K_THREAD_DEFINE` thread can start before `main()` does). This module
  works fine with CS left unconnected (likely has an onboard pull-up).
- Uses Zephyr's in-tree driver: `drivers/sensor/st/lis2dh/lis2dh.c`
  supports the whole register-compatible family (LIS2DH, LIS3DH,
  LSM303DLHC, LIS2DH12, LSM303AGR) through one driver. Kconfig symbol is
  `CONFIG_LIS2DH=y` (named after the driver file, not the specific chip
  - worth remembering).
  **The devicetree compatible must be `"st,lis2dh"`** - Zephyr's docs
  also list a separate `"st,lis3dh"` binding page, but the actual driver
  doesn't respond to that string, so no device gets instantiated (a
  known, filed Zephyr issue, #31253) - see the [troubleshooting doc](./doc/03_HW664_LIS3DH_TROUBLESHOOTING_en.md) for
  the full story.
- Uses the standard `sensor_sample_fetch()`/
  `sensor_channel_get(SENSOR_CHAN_ACCEL_XYZ)` API, returning m/s^2,
  converted to g here (divide by 9.80665)
- Confirmed on real hardware: default full-scale range is +-2g
  (`fs=2` in the boot log)
- **Datasheet**: [Official ST LIS3DH datasheet (PDF)](https://www.st.com/resource/en/datasheet/lis3dh.pdf)
- Reference retailer:
  [AliExpress HW-664 product page](https://ko.aliexpress.com/item/4000116105737.html?gatewayAdapt=glo2kor)
  (as noted above, a real example of the listing not matching the
  actual part inside)

### SSD1306 (128x64 OLED, owned exclusively by core1)
- Reuses the same custom I2C driver (`ssd1306_display.c`) and font
  (`font5x7.h`) from Lab 01/02 - this avoids a confirmed platform issue
  where the ESP32 Zephyr I2C driver doesn't reliably handle
  multi-segment write transactions
- **Datasheet**: [Solomon Systech SSD1306 datasheet (PDF, Adafruit mirror)](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)

## 3. Wiring

| Signal | ESP32-S3 pin | Connects to | Owning core |
| --- | --- | --- | --- |
| I2C0 SDA | GPIO8 | HW-664 SDA | core0 |
| I2C0 SCL | GPIO9 | HW-664 SCL | core0 |
| I2C1 SDA | GPIO4 | SSD1306 SDA | core1 |
| I2C1 SCL | GPIO5 | SSD1306 SCL | core1 |
| VCC / GND | 3.3V / GND | everything | - |
| CS (optional - unconnected works fine on this module) | - | HW-664 CS | - |
| SDO/SA0 (optional - unconnected gives 0x19 on this module) | GND (0x18) or unconnected/HIGH (0x19) | HW-664 SDO | - |


**Lesson carried over from Lab 01/02**: after wiring up a new I2C
device, scan it with `lab_tools/i2c0_scanner` before putting an address
in the overlay - in this lab, that scan result (0x19) is also what led
to re-examining the chip's actual identity in the first place.

## 4. IPM protocol (`ipm_protocol.h`)

```c
#define IPM_SENSOR_CHANNEL 2   /* same channel as Lab 01/02 - the labs are never flashed onto the same board at once */

struct ipm_sensor_payload {
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    uint8_t valid;
};
```
`lab/src/ipm_protocol.h` and `lab/remote/src/ipm_protocol.h` are two
hand-maintained copies - update both if you change the fields.

## 5. Building

Main lab (dual-core):
```
west build -p always --sysbuild -b esp32s3_devkitc/esp32s3/procpu \
    03_HW664_LIS3DH/lab
west flash
```
`lab/sysbuild.conf` needs `SB_CONFIG_BOOTLOADER_NONE=y` (same reason as
Lab 01/02 - Espressif boards default to building MCUboot alongside the
app whenever `--sysbuild` is used).

I2C bus scanner (diagnostic tool, no sysbuild needed):
```
west build -p always -b esp32s3_devkitc/esp32s3/procpu \
    03_HW664_LIS3DH/lab_tools/i2c0_scanner
west flash
```

## 6. Expected results (confirmed on real hardware)

### core0 boot log
```
[00:00:00.229,000] <err> lis2dh: Failed to read chip id.
[00:00:00.235,000] <inf> lis2dh: fs=2, odr=0x4 lp_en=0x0 scale=9576
*** Booting Zephyr OS build v4.4.0 ***
[00:00:00.237,000] <inf> app_procpu: 03_HW664_LIS3DH (core0/procpu) starting
[00:00:00.242,000] <inf> app_procpu: LIS3DH found at 0x19 (SDO/SA0 high/unconnected)
```
The first line's `Failed to read chip id.` comes from the unwired 0x18
node's init running automatically during kernel boot and failing - this
is **expected, normal behavior** (same pattern as Lab 02's MPU6050
0x68/0x69 auto-detect). The next line confirms 0x19 initialized
successfully with `fs=2` (+-2g, as expected).

### core1 screen
```
LINK: OK

AX:-0.02
AY:0.01
AZ:0.98
```
- At rest, whichever axis is aligned with "down" reads close to ±1.00
  (1g), and the other two read close to 0
- Moving the board is reflected on screen within the next 100ms cycle
- Before the first message arrives, the screen shows `WAITING FOR
  CORE0...`
- If nothing arrives from core0 for 2+ seconds, the top line switches to
  `LINK: FAIL`

## 7. Ideas for extension

- Use LIS3DH's built-in FIFO/interrupt/click-detection (tap/double-tap)
  features to move from polling to event-driven reads
  (`CONFIG_LIS2DH_TRIGGER_*`)
- Switch from +-2g to a wider full-scale range (+-4g/8g/16g)
- Read LIS3DH's auxiliary ADC channels (which can also be used for a
  temperature reading, though Zephyr's driver doesn't expose it as a
  sensor channel) via raw register access, as a bonus temperature
  channel
