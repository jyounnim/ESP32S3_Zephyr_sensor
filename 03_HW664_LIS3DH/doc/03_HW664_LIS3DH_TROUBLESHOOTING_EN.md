[한국어](./03_HW664_LIS3DH_TROUBLESHOOTING_kr.md) | **English** · [← Lab README](../README.md)

# Lab 03 Troubleshooting (HW-664, dual-core/IPM)

Only the issues actually hit during real-hardware bring-up, with their
resolutions, are documented here.

## 1. The chip's real identity didn't match the listing (LIS3DSH) - it's actually LIS3DH

**Symptom**: assuming LIS3DSH and writing a custom raw-I2C driver for
it, the boot log showed:
```
<wrn> lis3dsh: Device at 0x19 answered but WHO_AM_I=0x33 (expected 0x3F)
<err> lis3dsh: LIS3DSH not found at 0x18 or 0x19 - check wiring
```

**Cause**: `0x33` is not LIS3DSH's fixed value (`0x3F`) - it's the
WHO_AM_I value for the **LIS3DH family** (LIS3DH/LIS2DH/LIS2DH12/
LSM303DLHC/LSM303AGR, all register-compatible). The HW-664 module's
silkscreen and listing didn't match the part actually soldered on it -
not uncommon with cheap breakout modules.

**Implication**: the earlier custom raw-I2C driver had been written
under the premise that "Zephyr has no in-tree LIS3DSH support"
(confirmed via Zephyr GitHub discussion #73810) - but since the real
chip turned out to be LIS3DH, that premise no longer applied. Zephyr
officially supports LIS3DH, so the custom driver was removed entirely in
favor of the standard sensor API (once the devicetree compatible issue
in #2 below was also sorted out).

**Lesson**: don't fully trust the part number printed on a listing or
silkscreen - if the chip has a self-identifying register like WHO_AM_I,
check it on real hardware first. In this case, that check led to a
simpler, more standard implementation than originally expected.
(Side note: the [SparkFun_LIS3DH_Arduino_Library](https://github.com/sparkfun/SparkFun_LIS3DH_Arduino_Library)
reviewed earlier had already confirmed "LIS3DH's WHO_AM_I is 0x33,"
which matched this real-hardware result exactly and helped pin down the
chip's identity quickly.)

## 2. Using devicetree compatible `st,lis3dh` caused a link error

**Symptom**: after confirming the chip was LIS3DH and switching to the
standard API, using `compatible = "st,lis3dh"` compiled fine but failed
at link time with:
```
undefined reference to `__device_dts_ord_102'
undefined reference to `__device_dts_ord_103'
```
Those two ordinal numbers matched exactly the two LIS3DH devicetree
nodes declared for this lab - meaning the nodes existed, but no actual
device driver instance was compiled in for them.

**Cause**: confirmed via Zephyr GitHub issue #31253 ("lis3dh driver
support is confusing") - Zephyr documents a separate `"st,lis3dh"`
devicetree binding (a YAML file), which makes it look like a dedicated
driver exists for it, but the actual `lis2dh.c` driver that instantiates
devices only responds to the `"st,lis2dh"` compatible
(`#define DT_DRV_COMPAT st_lis2dh`). Declaring a node with only
`"st,lis3dh"` produces no device object at all, even with
`CONFIG_LIS2DH=y` (`#warning "LIS2DH driver enabled without any
devices"`).

**Fix**: changed both nodes' `compatible = "st,lis3dh"` to
`compatible = "st,lis2dh"`. The chip itself genuinely is LIS3DH
(confirmed via WHO_AM_I=0x33) - it's just that Zephyr's driver requires
this particular compatible string regardless.

**Lesson**: the existence of an official binding page for a given
compatible string doesn't guarantee that string actually instantiates a
device. When one driver covers a family of register-compatible chips,
the documented alias compatible strings and the one the driver's
`DT_DRV_COMPAT` actually responds to can differ. A link-time
`undefined reference to __device_dts_ord_N` means "the devicetree node
exists but no device instance was generated for it" - suspect the
compatible string first.

## ✅ Final confirmation on real hardware

```
[00:00:00.229,000] <err> lis2dh: Failed to read chip id.
[00:00:00.235,000] <inf> lis2dh: fs=2, odr=0x4 lp_en=0x0 scale=9576
*** Booting Zephyr OS build v4.4.0 ***
[00:00:00.237,000] <inf> app_procpu: 03_HW664_LIS3DH (core0/procpu) starting
[00:00:00.242,000] <inf> app_procpu: LIS3DH found at 0x19 (SDO/SA0 high/unconnected)
```
The first line's `Failed to read chip id.` comes from the unwired 0x18
node's init failing, the same expected pattern as Lab 02's MPU6050
0x68/0x69 auto-detect - the next line confirms 0x19 initialized
successfully with `fs=2` (+-2g, as expected). The core1 screen was also
confirmed rendering correctly.

## Reference: register map cross-check against the SparkFun LIS3DH library

While the chip was still misidentified as LIS3DSH, the
[SparkFun_LIS3DH_Arduino_Library](https://github.com/sparkfun/SparkFun_LIS3DH_Arduino_Library)
was reviewed as a reference - which turned out to be an
exact match for the real chip, not just a loosely related reference:

| Item | LIS3DH (SparkFun library) | For reference: LIS3DSH (the chip originally assumed) |
| --- | --- | --- |
| WHO_AM_I value | `0x33` | `0x3F` |
| ODR + axis-enable register | `CTRL_REG1` (0x20) | `CTRL_REG4` (0x20) |
| Full-scale (FS) register | `CTRL_REG4` (0x23) | `CTRL_REG5` (0x24) |
| Output registers (OUT_X_L..OUT_Z_H) | 0x28-0x2D | 0x28-0x2D (same) |
