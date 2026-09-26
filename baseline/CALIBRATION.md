# PPK2 calibration data

Each kit carries its own factory calibration in the EEPROM (U7, a 24CW160 at
I2C `0x50`). This file explains what the fields mean, where they live, how
they can be lost, and how to put them back. It applies to both firmwares in
this repository and to Nordic's stock firmware, which all read the same
layout.

**Calibration is per-unit.** The numbers describe one specific board's shunt
resistors and amplifier offsets. Loading another unit's values gives readings
that look plausible and are quietly wrong.

## Back it up first

Reflashing firmware does not touch the EEPROM, but a backup costs nothing:

```powershell
python tools\ppktool.py backup-cal COM21 -o my-kit.txt   # the text reply, any firmware
```

On our firmware the shell also dumps the raw 2 KiB image, which is the
lossless form: `ppk meta raw` on the shell port. Keep both.

## How the numbers are used

The device streams raw ADC counts; the *host* applies calibration. This is
the conversion the desktop app performs (`getAdcResult` in
`serialDevice.ts`), with `adcVal` being the 14-bit sample multiplied by 4 and
`adcMult = 1.8 / 163840`:

```js
resultWithoutGain = (adcVal - O[range]) * (adcMult / R[range]);

amps = UG[range] * (
         resultWithoutGain * (GS[range] * resultWithoutGain + GI[range])
         + (S[range] * (vdd / 1000) + I[range])
       );
```

Read outward from the middle:

| Field | Meaning |
| --- | --- |
| `R0..R4` | Shunt resistance in ohms for each measurement range. Converts the measured voltage into a current. |
| `O0..O4` | Zero-current offset, in ADC counts on the `adcVal` (×4) scale. Subtracted before anything else: the pedestal the instrumentation amplifier sits at with no current flowing. |
| `GI0..GI4` | Gain correction, linear term (typically 0.97–1.0). |
| `GS0..GS4` | Gain correction, quadratic term. Together: `x * (GS*x + GI)`, correcting the front end's slight non-linearity. |
| `S0..S4` | Offset that scales with the DUT supply voltage, in amps per volt. Leakage that grows with applied voltage. |
| `I0..I4` | Fixed offset current, in amps. |
| `UG0..UG4` | **User** gain, default 1.0. The one field meant to be changed; see *Trimming* below. |

Non-calibration fields in the same reply:

| Field | Meaning |
| --- | --- |
| `VDD` | Last source-mode output setpoint, mV. Mutable state. |
| `mode` | Last mode: 1 = ampere meter, 2 = source meter. Mutable state. |
| `IA` | Instrumentation-amplifier offset. Stock stores a value here that it reports verbatim, and reports its own *measured* offset on the data port; our firmware stores the MCP4451 wiper-3 code. The app ignores it either way. |
| `HW` | **Not stored.** Derived from the low 16 bits of the SoC's `DEVICEID[0]`, reported as a decimal integer. Being part of the chip's unique ID, it is worth leaving out of anything you publish. |
| `Calibrated` | Stock's stored flag byte. It can read `0` on a kit with perfectly good factory data, and the desktop app never reads it (`parseMeta` only consumes `r/gs/gi/o/s/i/ug`), so it is informational, not a verdict on the data. |

### Range-0 gain terms are masked

Stock reports `GS0 = 1e-19` and `GI0 = 1.0` regardless of what the EEPROM
holds (a kit can store something like `GS0 = −1138.7`): it disables the
correction polynomial for range 0, and sends ε rather than `0` because the
app substitutes its own default of 1.0 for anything that parses as zero. Our
firmware reproduces that in its reply and leaves the stored bytes alone.

### Ranges

Range 0 is the most sensitive (largest shunt, smallest currents); range 4 the
least. They follow the hardware ladder directly: R41 (1 kΩ) is always in
circuit, with 110 Ω, 11 Ω, 1 Ω and 0.051 Ω switched in parallel on top.

| Range | Nominal | One real kit |
| --- | --- | --- |
| 0 | 1 kΩ | 996.166 Ω |
| 1 | 1k ∥ 110 ≈ 99.1 Ω | 100.710 Ω |
| 2 | ∥ 11 ≈ 9.92 Ω | 10.118 Ω |
| 3 | ∥ 1 ≈ 0.907 Ω | 0.946 Ω |
| 4 | ∥ 0.051 ≈ 0.048 Ω | 0.0564 Ω |

Ranges are selected **by hardware comparators, not by firmware**: `SW1..SW4`
(P0.00, P0.01, P0.26, P0.27) are inputs. Firmware can only observe which
range the analog front end has chosen. This matters for recalibration.

## EEPROM map

Stock keeps a second copy of its image at `0x600`; the layout below was
pinned by diffing a kit's primary image against that copy, which differs in
exactly the fields stock reports differently (`VDD`, `IA`).

