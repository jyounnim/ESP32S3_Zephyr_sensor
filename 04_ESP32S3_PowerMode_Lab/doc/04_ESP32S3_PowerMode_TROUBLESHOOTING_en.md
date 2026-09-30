[한국어](./04_ESP32S3_PowerMode_TROUBLESHOOTING_kr.md) | **English** · [← Lab README](../README.md)

# Lab 04 Troubleshooting (ESP32-S3 Power Mode) - CONFIG_PM Trial-and-Error Log

This document records the build errors and runtime crashes encountered while validating [Lab 04](../README.md) on real hardware (an ESP32-S3 DevKitC-1), the root-cause analysis behind each one, and the full path that led to stabilizing on `CONFIG_PM=n` (a software-only policy) as the final approach.

The main lab document ([README.md](../README.md)) only describes the final, stabilized structure. If you're curious why this lab doesn't use Zephyr's `CONFIG_PM`-based real Light-Sleep, this document explains why. It's also meant as a debugging reference for anyone who runs into the same issues.

> Environment: Zephyr `v4.4.0-13070-g6d1d551f6080`, board targets `esp32s3_devkitc/esp32s3/procpu` / `.../appcpu`, built with `west build --sysbuild`

---

## 1. Build Error: Declaring a GPIO Node Without a `gpio-leds` Binding

### Symptom

Declaring the OLED power-gating GPIO (`oled-pwr`) in the overlay like this:

```dts
/ {
    oled_pwr_ctrl {
        oled_power_pin: oled_power_pin {
            gpios = <&gpio0 6 GPIO_ACTIVE_HIGH>;
        };
    };
};
```

compiles the DTS fine, but the moment `GPIO_DT_SPEC_GET(DT_ALIAS(oled_pwr), gpios)` is used in C code, the build fails with a link/compile error like:

```
error: '__device_dts_ord_DT_N_S_oled_pwr_ctrl_P_gpios_IDX_0_PH_ORD' undeclared
```

### Root Cause

Zephyr's devicetree code generator only produces the property accessor macros that things like `GPIO_DT_SPEC_GET()` need (`DT_GPIO_CTLR`, `DT_GPIO_PIN`, etc.) for nodes that have a matching **binding** (a YAML schema tied to a `compatible` property). A node with a bare `gpios` property and no `compatible` is valid DTS, but since it has no matching binding, none of those accessor macros get generated, and the build breaks at the point above.

### Fix

Borrow Zephyr's in-tree `gpio-leds` binding. It's built specifically to support exactly this shape — a parent node with `compatible = "gpio-leds";` and child nodes that each carry a `gpios` property, with no `compatible` needed on the children — and works just as well for a plain on/off control pin (like an OLED power gate) as it does for an actual LED.

```dts
/ {
    oled_pwr_ctrl {
        compatible = "gpio-leds";

        oled_power_pin: oled_power_pin {
            gpios = <&gpio0 6 GPIO_ACTIVE_HIGH>;
        };
    };

    aliases {
        oled-pwr = &oled_power_pin;
    };
};
```

After this change, `GPIO_DT_SPEC_GET(DT_ALIAS(oled_pwr), gpios)` compiles cleanly.

---

## 2. Runtime Crash: `CONFIG_PM_DEVICE=y` Conflicting with the LIS2DH Sensor Driver

### Symptom

With `CONFIG_PM=y` and `CONFIG_PM_DEVICE=y` both enabled, the board boots but crashes shortly after, with logs like this repeating first:

```
[00:00:0x.xxx,000] <err> lis2dh: failed to read reg_ctrl1
[00:00:0x.xxx,000] <err> pm_device: Device lis3dh@19 did not enter suspended state (-116)
[00:00:0x.xxx,000] <err> app_procpu: Could not read accel axis data
...
***** BOOTING ZEPHYR OS (FATAL EXCEPTION) *****
E (xxxxx) esp_core_dump_flash: ...
Exception cause: 29 (StoreProhibitedCause)
```

`EXCCAUSE 29` (store prohibited, an invalid write address) is typically a NULL-pointer dereference or a write into already-freed/corrupted memory.

### Root Cause

With `CONFIG_PM_DEVICE=y` enabled, Zephyr calls suspend/resume hooks on every PM_DEVICE-capable driver around each Light-Sleep entry/exit. On this Zephyr snapshot, the LIS2DH driver does not appear to fully recover the I2C0 bus state through this suspend/resume cycle — subsequent register reads (`reg_ctrl1`) start failing, related internal driver state gets corrupted, and it eventually crashes with a NULL-pointer access.

This combination (the LIS2DH driver + `CONFIG_PM_DEVICE` + the ESP32-S3 SoC PM driver) reproduced the crash reliably on real hardware.

### Attempted Fix 1

Disabled `CONFIG_PM_DEVICE=y` while keeping `CONFIG_PM=y`, and restored `CONFIG_COUNTER=y` / `CONFIG_COUNTER_RTC_ESP32=y`, which had accidentally been removed along the way. → The crash disappeared, but the warning described in section 3 below started appearing instead.

---

## 3. Warning: `Sleep skipped. Make sure RTC counter driver is enabled.`

### Symptom

No more crash, but this warning is printed every time the system attempts to enter Light-Sleep:

```
[00:00:00.367,000] <wrn> soc_pm: Sleep skipped. Make sure RTC counter driver is enabled.
```

This happened even though `CONFIG_COUNTER=y` / `CONFIG_COUNTER_RTC_ESP32=y` were both already enabled.

### Root Cause

