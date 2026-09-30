[한국어](./02_GY521_MPU6050_TROUBLESHOOTING_kr.md) | **English** · [← Lab README](../README.md)

# Lab 02 Troubleshooting (GY-521 MPU-6050, dual-core/IPM)

Only what was actually confirmed during real-hardware bring-up is
documented here.

## 1. I2C address 0x68/0x69 auto-detect - design and verification

**Background**: scanning with `i2c0_scanner` confirmed that the GY-521's
AD0 pin gives address `0x68` when tied to GND (or left floating), and
`0x69` when tied to VCC. Rather than hardcoding one address in the
overlay, the goal was to support both without any code changes.

**Implementation**: Zephyr binds device instances to devicetree at
compile time, so "scan at runtime and construct a device from the
result" isn't a standard pattern. Instead, the overlay declares both
`mpu6050_68` (0x68) and `mpu6050_69` (0x69) as separate nodes, and at
boot the firmware picks whichever one actually reports
`device_is_ready() == true`. Zephyr's mpu6050 driver reads the WHO_AM_I
register during `init()` and returns an explicit error if that fails
(the unwired address NACKs at the I2C level) - this design leans directly
on that behavior.

**Avoiding a race condition**: the device-selection logic
(`mpu6050_select()`) runs at the start of the `mpu6050_thread` itself,
not in `main()`. A thread registered with `K_THREAD_DEFINE` (0ms delay)
can start running before `main()` does, so if `main()` had picked the
device and the thread read that choice later, there would be a real race
- the thread could call `sensor_sample_fetch()` on a still-NULL pointer.
Moving the selection into the thread's own entry point removes that race
entirely.

**✅ Confirmed on real hardware**: the boot log showed this sequence:
```
[00:00:00.225,000] <err> MPU6050: Failed to read chip ID.
*** Booting Zephyr OS build v4.4.0 ***
[00:00:00.226,000] <inf> app_procpu: 02_GY521_MPU6050 (core0/procpu) starting
[00:00:00.231,000] <inf> app_procpu: MPU6050 found at 0x68 (AD0 low/floating)
```
The `Failed to read chip ID.` error comes from the unwired 0x69 node's
`init()` running automatically during kernel boot (before the
application's `main()`) and failing to read WHO_AM_I - confirmed to be
**expected, intentional behavior**. `mpu6050_select()` then correctly
picked 0x68 right after. No noticeable boot delay was introduced (0.225s
to 0.231s).

**Note**: if that error line itself is bothersome, the MPU6050 module's
own log level can be lowered to quiet it (no functional effect). The
exact Kconfig symbol name can vary by Zephyr version - search "MPU6050"
in `west build -t guiconfig` to find it.

**Alternative considered and rejected**: probing the addresses directly
with raw I2C writes/reads (like `i2c0_scanner` does) and branching on the
result was also possible, but that would mean giving up Zephyr's standard
sensor driver (`sensor_sample_fetch`/`sensor_channel_get`) and
implementing the MPU6050 register protocol by hand - considerably more
work. Declaring both devicetree nodes up front and using the driver's own
init failure as the presence signal achieves the same goal with much less
code while keeping the standard API.

## 2. Preventive measures carried over from Lab 01

Issues confirmed on real hardware in Lab 01 were applied to this lab from
the start, to avoid repeating them:
- **sysbuild + MCUboot**: `lab/sysbuild.conf` needs
  `SB_CONFIG_BOOTLOADER_NONE=y` (Espressif boards default to bundling
  MCUboot whenever `--sysbuild` is used)
- **IPM Kconfig symbol name**: use `CONFIG_ESP32_SOFT_IPM=y`, not
  `CONFIG_IPM_ESP32`
- **Watch for missing font glyphs**: check that every character used in
  an on-screen label actually exists in `font5x7.h` beforehand - this lab
  added 'X' and 'Z' up front for the AX/AY/AZ/GX/GY/GZ/TEMP labels, and
  the screen was confirmed rendering correctly on real hardware
- **Scan new I2C devices before wiring an address into the overlay**:
  not strictly required for this lab given the auto-detect feature, but
  kept as a habit

## 3. On-screen units (g, deg/s) were tried and dropped

Adding a 'g' suffix to the acceleration values was tried (including
adding a lowercase 'g' glyph to the font), but ultimately dropped - it
was judged to make the screen look more cluttered. The unused 'g' glyph
was removed again to keep the font minimal. This can be revisited any
time if wanted - see "Ideas for extension" in the design doc.
