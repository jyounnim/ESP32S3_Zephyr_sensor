[한국어](./README_kr.md) | **English**

# ESP32-S3 Dual-core AMP: HLK-LD2402 Radar + SSD1306 Display (Zephyr)

> Status: **Verified on real hardware (2026-10-02)**. Passed host-level syntax checks (gcc, Zephyr API stubs) and the LD2402 parser unit tests, then confirmed against real hardware (ESP32-S3-DevKitC-1 + HLK-LD2402 + SSD1306) through the verification checklist in section 7.
> For sensor protocol details, see [`HLK-LD2402_sensor_research_kr.md`](../HLK-LD2402_sensor_research_kr.md) (Korean) and Hi-Link's official user manual (`HLK-LD2402_User_Manual.pdf`), both at the repo root.

## 1. Goal

- **Core 0 (PRO_CPU)**: the **sensor-only core** — receives LD2402 UART frames, parses them, and configures engineering mode.
- **Core 1 (APP_CPU)**: the **display-only core** — draws status, distance, and a 16-gate energy bar graph on the SSD1306.
- The two cores are fully independent Zephyr images (AMP), exchanging 56-byte messages over soft IPM.

This lab extends the same structure used by multi-sensor AMP Labs 01-04 (core0 = sensor, core1 = SSD1306, IPM, LINK status) to the LD2402 radar.

## 2. Architecture

```
 LD2402 ──UART1 115200──▶ [PRO_CPU] ld2402.c ─ parser ─▶ sensor_main.c
                                                         │ radar_msg (56B)
                                                         ▼ ipm_send()
                                             ipm0 shared memory (512B per direction)
                                                         │ FROM_CPU IRQ
                                                         ▼
                           [APP_CPU] ISR copy → k_msgq → display_main.c → radar_display_ssd1306.c → ssd1306_display.c (raw I2C) → SSD1306 (I2C1)
```

| Item | PRO_CPU (sensor) | APP_CPU (display) |
|---|---|---|
| Board target | `esp32s3_devkitc/esp32s3/procpu` | `esp32s3_devkitc/esp32s3/appcpu` (built automatically by sysbuild) |
| Peripheral | UART1 (GPIO17 TX / GPIO18 RX) | I2C1 (GPIO4 SDA / GPIO5 SCL) |
| IPC | `radar_link_send()` → `ipm_send()` | `ipm_register_callback()` → `k_msgq` |
| Console | UART0 (USB) | none (the screen is the only output) |

### Send policy (sensor core)

| Condition | Action |
|---|---|
| presence / flags / **direction** changes | send immediately |
| distance change ≥ 10 cm | send immediately |
| otherwise | send every 500 ms (doubles as the bar-graph refresh and heartbeat) |
| no radar frame for 2 s | clear `RADAR_FLAG_SENSOR_OK` → screen shows `NO SENSOR` |

The display core shows `LINK: FAIL` if no message arrives for 3 seconds.

### IPM usage notes (based on `drivers/ipm/ipm_esp32.c`)

- The receive callback runs in **ISR context** and is handed only a shared-memory pointer, not a payload size — so the message carries its own `magic` field for validation.
- The next `ipm_send()` overwrites the same buffer, so the receiver must copy it out immediately inside the callback. This lab's send period is ≥165 ms, so no overwrite race occurs in practice.
- Turning on `CONFIG_IPM=y` makes PRO_CPU load and start the APP_CPU image (`SOC_ENABLE_APPCPU`). `CONFIG_ESP32_SOFT_IPM` is selected automatically from devicetree.

## 3. Bill of Materials and Wiring

| LD2402 | ESP32-S3-DevKitC-1 |
|---|---|
| V | 3V3 |
| G | GND |
| T (TX) | GPIO18 (UART1 RX) |
| R (RX) | GPIO17 (UART1 TX) |
| IO | unused |

The onboard Boot button (GPIO0) is reused as-is (for triggering auto-threshold).

| SSD1306 128x64 (I2C) | ESP32-S3-DevKitC-1 |
|---|---|
| VCC / GND | 3V3 / GND |
| SDA | GPIO4 (I2C1) |
| SCL | GPIO5 (I2C1) |

- Both devices are 3.3 V IO, so no level shifter is needed.
- The LD2402 draws about 50 mA on average. The DevKit's 3V3 rail is sufficient, but adding a 100 µF decoupling capacitor is recommended if possible.
- The SSD1306 address is auto-probed at boot (0x3C, then 0x3D) by the raw driver, so it is not hardcoded in the overlay.

