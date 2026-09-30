[한국어](./01_AHT20_BMP280_MultiSensor_TROUBLESHOOTING_kr.md) | **English** · [← Lab README](../README.md)

# Lab 01 Troubleshooting (AHT20 + BMP280 MultiSensor, dual-core/IPM)

Only the issues actually hit during real-hardware bring-up, with their
resolutions, are documented here.

## 1. sysbuild tried to build MCUboot and failed

**Symptom**: `west build --sysbuild ...` failed at configure time with
`CMake configure failed for Zephyr project: mcuboot` (reproduced even after
fully deleting the build directory).

**Cause**: Espressif boards default to building MCUboot alongside the app
whenever `--sysbuild` is used (per Zephyr's own docs: "By default, the
ESP32 sysbuild creates bootloader (MCUboot) and application images").
`SB_CONFIG_BOOTLOADER` is a Kconfig **choice** (single-select), so turning
off the unwanted member with `SB_CONFIG_BOOTLOADER_MCUBOOT=n` has no
effect - you have to explicitly select the alternative member with `=y`
instead.

**Fix**: add this to `lab/sysbuild.conf`:
```
SB_CONFIG_BOOTLOADER_NONE=y
```

## 2. The IPM Kconfig symbol name didn't match what the docs implied

**Symptom**: `CONFIG_IPM_ESP32=y` in `prj.conf` produced `undefined symbol
IPM_ESP32` and aborted the Kconfig step.

**Cause**: checked `drivers/ipm/Kconfig` upstream directly - the real
symbol for the ESP32 IPM driver is `CONFIG_ESP32_SOFT_IPM`. `IPM_ESP32`
simply doesn't exist as a symbol.

**Fix**: use `CONFIG_ESP32_SOFT_IPM=y` in `prj.conf`. It actually defaults
to `y` automatically once the `&ipm0` devicetree node is enabled, but it's
kept explicit here for clarity.

## 3. BMP280 I2C address mismatch (0x76 vs 0x77)

**Symptom**: boot log showed `BMP280 device not ready`, and every
subsequent `sensor_sample_fetch` failed with `-14` (EFAULT). Driver debug
logs showed only `bme280_chip_init: ID read failed` - no "bad chip id"
message, meaning nothing responded at all.

**Cause**: the overlay defaulted to 0x76 (assuming SDO tied to GND), but
this particular module has SDO tied to VDD, making 0x77 the correct
address - a common pattern on cheap AHT20+BMP280 combo boards.

**Fix**: scanned the actual bus with `lab_tools/i2c0_scanner` (1-byte
write-based probing) and confirmed `0x38` (AHT20) and `0x77` (BMP280)
directly, then updated the overlay's `bmp280@76`/`reg = <0x76>` to
`bmp280@77`/`reg = <0x77>`.

**Lesson**: after wiring up a new I2C device, scan for its real address
with the scanner before putting anything in the overlay - a module's
actual SDO wiring frequently doesn't match the datasheet's default
address.

## 4. AHT20 passed CRC but returned physically impossible readings

**Symptom**: temperature stuck around ~145C, humidity stuck at exactly
~50%, with no errors even after enabling CRC checking
(`CONFIG_DHT20_CRC=y`) - meaning the bytes weren't corrupted in transit;
the sensor was genuinely sending that value.

**Diagnosis**: ~50% humidity corresponds exactly to half of the ADC's
20-bit range (raw ~ 0x80000, only the MSB set) - the classic signature of
an unconverted/reset register rather than a real measurement. On real
hardware, removing and reapplying the AHT20's own VCC cleared the problem;
an MCU reset alone did not - confirming this was a stuck state that only
a real power cycle of the sensor itself could clear.

**Fix**: since physically power-cycling the sensor isn't practical in the
field, added a self-heal routine that sends AHT20's documented soft-reset
command (`0xBA`, no payload, ~20ms to complete) directly over I2C,
bypassing the Zephyr sensor API - triggered automatically whenever a
reading falls outside AHT20's rated range (-40C to 85C). See
`aht20_soft_reset()` in `main.c`.

**Caveat**: whether this soft reset is a perfect substitute for a real
power cycle isn't 100% guaranteed in every case, even though the
datasheet describes it as such. BMP280 pressure has occasionally been
observed misbehaving at the same time (jumping to values in the 600s
hPa), which leaves open the possibility of a shared root cause (power
rail or wiring quality) rather than two independently defective chips -
see "Known limitations" in the design doc.

## 5. The letter 'K' was missing from the OLED font ("LINK" rendered as "LIN : O")

**Symptom**: drawing `LINK: OK` on screen actually showed up as `LIN : O`
(with a blank gap) - and that "O" was sometimes misread as the digit "0".

**Cause**: the small 5x7 font built for this lab (`font5x7.h`) never
included an uppercase 'K' (none of the original AHT20/BMP280 labels
needed one). `font5x7_lookup('K')` returning NULL makes that character
cell render as blank.

**Fix**: added 'K', plus lowercase 'h' and 'a' (needed to render the
pressure unit as `hPa`), to the font table.

**Lesson**: whenever a label string changes or grows, check that every
character it uses actually exists in the font table - a missing glyph
fails silently as a blank cell, with no compile or runtime error to point
at the cause.
