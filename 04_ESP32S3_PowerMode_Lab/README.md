[한국어](./README_kr.md) | **English**

# Lab 04: ESP32-S3 Zephyr RTOS Power Mode Lab (HW-664 + SSD1306, Dual-Core AMP)

> Fourth lab in the `zephyr_sensor` series. Built and verified on real hardware with the ESP32-S3-DevKitC-1.
> Issues hit during bring-up and how they were solved: [Troubleshooting](./doc/04_ESP32S3_PowerMode_TROUBLESHOOTING_en.md)

This document is a hands-on guide that combines Zephyr RTOS power management concepts with inter-core communication (IPM) on the ESP32-S3's dual-core (AMP) architecture, turning them into a tangible, low-power scenario you can actually observe on real hardware. This document is self-contained: anything already covered in other labs in this series is re-explained here as needed so you can follow along from start to finish using only this file.

---

## 1. Lab Objectives

- **Understand precisely how the ESP32-S3's hardware power modes relate to the Zephyr RTOS PM subsystem**, and get a hands-on feel for the gap between "what the chip's datasheet claims" and "what Zephyr can actually use today."
- **Low-power synchronization across an AMP layout**: learn how PRO_CPU (core0) and APP_CPU (core1) share power-state information over IPM (Inter-Processor Messaging).
- **Hardware power gating**: implement cutting an external peripheral's (the OLED's) power rail entirely via GPIO, rather than merely blanking its display.
- **Sensor-event-driven wake-up**: implement a scenario where a sharp change (a threshold crossing) detected by polling the accelerometer (HW-664 / LIS3DH) automatically brings the system back out of a low-power mode.

---

## 2. What You'll Need

### 2.1 Hardware

| Part | Notes |
| --- | --- |
| ESP32-S3-DevKitC-1 | The base board used throughout this series |
| HW-664 accelerometer module | The module's silkscreen/listing says LIS3DSH, but the chip actually on the board is **LIS3DH** (see [Lab 03](../03_HW664_LIS3DH/README.md)) |
| SSD1306 128x64 OLED (I2C) | 4-pin (VCC/GND/SDA/SCL) module |
| A switch for OLED power gating | A P-MOSFET (high-side) or a small load-switch IC is recommended — see section 5.3 |
| Breadboard / jumper wires | |

### 2.2 Build Environment

- **west**: every build command in this document assumes west.
- **Zephyr IDE (VS Code extension)**: simply open your west workspace as-is; no extra setup is needed just for this lab.
- Zephyr RTOS v4.4+ (west-based), Zephyr SDK (Xtensa toolchain), and the Espressif HAL binaries fetched with `west blobs fetch hal_espressif`

---

## 3. Pin Mapping

