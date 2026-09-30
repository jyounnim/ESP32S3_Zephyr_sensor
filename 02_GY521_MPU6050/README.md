[한국어](./README_kr.md) | **English**

# Lab 02: GY-521 (MPU-6050) 6-Axis IMU (Dual-Core, IPM, SSD1306)

> Second lab in the `zephyr_sensor` series. Built and verified on real
> hardware with the ESP32-S3-DevKitC-1.
> Issues hit during bring-up and how they were solved: [Troubleshooting](./doc/02_GY521_MPU6050_TROUBLESHOOTING_en.md)

## 1. Overview

core0 (procpu) reads 3-axis acceleration, 3-axis angular rate, and die
temperature from a GY-521 module (MPU-6050 chip). core1 (appcpu) displays
those values on an SSD1306 OLED. Just like Lab 01, the two cores are
completely independent Zephyr images (AMP, built with `west build
--sysbuild`), and values are handed off over ESP32's IPM (inter-processor
mailbox).

```
core0 (procpu)                              core1 (appcpu)
────────────────                            ────────────────
[mpu6050_thread] 100ms (fetch accel + gyro + temperature)
   -> push immediately if accel vector magnitude moves >=0.15g
   -> push immediately if gyro vector magnitude moves >=15 deg/s

[heartbeat_thread] 1000ms - push unconditionally (link keep-alive)

   --(all three paths above)--> push_snapshot_to_core1()
                                    always sends the FULL snapshot
                                    (7 values: accel X/Y/Z, gyro X/Y/Z,
                                     temperature)
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
                                SSD1306 (I2C1) redraw: LINK, AX/AY/AZ, GX/GY/GZ, TEMP
```

**Push triggers**: pushes immediately when the accel 3-axis vector
magnitude (sqrt(ax^2+ay^2+az^2)) moves by >=0.15g, or the gyro vector
magnitude moves by >=15 deg/s, since the last value actually sent to
core1. If neither trips, a 1s heartbeat pushes the full snapshot
unconditionally anyway. core1 considers the link down if no message has
arrived in 2 seconds.

**Structural difference from Lab 01**: Lab 01 has two sensors
(AHT20/BMP280) sharing one I2C0 bus, which required a `g_i2c0_bus_lock` to
serialize the two threads' bus access. This lab has only one sensor
(MPU6050), so there's no such contention to worry about and no bus lock
is needed - the thread structure is simpler as a result.

**Why split the cores, and why split the I2C buses too**: (same reasoning
as Lab 01) on ESP32-S3, Zephyr's core0/core1 is AMP, not SMP - two fully
independent images. Letting both cores drive the same physical I2C
controller at once is unsafe, so the sensor lives on I2C0 (owned
exclusively by core0) and the display lives on I2C1 (owned exclusively by
core1).

## 2. Sensors

### GY-521 / MPU-6050 (3-axis accel + 3-axis gyro + temperature)
- Made by InvenSense (now TDK): a 6-axis motion tracking device combining
  a 3-axis MEMS accelerometer, a 3-axis MEMS gyroscope, and a temperature
  sensor on one chip
- I2C address: `0x68` if the AD0 pin is GND or floating (most GY-521
  boards have an onboard pull-down), `0x69` if tied to VCC - **both
  confirmed on real hardware with a bus scan**
- **Address auto-detect**: the overlay declares both the `0x68` and
  `0x69` nodes, and at boot the firmware picks whichever one actually
  reports `device_is_ready() == true`. Zephyr's mpu6050 driver reads the
  WHO_AM_I register during init, so the address with no real hardware
  behind it simply fails to initialize - this lets the same firmware
  work whether AD0 is wired to GND or VCC, with no raw I2C bypass needed
  (see `mpu6050_select()` in `main.c`). This selection runs at the start
  of the `mpu6050_thread` itself, not in `main()` - a thread registered
  with `K_THREAD_DEFINE` can start running before `main()` does, so
  having `main()` pick the device and the thread read that choice later
  would be a race condition. This project uses 0x68 (AD0 = GND), and
  that's been confirmed working on real hardware (boot log shows
  `MPU6050 found at 0x68`).
- Uses Zephyr's in-tree driver: devicetree compatible
  `"invensense,mpu6050"` (`drivers/sensor/tdk/mpu6050/mpu6050.c`), Kconfig
  `CONFIG_MPU6050=y`
- Units the driver returns: acceleration via `SENSOR_CHAN_ACCEL_XYZ` in
  m/s^2, angular rate via `SENSOR_CHAN_GYRO_XYZ` in rad/s, temperature via
  `SENSOR_CHAN_DIE_TEMP` in Celsius. This lab converts acceleration to g
  (divide by 9.80665) and angular rate to deg/s (multiply by 180/pi) for
  a more intuitive display
- This lab uses polling mode with no interrupt pin
  (`CONFIG_MPU6050_TRIGGER_NONE=y`) - the thread calls
  `sensor_sample_fetch()` directly every 100ms
