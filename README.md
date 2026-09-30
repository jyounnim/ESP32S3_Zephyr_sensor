[한국어](./README_kr.md) | **English**

# Zephyr Multi-Sensor Example Series (ESP32-S3, Dual-Core + IPM)

A collection of learning examples built on the ESP32-S3-DevKitC-1 with
Zephyr RTOS, where a new I2C sensor is added one lab at a time. The
sensor changes from lab to lab, but the **core architecture stays the
same across every lab**: sensor reading and display output are each
assigned to one of ESP32-S3's two cores.

## Core Architecture: Splitting Work Across the Two Cores

ESP32-S3 physically has two cores (procpu and appcpu). Zephyr treats
these not as SMP (one kernel scheduling both cores) but as **AMP** -
two completely independent Zephyr images. This series leans directly on
that structure to give each core a clear, single job.

```
        core0 (procpu)                         core1 (appcpu)
   ─────────────────────                  ─────────────────────
   Owns sensor reading                    Owns the display
   I2C0 (SDA=GPIO8, SCL=GPIO9)             I2C1 (SDA=GPIO4, SCL=GPIO5)
        │                                        │
   A sensor thread reads periodically,      Draws values on an SSD1306 OLED
   pushes immediately on a big change,      (event-driven display thread)
   otherwise on a steady heartbeat                │
        └──────────── IPM (inter-processor mailbox) ────────────┘
                        (channel 2, ipm_send / ipm_callback)
```

- **core0 (procpu) — owns sensor reading**: each sensor gets its own
  thread that reads it periodically. Whenever a value moves past a
  threshold, it's pushed to core1 immediately over `ipm_send()`; even
  when nothing changes, a 1-second heartbeat still pushes once so core1
  has a way to know the link is alive.
- **core1 (appcpu) — owns the display**: the IPM callback (interrupt
  context) only copies the incoming value into a message queue; a
  separate display thread consumes that queue and redraws the SSD1306
  OLED. If nothing arrives for a while (usually 2 seconds), the screen
  shows the link as down.

**Why split the cores, and why split the I2C buses too**: since
core0/core1 are two fully independent images under Zephyr (AMP), having
both images drive the same physical I2C controller at once would be
unsafe. So sensors always live on I2C0 (owned exclusively by core0) and
the display always lives on I2C1 (owned exclusively by core1) - this
avoids the problem structurally rather than by convention alone.

## Shared Conventions

- **Directory layout**: each lab is its own folder named
  `NN_ModuleName_ChipName/`, laid out consistently:
  ```
  NN_ModuleName_ChipName/
  ├── README.md / README_kr.md             lab guide (English / Korean)
  ├── doc/                                  troubleshooting notes (_en.md / _kr.md)
  ├── lab/                                  main application (built with west build --sysbuild)
  │   ├── src/                              core0 (procpu) sources
  │   ├── remote/src/                       core1 (appcpu) sources
  │   ├── boards/*.overlay                  core0 overlay
  │   ├── remote/boards/*.overlay           core1 overlay
  │   ├── sysbuild.cmake / sysbuild.conf
  │   └── prj.conf / remote/prj.conf
  └── lab_tools/i2c0_scanner/               diagnostic tool for confirming a new I2C device's address (Labs 01-03)
  ```
- **Building**:
  ```
  west build -p always --sysbuild -b esp32s3_devkitc/esp32s3/procpu <lab_path>/lab
  west flash
  ```
- **sysbuild + MCUboot gotcha**: Espressif boards default to building
  MCUboot alongside the app whenever `--sysbuild` is used. This series
  is a plain two-image (procpu + appcpu) build with nothing to do with
  OTA, so each lab's `lab/sysbuild.conf` explicitly turns that off with
  `SB_CONFIG_BOOTLOADER_NONE=y`.
- **IPM channel**: Labs 01-03 send every snapshot with ID 2 (0/1 are
  reserved by the platform, 2/3 are free for application use). Lab 04
  carries a command code (`ipm_msg_t.cmd`) in the payload instead, since
  it exchanges several message types.
- **Wiring a new sensor**: scan for its real address with
  `lab_tools/i2c0_scanner` before hardcoding anything into an overlay -
  a module's actual wiring often doesn't match the datasheet's default
  address.
- **Documentation flow**: only Korean docs are written before real
  hardware verification; once verification passes, the docs are cleaned
  up and an English version is written alongside. Every document has a
  language switch link at the top.

## Labs So Far

**This series keeps growing** - a new lab gets added every time another
sensor joins the lineup. The table below reflects progress so far.

| # | Folder | Module / Chip | Sensor type | Status |
| --- | --- | --- | --- | --- |
| 01 | [`01_AHT20_BMP280_MultiSensor`](./01_AHT20_BMP280_MultiSensor) | AHT20 + BMP280 | Temperature/humidity + pressure (two sensors sharing one I2C0 bus) | ✅ Verified on real hardware |
| 02 | [`02_GY521_MPU6050`](./02_GY521_MPU6050) | GY-521 (MPU-6050) | 6-axis IMU (3-axis accel + 3-axis gyro + temperature) | ✅ Verified on real hardware |
| 03 | [`03_HW664_LIS3DH`](./03_HW664_LIS3DH) | HW-664 (actual chip: LIS3DH - the listing said LIS3DSH, corrected via WHO_AM_I) | 3-axis accelerometer | ✅ Verified on real hardware |
| 04 | [`04_ESP32S3_PowerMode_Lab`](./04_ESP32S3_PowerMode_Lab) | HW-664 (LIS3DH) + SSD1306 power gating | Power modes: core0 = always-on sensing, core1 = display power control (Normal / Sleep / Ultra-Sleep) | ✅ Verified on real hardware |

Each lab folder's `README.md` covers design, wiring, build, and expected
results; troubleshooting notes live under that lab's `doc/` folder.

## Checklist for Starting a New Lab

1. Wire up the new sensor.
2. Scan for its real address with `lab_tools/i2c0_scanner` before
   writing anything into an overlay.
3. Check whether Zephyr has official driver support - if so, use the
   standard `sensor_sample_fetch()`/`sensor_channel_get()` API; if not,
   write a custom raw-I2C driver the same way this series did for the
   SSD1306.
4. Apply the pattern established in Labs 01-03 as-is: dual-core + IPM +
   threshold-triggered push + heartbeat.
5. Write the Korean docs first, verify on real hardware, then clean up
   the docs and add the English translation.

## Environment

- Board: ESP32-S3-DevKitC-1 (N16R8, 16MB flash / 8MB PSRAM)
- Zephyr: v4.4.0
- Build system: west + sysbuild