| Offset | Size | Field |
| --- | --- | --- |
| `0x000` | 5 × f32 | `R0..R4` |
| `0x014` | 5 × f32 | `GS0..GS4` |
| `0x028` | 5 × f32 | `O0..O4` |
| `0x03C` | 5 × f32 | `S0..S4` |
| `0x050` | 5 × f32 | `I0..I4` |
| `0x064` | 5 × f32 | `GI0..GI4` |
| `0x078` | u16 | `VDD` (mV) |
| `0x07A`–`0x07F` | 6 | unknown (`ff ff 98 ff ff ff` seen). Left alone. |
| `0x080` | 5 × f32 | `UG0..UG4` |
| `0x094`–`0x0FA` | — | erased |
| `0x0FB` | u8 | `mode` |
| `0x0FC` | u8 | `Calibrated` |
| `0x0FD`–`0x0FE` | 2 | `ff 00`, unknown |
| `0x0FF` | u16 | `IA` (unaligned) |
| `0x600`–`0x700` | 257 | stock's backup: same layout |
| `0x701` | u16 | stock's backup CRC16, checked by its `metadata backup valid` |
| `0x200`–`0x29B` | 156 | **ours**: `"PPK2"` magic + our RAM struct + CRC32. Clear of both stock regions. |

Everything else in the 2 KiB device is `0xFF`.

Our firmware reads the stock layout and writes **only** `VDD`, `UG`, `mode`
and `IA`, each to its own offset, each only when it has changed. The factory
constants at `0x000`–`0x077` and the unknown bytes are never written.

## How it could get destroyed

Reflashing firmware does **not** touch calibration: the EEPROM is a separate
I2C chip, untouched by programming the nRF52840's flash. Only an explicit
EEPROM write can lose it. The realistic ways:

1. `metadata write` on the stock shell.
2. `ppk meta set r0 …` followed by `ppk meta save` on ours, except that
   `save` deliberately does not persist `r/gs/gi/o/s/i`, so this is not
   currently possible. Only `vdd`, `ug`, `mode`, `ia` are ever written.
3. A bug. Which is what the backup is for.

## Restoring

In order of preference.

### 1. On-device backup (stock firmware)

The EEPROM carries stock's own second copy. If `metadata backup valid` on
the stock shell says it is:

```
metadata backup recover
```

Then power-cycle; stock asks for a reboot before a recovered copy takes
effect. Expect the backup's `VDD` and `IA` to differ from what the primary
last held; the calibration constants are what matter.

### 2. From a raw image, byte for byte

Stock's whole primary image is the 257 bytes at `0x000`–`0x100`. Written
back verbatim (any tool that can do I2C EEPROM writes; our firmware does not
expose a raw write on purpose), stock is restored exactly, unknown bytes
included. `%.20f` text is lossless for float32 too, but the raw image is
what removes all doubt.

### 3. From the text, via the stock shell

```
metadata write "<string>"
```

Stock takes a single comma-separated string of the same `Key: value` pairs,
and guards it: it warns, requires `--i-know-what-i-am-doing`, and wants the
command repeated to actually commit. It refuses anything it considers
malformed, and prompts if the string contains NaNs. Confirm the exact token
set against `metadata write --help` on the device before relying on it.

## Recalibrating

**First: you probably should not.** These are factory values measured
against reference equipment better than anything on the board. If readings
look off, restore from backup before assuming the calibration is wrong.

### Trimming against a known reference (the supported path)

`UG0..UG4` exist precisely for this. Measure a known steady current,
compare, and set the per-range user gain to the ratio. The desktop app
exposes this under *Advanced → Gains*, and it sends `SetUserGains` (opcode
`0x25`, range byte + little-endian float32). It never touches the factory
constants, and `UG` reverts cleanly to 1.0.

### Full recalibration from scratch

Only relevant if the EEPROM is lost and there is no backup. The board has
four switchable calibration loads across `VDUT+`/`VDUT-`: 100 kΩ (R40),
10 kΩ (R58), 1 kΩ (R33), 100 Ω (R38) on P0.20–P0.23 (`ppk cal load` on our
shell), which, combined with a settable source voltage, give known currents:

| Load | At 0.8 V | At 5 V |
| --- | --- | --- |
| 100 kΩ | 8 µA | 50 µA |
| 10 kΩ | 80 µA | 500 µA |
| 1 kΩ | 0.8 mA | 5 mA |
| 100 Ω | 8 mA | 50 mA |

The sequence, per range:

1. **`O`**: output off, no load. The mean ADC reading *is* the offset.
2. **`R`**: apply a known load at a known voltage, so `I = V / R_load` is
   known; with `O` already subtracted, `R[range]` falls out of the measured
   counts.
3. **`GS`/`GI`**: repeat at several currents within one range and fit the
   quadratic.
4. **`S`/`I`**: sweep the source voltage with *no* load and fit the residual
   offset against voltage: slope is `S`, intercept is `I`.

Two real limitations:

- **You cannot force a range.** The comparators choose it. You reach a given
  range only by driving a current that lands in its band, which is what the
  load/voltage grid above is for.
- **The on-board loads do not span the extremes.** They cover roughly 8 µA
  to 50 mA, so range 0 (sub-µA) and range 4 (into the amps) need external
  precision loads. Stock contains internal `CMD_CALIBRATE_OFFSET` /
  `CMD_CALIBRATE_GAIN` / `CMD_SAVE_RESISTORS` handlers for its factory flow,
  but their opcodes are not part of the app's public protocol.

Set `IA` first regardless: it parks the zero-current pedestal, and step 1
depends on it. Our firmware's `ppk power ia-trim` targets `O0`, the raw code
range 0 reads at zero current, which is what the factory constants assume;
it also re-trims automatically (in RAM) whenever the DUT output is enabled
in source mode, so the stored `IA` is only a boot-time starting point. Stock
exposes `power ia_offset_target` / `power ia_offset_measured`, but that
figure is not the stream's zero-current reading: on a real kit it sat about
30 counts below what stock's stream reported, which is enough to move
range-0 readings by several percent.