- **Datasheet**: [Official InvenSense MPU-6000/6050 Register Map (PDF)](https://www.invensense.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf)
  (the product specification document's official link tends to move
  around since InvenSense's acquisition by TDK, so searching for
  "MPU-6000 and MPU-6050 Product Specification Revision 3.4" is a good
  way to find a current mirror)
- Reference retailer (Korea):
  [Devicemart GY-521 product page](https://www.devicemart.co.kr/goods/view?no=1247052)

### SSD1306 (128x64 OLED, owned exclusively by core1)
- Reuses the same custom I2C driver (`ssd1306_display.c`) from Lab 01 -
  this avoids a confirmed platform issue where the ESP32 Zephyr I2C
  driver doesn't reliably handle multi-segment write transactions
- **Datasheet**: [Solomon Systech SSD1306 datasheet (PDF, Adafruit mirror)](https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf)
- The font (`font5x7.h`) is based on Lab 01's, with uppercase 'X' and 'Z'
  added for this lab's axis labels (AX/AY/AZ/GX/GY/GZ), and the lowercase
  'h'/'a' glyphs Lab 01 needed but this lab doesn't removed, keeping the
  set minimal for what this lab actually prints

## 3. IPM protocol (`ipm_protocol.h`)

```c
#define IPM_SENSOR_CHANNEL 2   /* same channel as Lab 01 - the two labs are never flashed on the same board at once, so no real conflict */

struct ipm_sensor_payload {
    float accel_x_g;
    float accel_y_g;
    float accel_z_g;
    float gyro_x_dps;
    float gyro_y_dps;
    float gyro_z_dps;
    float temp_c;
    uint8_t valid;
};
```
`lab/src/ipm_protocol.h` and `lab/remote/src/ipm_protocol.h` are two
hand-maintained copies - update both if you change the fields (same
caveat as Lab 01).

## 4. Wiring

| Signal | ESP32-S3 pin | Connects to | Owning core |
| --- | --- | --- | --- |
| I2C0 SDA | GPIO8 | GY-521 SDA | core0 |
| I2C0 SCL | GPIO9 | GY-521 SCL | core0 |
| I2C1 SDA | GPIO4 | SSD1306 SDA | core1 |
| I2C1 SCL | GPIO5 | SSD1306 SCL | core1 |
| VCC / GND | 3.3V / GND | everything | - |
| AD0 | GND (or unconnected) | GY-521 AD0 | - (pins the address to 0x68) |

**Lesson carried over from Lab 01**: after wiring up a new I2C device,
scan it with `lab_tools/i2c0_scanner` before putting an address in the
overlay. This lab happens to auto-detect either 0x68 or 0x69 with no code
changes needed, so scanning isn't strictly required for this specific
sensor - but it's still good practice for anything new.

## 5. Building

Main lab (dual-core):
```
west build -p always --sysbuild -b esp32s3_devkitc/esp32s3/procpu \
    02_GY521_MPU6050/lab
west flash
```
`lab/sysbuild.conf` needs `SB_CONFIG_BOOTLOADER_NONE=y` (same reason as
Lab 01 - Espressif boards default to building MCUboot alongside the app
whenever `--sysbuild` is used).

I2C bus scanner (diagnostic tool, no sysbuild needed):
```
west build -p always -b esp32s3_devkitc/esp32s3/procpu \
    02_GY521_MPU6050/lab_tools/i2c0_scanner
west flash
```

## 6. Expected results

### core0 boot log (normal)
```
[00:00:00.225,000] <err> MPU6050: Failed to read chip ID.
*** Booting Zephyr OS build v4.4.0 ***
[00:00:00.226,000] <inf> app_procpu: 02_GY521_MPU6050 (core0/procpu) starting
[00:00:00.231,000] <inf> app_procpu: MPU6050 found at 0x68 (AD0 low/floating)
```
The `Failed to read chip ID.` line comes from the unwired 0x69 node's
init running automatically during kernel boot and failing - this is
**expected, confirmed-normal behavior** (see the [troubleshooting doc](./doc/02_GY521_MPU6050_TROUBLESHOOTING_en.md)). If
it bothers you, the MPU6050 module's log level can be lowered to quiet
it.

### core1 screen (confirmed on real hardware)
```
LINK: OK
AX:-0.02
AY:0.01
AZ:0.98
GX:-0.3
GY:0.1
GZ:0.0
TEMP:24.3C
```
- At rest, only gravity acts on the sensor, so whichever axis is aligned
  with "down" reads close to ±1.00 (1g), and the other two read close to
  0
- At rest, all three gyro axes should read close to 0 (some noise/bias is
  normal)
- Moving or rotating the board should be reflected on screen within the
  next 100ms cycle - confirmed on real hardware that the threshold-based
  immediate push actually works this way
- Before the first message arrives, the screen shows `WAITING FOR
  CORE0...`
- If nothing arrives from core0 for 2+ seconds, the top line switches to
  `LINK: FAIL`

## 7. Ideas for extension

- Show a distinct event when the accel vector magnitude crosses a much
  higher threshold (e.g. >=2g, for basic drop/impact detection)
- Integrate the gyro readings on core1 into a rough pitch/roll estimate
  (a good teaching point for demonstrating gyro drift, since it won't be
  precise)
- Switch to MPU6050's built-in FIFO/interrupt mode instead of polling
  (`CONFIG_MPU6050_TRIGGER_GLOBAL_THREAD` etc.)
- Add units (g, deg/s) to the on-screen values - this needs a few more
  lowercase glyphs in the font first (considered for this revision but
  held off)