| Function / Device | ESP32-S3 Pin | Connects To | Owning Core | Notes |
| --- | --- | --- | --- | --- |
| I2C0 SDA | GPIO8 | HW-664 SDA | core0 | This series' standard I2C0 convention |
| I2C0 SCL | GPIO9 | HW-664 SCL | core0 | |
| I2C1 SDA | GPIO4 | SSD1306 SDA | core1 | |
| I2C1 SCL | GPIO5 | SSD1306 SCL | core1 | |
| OLED power gate | GPIO6 (`&gpio0` pin 6) | Power switch control input | core1 | HIGH = OLED powered ON, LOW = OFF |
| Boot button | GPIO0 (onboard) | Onboard BOOT switch | core0 | Active-LOW, used for mode cycling |
| VCC / GND | 3.3V / GND | Common to all modules | - | |
| HW-664 CS | Not connected | - | - | Leaving CS unconnected is normal for this module |
| HW-664 SDO/SA0 | GND, or unconnected/HIGH | - | - | GND → 0x18, unconnected/HIGH → 0x19 (this module's default) |

The OLED power-gate pin and the Boot button both physically belong to the `gpio0` controller, but each is declared independently in a different core's overlay (core1 / core0 respectively) — as with every other lab in this series, a single pin is always owned by exactly one core's image, never shared.

---

## 4. Power Mode Concepts

### 4.1 ESP32-S3 Chip-Level Power Modes (per Espressif)

| Mode | CPU | What's Retained | Notes |
| --- | --- | --- | --- |
| Active | ON | Everything | Normal operating state |
| Modem-Sleep | ON | All of CPU/RAM; only the Wi-Fi/BT PHY cycles OFF periodically | Only meaningful when Wi-Fi/BT is in use — **not applicable to this lab, which never touches radios** |
| Light-Sleep | Paused | RAM/register contents preserved | Fast resume, multiple possible wake sources |
| Deep-Sleep | OFF | Only the RTC domain | Resuming is effectively a reboot |

### 4.2 What the Zephyr RTOS PM Subsystem Actually Supports (ESP32-S3)

The chip datasheet lists four modes, but **the only one Zephyr's ESP32-S3 SoC PM driver actually supports through the standard `pm_state_set()` path is Light-Sleep (`PM_STATE_SUSPEND_TO_IDLE`)**. Enabling `CONFIG_PM=y` makes the system automatically enter Light-Sleep whenever it goes idle — and Zephyr's own official sample (`samples/boards/espressif/light_sleep`) documents that "wake-up from light sleep is supported only via an RTC timer," meaning **the wake source is restricted to the RTC timer**. There is no standard path to wake from Light-Sleep via a GPIO interrupt.

Deep-Sleep can be reached at the hardware level via `sys_poweroff()` combined with `esp_sleep_enable_timer_wakeup()`, but this combination has a known, reported upstream issue on both ESP32 and ESP32-S3 that causes watchdog-reset loops or outright boot failure (zephyr-rtos/zephyr#86193, closed as "not planned"). On top of that, this lab's ultra-low-power mode still needs to **keep reading the sensor once a second**, and a genuine Deep-Sleep powers off the CPU/RAM entirely — without a ULP co-processor, no code at all can run in the meantime.

**This lab does not use `CONFIG_PM` (Zephyr System Power Management) at all.** We did get real Light-Sleep working on real hardware with `CONFIG_PM=y` during development, but confirmed that on this particular Zephyr snapshot (`v4.4.0-13070-g6d1d551f6080`), the ESP32-S3 SoC PM driver does not yet coexist reliably with this specific board/driver combination. The full attempt history, root causes, and hardware logs are written up in a separate document, [`doc/04_ESP32S3_PowerMode_TROUBLESHOOTING_en.md`](./doc/04_ESP32S3_PowerMode_TROUBLESHOOTING_en.md) — if you want to try reviving real Light-Sleep yourself, start there. Modem-Sleep is likewise out of scope, since this lab never touches a radio at all.

In other words, this lab's three "low-power modes" are not implemented through the Zephyr PM subsystem — they are **a policy implemented entirely at the application level**: a combination of power gating (cutting the OLED's VCC), changing the sampling interval, and a software reset on trigger. The point of this lab is less "how deeply asleep can the chip actually get" and more "how well can the application throttle its own peripherals and activity based on context." Sections 5–6 below define this policy in detail.

---

## 5. System Architecture

### 5.1 Dual-Core Role Split

```text
core0 (procpu)                         core1 (appcpu)
-----------------------------          -----------------------------
I2C0 -- HW-664 (LIS3DH)                I2C1 -- SSD1306 OLED
Boot button (mode cycling)             OLED power-gate GPIO
Owns the mode state machine            Screen rendering / link-loss detection
        |                                       ^
        +--------------- IPM (ipm0) ------------+
```

core0 acts as the "brain": it reads the sensor and makes every mode-transition decision. core1 acts purely as the "output": it takes instructions from core0 and only ever touches the screen and the OLED's power. core1 never reads the sensor directly, and core0 never draws to the screen directly.

### 5.2 IPM Message Types (`ipm_protocol.h`)

| Message | Direction | Meaning |
| --- | --- | --- |
| `IPM_CMD_ACCEL_AVG` | core0 → core1 | Every 1000 ms: the average of the last 10 samples (100 ms apart). Sent only in Normal mode |
| `IPM_CMD_ACCEL_ALERT` | core0 → core1 | Sent immediately, independent of the periodic cycle, whenever a delta ≥ 0.15 g is detected while in Normal mode |
| `IPM_CMD_DISPLAY_SLEEP` | core0 → core1 | Instructs core1 to power off the OLED |
| `IPM_CMD_DISPLAY_WAKE` | core0 → core1 | Instructs core1 to power the OLED back on and re-initialize it from scratch |

This header exists **identically in both** `lab/src/ipm_protocol.h` and `lab/remote/src/ipm_protocol.h` (the two images are separate build trees with no shared include path, so the content is deliberately duplicated). If you change the protocol, you must update both copies together.

### 5.3 A Note on OLED Power-Gating Wiring and its Devicetree Representation

The `oled-pwr` GPIO is meant to cut the SSD1306 module's VCC line entirely. These modules typically draw roughly 20–30 mA when active, which does not leave much headroom against an ESP32-S3 GPIO pin's absolute maximum source current (~40 mA). For that reason, **we recommend gating the OLED's power through a P-MOSFET high-side switch or a small load-switch IC, rather than driving VCC directly from the GPIO**. Wiring VCC directly to the GPIO will technically work for a quick demo, but is not recommended for a finished lab setup or blog write-up.

On the devicetree side, this pin **must be declared as a child node of the `gpio-leds` binding**. A node with a bare `gpios` property and no `compatible` will compile fine as an overlay, but the C macro that `GPIO_DT_SPEC_GET()` needs is never generated, and the application build fails.

---

## 6. State Machine in Detail

```text
                    Boot button              Boot button
   [NORMAL] -----------------> [SLEEP] -----------------> [ULTRA_SLEEP]
      ^                           |                             |
      |                           |                             |
      +--- delta >= 0.15g --------+                             |
      |    (core1: WAKE, re-init)                                |
      |                                                          |
      +---------------------- sys_reboot() <--- delta >= 0.15g --+
           (after reboot, the firmware always starts in NORMAL)
```

### 6.1 NORMAL

- core0 reads the HW-664 every 100 ms. Every 10 samples (= 1000 ms), it computes the average and sends it to core1 as `IPM_CMD_ACCEL_AVG`.
- core0 also checks every single sample: if the accel vector magnitude has moved by ≥ 0.15 g relative to the last value it actually sent, it fires `IPM_CMD_ACCEL_ALERT` immediately, independent of the periodic cycle.
- core1 renders whatever it receives and shows the link status (`LINK: OK` / `LINK: FAIL`) on the first line. If no periodic data arrives for 3 cycles (= 3000 ms), it switches to `LINK: FAIL`.
- Pressing the Boot button transitions to SLEEP and sends `IPM_CMD_DISPLAY_SLEEP` to core1.

### 6.2 SLEEP

- core0 still samples every 100 ms (so the wake response stays fast), but stops sending `IPM_CMD_ACCEL_AVG`/`ALERT` — there is no point pushing data to a screen that's off.
- On receiving `IPM_CMD_DISPLAY_SLEEP`, core1 drives the `oled-pwr` GPIO LOW, physically cutting the OLED's power.
- If the accel delta reaches ≥ 0.15 g while in this state, core0 immediately returns to NORMAL and sends `IPM_CMD_DISPLAY_WAKE` to core1. core1 drives `oled-pwr` HIGH and **fully re-initializes the OLED from scratch**, since the panel's internal state was completely lost when its power was cut.
- Pressing the Boot button once more from here transitions to ULTRA_SLEEP (core1 needs no separate notification for this — the screen simply stays in whatever off state it was already in).

### 6.3 ULTRA_SLEEP

- core0 slows its sensor-reading interval to 1000 ms and does not communicate with core1 at all.
- This mode is a **software-only low-power policy that does not use Zephyr Light-Sleep** (see section 4.2). The CPU remains fully Active the entire time, simply looping on `k_msleep(1000)`, so while resting the sensor and display does save some power, there is no meaningful chip-level (SoC) power saving here.
- If the accel delta reaches ≥ 0.15 g, the firmware performs a **software reboot** via `sys_reboot(SYS_REBOOT_WARM)`. Since the rebooted firmware always starts in NORMAL, the net effect is "reboot, and land back in Normal mode."

---

## 7. File Layout

```text
04_ESP32S3_PowerMode_Lab/
├── README_kr.md                                    (Korean)
├── README.md                                       (English, this document)
├── doc/
│   ├── 04_ESP32S3_PowerMode_TROUBLESHOOTING_kr.md  (Korean troubleshooting notes)
│   └── 04_ESP32S3_PowerMode_TROUBLESHOOTING_en.md  (English troubleshooting notes)
└── lab/
    ├── CMakeLists.txt                (core0 / procpu)
    ├── prj.conf
    ├── sysbuild.conf
    ├── sysbuild.cmake
    ├── sample.yaml
    ├── boards/
    │   └── esp32s3_devkitc_esp32s3_procpu.overlay
    ├── src/
    │   ├── main.c
    │   ├── sensor_hub.c
    │   ├── sensor_hub.h
    │   └── ipm_protocol.h
    └── remote/                       (core1 / appcpu)
        ├── CMakeLists.txt
        ├── prj.conf
        ├── boards/
        │   └── esp32s3_devkitc_esp32s3_appcpu.overlay
        └── src/
            ├── main.c
            ├── ipm_protocol.h
            ├── ssd1306_display.c
            ├── ssd1306_display.h
            └── font5x7.h
```

---

## 8. Build and Run

From the root of your west workspace:

```bash
west build -p always --sysbuild -b esp32s3_devkitc/esp32s3/procpu 04_ESP32S3_PowerMode_Lab/lab
west flash
west espressif monitor
```

If you're using the Zephyr IDE, just create one build target with the same settings as the west command above (board: `esp32s3_devkitc/esp32s3/procpu`, sysbuild enabled, application path: `04_ESP32S3_PowerMode_Lab/lab`).

---

## 9. Verification Checklist

1. Check the boot log for `HW-664 (LIS3DH) found at I2C0 address 0x1x` on core0's console and `SSD1306 found at 0x3x` on core1's console (both are auto-address-scan results).
2. With the board left still, confirm the screen shows `LINK: OK` with AX/AY/AZ values refreshing once a second.
3. Shake the board quickly to produce a change ≥ 0.15 g, and confirm the screen switches to `IMPACT!` immediately, independent of the 1-second cycle.
4. Press the Boot button once and confirm the OLED turns off completely (including its backlight, not just a blank screen).
5. While in that state, shake the board and confirm the OLED re-initializes, turns back on, and briefly shows `NORMAL MODE`.
6. Press the Boot button again to go to SLEEP, then once more to transition to ULTRA_SLEEP (the screen stays off throughout, so you can only observe the state via core0's console log).
7. While in ULTRA_SLEEP, shake the board and confirm a reboot log appears on core0's console, the board restarts, and it comes back up in NORMAL state (screen on).
8. (Optional) Reset only core0, or deliberately interrupt the IPM link, and confirm core1's screen shows `LINK: FAIL`.

---

## 10. Notes and Cautions

- **OLED power-gating wiring**: see section 5.3 — a MOSFET or load-switch is recommended over driving VCC directly from the GPIO.
- **ISR context**: both the Boot button's GPIO callback and the IPM callback run in interrupt context. If you modify this code, be careful never to call `k_sleep()` or any other blocking API directly from inside them (this lab's code only sets a flag or pushes to a message queue from the callbacks — actual processing always happens in the main loop or a dedicated thread).
- **Adding a plain GPIO output pin to a devicetree overlay**: a control-only pin with no real driver behind it, like `oled-pwr`, must be declared as a child node of the `gpio-leds` binding (see section 5.3) — a `gpios` property with no `compatible` will build fine as an overlay, but breaks the C build the moment `GPIO_DT_SPEC_GET()` is used on it.
- The build/runtime issues encountered while validating this lab on real hardware (particularly the `CONFIG_PM` trial-and-error) and how they were diagnosed and resolved are written up in a separate document, [`doc/04_ESP32S3_PowerMode_TROUBLESHOOTING_en.md`](./doc/04_ESP32S3_PowerMode_TROUBLESHOOTING_en.md).
