[한국어](./05_HLKLD2402_Radar_TROUBLESHOOTING_kr.md) | **English**

# [Troubleshooting] Lab 05: HLK-LD2402 Radar — Symptom-Based Checklist

This document collects the symptoms and checks you may run into while verifying Lab 05 (`../README.md`) on real hardware (ESP32-S3-DevKitC-1 + HLK-LD2402 + SSD1306). The main lab document only describes the final working setup, so when you hit a wiring or communication problem, check here first.

> For background on the sensor protocol itself (UART framing, commands/ACKs, parameter IDs), see [`HLK-LD2402_sensor_research_kr.md`](../../HLK-LD2402_sensor_research_kr.md) at the repo root (Korean) and Hi-Link's official user manual (`HLK-LD2402_User_Manual.pdf`, also at the repo root).

## Symptom-based checks

| Symptom | Check |
|---|---|
| `engineering mode attempt` keeps failing, only `txt` increments | Check the crossed TX/RX wiring (`R` ← GPIO17). This is the classic symptom of "receiving fine, but not transmitting" |
| `bin` and `txt` both stay at 0 | Check the GPIO18 ↔ LD2402 `T` wiring, power, and the 115200 bps setting |
| `bad` increments | Check for power noise or a shared-ground issue. If `ovf` also increments, increase `RX_RING_SIZE` |
| Screen stays completely blank | Check the I2C1 wiring (GPIO4/5) and SSD1306 power. Init fails if the raw driver gets no response from either 0x3C or 0x3D |
| `CMake configure failed for Zephyr project: remote` | The actual error (DTS or Kconfig) for the remote image is printed **above** this line in the sysbuild output — looking only at the tail won't show the cause |
| Distance always reads similarly (e.g. ~150 cm) whether or not anyone is present | That gate's background reflection is exceeding its threshold. If the sensor is lying flat, likely culprits are the ceiling, a monitor, or a wall → compare the boot-time thresholds against the empty-room bar graph → mount the sensor upright facing the people-side → hold Boot for 2 s to run auto-threshold, or see "Tuning with the PC-side HLK-LD2402 Tool" below |
| Reads 0 cm when a hand is within 10 cm | Normal behavior (gate 0 = 0-70 cm, strong near-field reflection) |
| The arrow flickers or shows the wrong direction | Check the console speed values. If noise is high, raise `RADAR_TREND_ENTER_CMS`/`CONFIRM`; if the reaction is too slow, shrink `RADAR_TREND_WINDOW` |
| Always `LINK: FAIL` | Check whether `ipm0` is enabled (procpu overlay) and whether the APP_CPU image was actually built alongside it via sysbuild |

## Tuning with the PC-side HLK-LD2402 Tool (no board code changes needed)

Symptoms like "always reads a similar distance" above, where the threshold itself needs adjusting, can be fixed with this lab's own code (hold Boot for 2 s → auto-threshold), but Hi-Link's own **PC configuration tool ("HLK-LD2402 Tool")** makes it more convenient.

- Download: <https://h.hlktech.com/Mobile/download/fdetail/318.html>
- Connect the LD2402 directly to a PC via a USB-UART adapter (bypassing the ESP32-S3 entirely — wire straight to the sensor's UART T/R pins).
- Lets you watch the live 16-gate energy graph and adjust each gate's motion/micro threshold visually. The adjusted values are saved to the radar module's own flash, so they carry over the next time it's connected to the ESP32-S3.
- This lab's own auto-threshold routine (`0x0009`/`0x000A`/`0x00FD`, see section 5.5.2 of the main doc) sets **the same underlying hardware configuration through a different path**, so use whichever is more convenient — both save to the radar's own flash, so they don't conflict.
- While using the PC tool, the ESP32-S3's UART1 must not be holding the LD2402 at the same time (either power down the ESP32-S3, or move the LD2402's T/R wires over to the PC adapter).