## 4. File Layout

```text
05_HLKLD2402_Radar/
├── README_kr.md                          (Korean)
├── README.md                             (English, this document)
└── lab/                                  ← PRO_CPU (sensor) app + sysbuild entry point
    ├── CMakeLists.txt  Kconfig  prj.conf
    ├── sysbuild.cmake  sysbuild.conf     ← adds the APP_CPU image, disables MCUboot
    ├── boards/esp32s3_devkitc_esp32s3_procpu.overlay
    ├── common/   radar_msg.h  radar_link.h  radar_link_ipm.c
    ├── src/      sensor_main.c  ld2402.c/h  radar_trend.c/h  radar_filter.c/h
    └── remote/                           ← APP_CPU (display) app
        ├── CMakeLists.txt  prj.conf
        ├── boards/esp32s3_devkitc_esp32s3_appcpu.overlay
        └── src/  display_main.c  radar_display.h  radar_display_ssd1306.c
                  ssd1306_display.c/h  font5x7.h   ← driver verified in multi-sensor AMP Lab 01
```

`sensor_main.c`, `display_main.c`, `ld2402.*`, `radar_trend.*`, `radar_filter.*`, `radar_display.h`, `radar_msg.h`, and `radar_link.h` are **byte-for-byte identical to the SR110 project's files.** The only platform-specific files are the IPC backend (`radar_link_ipm.c`), the display backend (`radar_display_ssd1306.c`), the overlays, and the `.conf` files.

## 5. Code Walkthrough

### 5.1 `ld2402.c`: one state machine handling three frame formats

```
ST_SYNC ─(window==F4F3F2F1)─▶ LEN ─▶ PAYLOAD ─▶ FOOTER(F8F7F6F5) ─▶ EVT_REPORT
        ─(window==FDFCFBFA)─▶ LEN ─▶ PAYLOAD ─▶ FOOTER(04030201) ─▶ EVT_ACK
        ─('O' or 'd')──────▶ ST_ASCII ─('\r'/'\n')─▶ "OFF" / "distance:N" ─▶ EVT_REPORT
```

- Reception goes UART RX IRQ → `ring_buf` → `k_sem`; the ISR only produces data, while parsing happens on the sensor thread.
- Setting `CONFIG_LD2402_RX_POLLING=y` switches to `uart_poll_in()`-based polling (a debug fallback).
- `ld2402_enter_engineering_mode()` proceeds enable(0x00FF) → version(0x0000) → mode(0x0012) → end(0x00FE). Even if an intermediate step fails, **the end-config command is always sent** — otherwise the radar stops reporting.

### 5.2 `radar_msg.h`: the inter-core contract

A fixed 56-byte struct with no padding (checked via `_Static_assert`). It carries the magic `"LD24"`, seq, uptime, distance, presence, flags, motion_db[16], and micro_db[16].

### 5.3 `radar_display_ssd1306.c`: the screen (raw I2C SSD1306 driver)

On ESP32-S3, this lab **does not use** Zephyr's in-tree `solomon,ssd1306` + CFB. The in-tree driver sends the control byte and the pixel payload as separate I2C messages, which reproduces the already-confirmed ESP32 I2C driver issue and produces screen noise. So this lab reuses `ssd1306_display.c` (control byte and framebuffer sent together in a single `i2c_write()`), verified on real hardware in multi-sensor AMP Lab 01. The following were added for this lab:

- `ssd1306_set_pixel()`, `ssd1306_draw_hline()`, `ssd1306_draw_vline()`: for the bar graph and direction arrow
- 5 new glyphs in `font5x7.h`: `+`, `/`, `c`, `m`, `s`

Screen layout (5x7 font, one page = 8 px):

```
┌────────────────────────────┐
│LINK: OK                    │  page0  (always at the top)
│                            │
│MOVE   123cm           ◀──  │  page2  status + distance, direction arrow at x112-127
│APPROACH -96cm/s            │  page3  direction + speed (blank until a verdict is reached)
│    ▔▔                      │  y=32   target gate marker
│▁▁ ▃▃ ▇▇ ▅▅ ▂▂ ...          │  y=34-63  16-gate motion dB
└────────────────────────────┘
```

The SR110 version implements the same `radar_display.h` API using CFB (a 10x16 font).