The ESP32-S3's base SoC devicetree (dtsi) ships the `rtc_timer` node (`compatible = "espressif,esp32-rtc-timer"`) with **`status = "disabled"`** by default. Enabling the Kconfig symbol (`CONFIG_COUNTER_RTC_ESP32`) only gets the driver code compiled in — it does not, by itself, enable the disabled devicetree node, so no actual device instance is ever created.

The binding documentation's wording — that the counter "doesn't need to be explicitly enabled... it runs from power-up" — describes the **physical RTC counter hardware**, not the devicetree node's `status` property. This wording made it easy to miss the fact that the node still needs to be explicitly enabled in the devicetree.

### Fix

Add this to the board overlay to explicitly enable the node:

```dts
&rtc_timer {
    status = "okay";
};
```

After this, the warning above disappeared — and was replaced by a different one (section 4 below).

---

## 4. Warning: `Make sure '&rtc_timer' is enabled as a wakeup source.` (and a Crash Recurrence)

### Symptom

After fixing item 3, the warning text changed to:

```
[00:00:07.286,000] <wrn> soc_pm: Sleep skipped. Make sure '&rtc_timer' is enabled as a wakeup source.
```

### Root Cause

Zephyr's device wakeup-source mechanism is a two-step process:

1. Adding `wakeup-source;` to a node in the devicetree only declares that the device **is capable of** being a wakeup source.
2. Actually **activating** it as a wakeup source requires a runtime call to `pm_device_wakeup_enable(dev, true)`, which is part of the Device PM framework and therefore requires `CONFIG_PM_DEVICE=y` — the very option that had just been disabled in section 2 to fix the crash.

In other words, there was a genuine structural conflict: using the RTC timer as a wakeup source required re-enabling `CONFIG_PM_DEVICE`, which was the thing identified as the crash's root cause.

### Attempted Fix 2 (Trying the Full Combination)

At this point there were two options:

- (A) Disable `CONFIG_PM=n` entirely and document Ultra-Sleep as a software-only policy
- (B) Go with the "proper" fix and add all three pieces together: `CONFIG_PM_DEVICE=y`, `wakeup-source;`, and `pm_device_wakeup_enable()`

Option (B) was tried first: `CONFIG_PM_DEVICE=y` was re-enabled, `wakeup-source;` was added to `&rtc_timer`, and `main()` was updated to call `pm_device_wakeup_enable(rtc_timer_dev, true)`.

### Result: Crash Recurrence

Flashing this combination reproduced the exact same crash from section 2 (LIS2DH register read failure → `pm_device` suspend failure → `EXCCAUSE 29`). This confirmed that the crash's root cause was not related to whether `wakeup-source` was configured — **`CONFIG_PM_DEVICE=y` being enabled at all is what conflicts with the LIS2DH driver.**

---

## 5. Final Decision: `CONFIG_PM=n` + a Software-Only Policy

Since the crash cannot be avoided as long as `CONFIG_PM_DEVICE` stays enabled, and there is no way to activate the RTC timer as a wakeup source without `CONFIG_PM_DEVICE`, this is a genuine structural conflict — actually entering and waking from real Light-Sleep in a meaningful way was not achievable with this combination. The final approach was:

- Set `CONFIG_PM=n`, disabling Zephyr System Power Management entirely (removing `CONFIG_PM_DEVICE`, `CONFIG_PM_DEVICE_RUNTIME`, `CONFIG_POWEROFF`, and the RTC counter Kconfig options along with it).
- Remove the `&rtc_timer { status = "okay"; wakeup-source; };` block from the overlay.
- Remove the `pm_device_wakeup_enable()` call and the `<zephyr/pm/device.h>` include from `main.c`.
- **Redefine Ultra-Sleep as a purely software-level policy**, rather than a real SoC Light-Sleep entry: it samples slowly (once a second) and, on detecting motion, returns to Normal via `sys_reboot(SYS_REBOOT_WARM)`. This still lowers average consumption (most of the time is spent in `k_msleep()` between short bursts of work), but it does not put the ESP32-S3 chip itself into an actual Light-Sleep state.

Re-validating with this combination, the `lis2dh` errors, `pm_device` errors, `Sleep skipped` warnings, and the crash all disappeared, and the full 10-step scenario ran stably.

### A Note on Why Deep-Sleep Was Never Attempted

Using ESP-IDF's native Deep-Sleep API from Zephyr (`sys_poweroff()` + `esp_sleep_enable_timer_wakeup()`) was also considered, but this combination has a reported upstream Zephyr issue that causes watchdog-reset loops or boot failures, affecting both ESP32 and ESP32-S3 (zephyr-rtos/zephyr#86193, closed as "not planned"). For that reason, the Deep-Sleep path was never attempted in this lab in the first place.

---

## Summary Table

| Step | Configuration | Result |
|---|---|---|
| Initial WIP | No `compatible` on the `oled-pwr` GPIO node | Build error (`__device_dts_ord_...` undeclared) |
| Fix 1 | Applied the `gpio-leds` binding | Build succeeds |
| Fix 2 | `CONFIG_PM=y` + `CONFIG_PM_DEVICE=y` | Runtime crash (`EXCCAUSE 29`) |
| Fix 3 | Removed `CONFIG_PM_DEVICE`, kept `CONFIG_PM=y` only | No crash, but `Sleep skipped` warning (RTC counter) |
| Fix 4 | Added `&rtc_timer { status = "okay"; }` | Warning text changes (now requires a wakeup source) |
| Fix 5 | Re-added `CONFIG_PM_DEVICE=y` + `wakeup-source;` + `pm_device_wakeup_enable()` | Crash recurs |
| **Final** | **`CONFIG_PM=n`, Ultra-Sleep as a software-only policy** | **Confirmed stable** |