### 5.4 `radar_trend.c`: approach / leave detection (sensor core)

The LD2402 is 1T1R, so it reports neither angle nor velocity. Approach and leaving are instead estimated from **the distance's change over time (radial speed)**.

1. A **least-squares slope** is computed over the last 8 samples (about 1.3 s) of (time, distance), in cm/s; negative means approaching.
2. Hysteresis is applied to classify the result.

| Current state | Enter condition | Hold condition |
|---|---|---|
| APPROACH | speed ≤ -20 cm/s | speed < -10 cm/s |
| LEAVE | speed ≥ +20 cm/s | speed > +10 cm/s |
| STEADY | otherwise | |

3. A new direction must appear for **3 consecutive samples** before the display changes — debouncing against the arrow flickering from ±15 cm of distance noise.
4. The window is reset back to `UNKNOWN` when: the target disappears, frames stop arriving for 600 ms or more, or the distance jumps by more than 100 cm in one step (treated as the target having been swapped for a different person).
5. No verdict is made until at least 5 samples (about 0.8 s) have accumulated.

Since a direction change is itself an immediate-send condition, the on-screen reaction lag is roughly 0.5-1.3 s.

**Host simulation results** (165 ms period, ±15 cm uniform noise added to distance, run with 4 seeds)

| Scenario | Result |
|---|---|
| Walking approach, -100 cm/s | APPROACH, estimated -103 cm/s |
| Walking away, +80 cm/s | LEAVE, estimated +79 cm/s |
| Slow approach, -30 cm/s | APPROACH, estimated -20 to -48 cm/s |
| Sitting still for 20 s | stayed STEADY, 0 false triggers |
| Distance jump / target disappearance | reset to UNKNOWN |

There are limits. Lateral movement that doesn't change the distance (e.g. moving left/right) reads as STEADY. Also, whenever the radar's reported target switches between multiple people present, the window resets.

Arrow direction matches the bar graph: since gate 0 (closest to the sensor) is on the left, approaching shows `◀──`, leaving shows `──▶`, and stationary shows a short `─`.

### 5.5 Radar tuning: parameter dump, auto-threshold, median filter

Besides the board's own auto-threshold via the Boot button (5.5.2), the same configuration can be tuned from a PC with Hi-Link's official "HLK-LD2402 Tool" — see the [troubleshooting doc](./doc/05_HLKLD2402_Radar_TROUBLESHOOTING_en.md#tuning-with-the-pc-side-hlk-ld2402-tool-no-board-code-changes-needed) for the download link and usage.

### 5.5.1 Radar parameters logged at boot

During the startup config session (enable → version → **read parameters** → engineering mode → end), the current radar settings are printed to the console. Thresholds are converted from their raw linear values to dB, so they can be compared directly against the on-screen bar graph (gate energy in dB).

```
[SENSOR] LD2402 firmware v3.5.5
[PARAM] max distance      : 5.0 m (raw 50)
[PARAM] disappear delay   : 30 s
[PARAM] power interference: none
[PARAM] gate     :  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
[PARAM] motion dB: 50 53 54 56 57 57 58 59 59 60 60 60 61 61 61 62
[PARAM] micro dB : 30 33 34 36 37 37 38 39 39 40 40 40 41 41 41 42
[PARAM] gate n = 70*n .. 70*(n+1) cm; threshold dB = 10*log10(raw)
[SENSOR] engineering mode ON
```
(values shown are illustrative)

**How to read it:** in an empty room, if a given gate's bar-graph energy exceeds that gate's motion threshold, that gate's distance range will be reported as occupied. For example, if gate 2 (140-210 cm) exceeds threshold with nobody present, the reported distance will **always sit near 150 cm**.

### 5.5.2 Auto-threshold generation (hold the Boot button for 2 seconds)

| Step | Sensor core | Screen |
|---|---|---|
| 1 | Detects the Boot button (GPIO0) held ≥2 s (polled in the main loop) | |
| 2 | 10-second countdown: you must **leave** the detection area during this time | `AUTO THRESHOLD` / `LEAVE THE AREA` / `START IN 10s` |
| 3 | Enters config mode → sends `0x0009` (trigger/hold/micro coefficient = 3.0) | |
| 4 | Polls progress every 1 s with `0x000A` (120 s timeout) | `CALIBRATING 40%` + progress bar |
| 5 | Saves to the radar's flash with `0x00FD`; if there's no response, falls back to the 0x003F rewrite method | |
| 6 | Re-reads and logs the new parameters → re-applies engineering mode → exits config | `CALIB DONE` / `SAVED TO RADAR` (3 s) |

- The result is saved to the **radar module's own flash**. It survives re-flashing or power-cycling the ESP32.
- The 3.0 coefficient is HLK's official tool default (range 1.0-20.0). A larger coefficient sets the threshold higher relative to the background, reducing false positives at the cost of sensitivity. Adjust it via `CALIB_COEFF_X10` in `sensor_main.c`.
- The radar is in config mode during calibration, so distance reporting pauses; status messages keep being sent in the meantime, so the screen does not fall back to `LINK: FAIL`.
- The button feature only compiles in when the devicetree alias `calib-button` is present.

### 5.5.3 Distance median filter

`radar_filter.c` takes the median of the last 5 samples (about 0.8 s). Unlike an average, a single outlier frame (e.g. one caught by a wall reflection) is discarded rather than blended in. The added latency is about 2 frames (330 ms).

- The filtered distance is what's used for the display, the approach/leave verdict, and the IPC message. The console still prints the raw value alongside it for comparison, e.g. `dist=152cm (raw 420)`.
- The filter resets whenever the target disappears or the sensor link drops.

**Host test**: input `150 152 420 149 151 153 0 150` → output `150 152 152 152 151 152 151 150`. The 420 and 0 spikes are removed.

## 6. Build and Run

```powershell
cd D:\work\ESP32-S3\zephyrproject
west build -p always -b esp32s3_devkitc/esp32s3/procpu --sysbuild <path>\05_HLKLD2402_Radar\lab
west flash
west espressif monitor
```

Thanks to `SB_CONFIG_BOOTLOADER_NONE=y` in `sysbuild.conf`, both images flash together without MCUboot. `sysbuild.cmake` follows the same form as the other multi-sensor AMP labs (naming the remote board target explicitly).

### Expected console output (PRO_CPU)

```
=== LD2402 AMP: sensor core (esp32s3_devkitc/esp32s3/procpu) ===
[SENSOR] LD2402 firmware v3.5.5, engineering mode ON
[SENSOR] NONE  dist=  0cm -          +0cm/s peak=24dB@g0  | sent=3 bin=6 txt=0 bad=0 ovf=0
[SENSOR] MOVE  dist=142cm APPROACH  -96cm/s peak=58dB@g2  | sent=9 bin=12 txt=0 bad=0 ovf=0
[SENSOR] STILL dist=138cm STEADY     +3cm/s peak=41dB@g1  | sent=11 bin=18 txt=0 bad=0 ovf=0
```

## 7. Real-Hardware Verification Checklist

| # | Check | Expected result |
|---|---|---|
| 1 | Screen right after boot | `LINK: WAIT` → `LINK: OK` within 1 s |
| 2 | Console firmware log | `engineering mode ON` (falls back to ASCII mode after 5 retries on failure) |
| 3 | `[PARAM]` log | prints max distance, disappear delay, and all 16-gate thresholds |
| 3-1 | `bin` counter | increments at roughly 6/s; `bad`/`ovf` stay at 0 |
| 4 | Walk in front of the sensor | `MOVE` + changing distance, the corresponding gate's bar rises |
| 5 | Sit still | becomes `STILL` after a few seconds |
| 6 | Walk toward / away from the sensor | `◀──` / `──▶` within about 1 s, console speed sign matches |
| 7 | Walk, then stop | `─` (STEADY) after about 1 s |
| 8 | Leave the area | `NONE` after the disappear-delay parameter (0x0004), arrow disappears |
| 9 | Hold Boot button 2 s, then leave the area within 10 s | `CALIBRATING n%` → `CALIB DONE`, new thresholds logged, persists after reboot |
| 10 | Disconnect the LD2402 TX line | `NO SENSOR` after 2 s |
| 11 | Halt PRO_CPU (debugger) | `LINK: FAIL` after 3 s |

## 8. Troubleshooting

Symptoms and checks you may run into verifying this lab on real hardware are collected in a separate document: [`doc/05_HLKLD2402_Radar_TROUBLESHOOTING_en.md`](./doc/05_HLKLD2402_Radar_TROUBLESHOOTING_en.md). It also covers tuning thresholds directly from a PC with the "HLK-LD2402 Tool".
