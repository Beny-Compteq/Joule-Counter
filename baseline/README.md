# PPK2 firmware

Open firmware for the Nordic Power Profiler Kit II (PCA63100 rev 1.0.1), built
on nRF Connect SDK / Zephyr. It speaks the desktop Power Profiler app's
protocol, streams 100 kHz current samples in the app's wire format, drives the
kit in both ampere-meter and source-meter mode, and reads the factory
calibration from the on-board EEPROM.

Target: **nRF52840-QIAA** (U12, aQFN73) — Cortex-M4F, `ppk2/nrf52840`.


## Building

```powershell
.\build.ps1              # incremental
.\build.ps1 -Pristine    # reconfigure from scratch
.\build.ps1 -Flash       # build, then program over SWD with J-Link
```

`build.ps1` exists because two things are not discoverable automatically: the
nRF Connect SDK toolchain environment, and `BOARD_ROOT`. Sysbuild (which `west
build` uses by default under NCS) does not inherit `BOARD_ROOT` from the
application's `CMakeLists.txt`, so it has to be passed on the command line.

The equivalent by hand:

```powershell
& <path-to>\nrfutil-sdk-manager.exe toolchain env --ncs-version v3.4.1 --as-script powershell |
    Out-String | Invoke-Expression
$env:ZEPHYR_BASE = "C:\ncs\v3.4.1\zephyr"
west build -b ppk2/nrf52840 -d build . -- -DBOARD_ROOT=$PWD
```

Toolchain: nRF Connect SDK **v3.4.1** (Zephyr 4.4.2), installed to `C:\ncs` via
the `nrfutil-sdk-manager.exe` bundled with the nRF Connect VS Code extension.

### Flashing

`zephyr.hex` starts at `0x1000`, above the factory nRF5 bootloader, so
programming it over SWD with `.\build.ps1 -Flash` leaves the bootloader intact.
Once this firmware runs, `ppk dfu` on the shell port (or the desktop app's
firmware update flow) reboots into that bootloader.

## What it does

### Host protocol (USB data port)

The kit enumerates as VID `0x1915` / PID `0xC00A`, product string `PPK2`, with
three USB functions in this order: Nordic's **DFU trigger** interface, the
**data port** (CDC ACM) and the **shell port** (CDC ACM). The order matters:
Nordic's Windows driver package binds the DFU-trigger driver to
`VID_1915&PID_C00A&MI_00`, so the trigger has to be interface 0, and the two
COM ports then land on `MI_01`/`MI_03` exactly as with stock firmware. The
USB serial number is the stock 12-digit form derived from `DEVICEADDR`, so a
kit keeps one identity across firmwares. The DFU trigger reports
`CONFIG_PPK2_DFU_SEMVER`, which the desktop app compares by exact string
match against the release it bundles to decide whether to offer an update.

Commands are one opcode byte plus fixed-length arguments. Those the app uses:

| Opcode | Command | Arguments | Effect |
| --- | --- | --- | --- |
| `0x06` | AverageStart | — | start the 100 kHz stream |
| `0x07` | AverageStop | — | stop the stream, flush the last partial block |
| `0x0C` | DeviceRunningSet | on (1 byte) | `VOUT_EN`: connect the DUT output |
| `0x0D` | RegulatorSet | mV (2 bytes, big-endian) | source-mode output voltage |
| `0x11` | SetPowerMode | 1 = ampere, 2 = source | select the supply path |
| `0x19` | GetMetadata | — | reply with the calibration text, ending in `END` |
| `0x20` | Reset | — | reboot |
| `0x25` | SetUserGains | range, float32 LE | store a per-range user gain |

PPK1-era opcodes (`0x01`–`0x05`, `0x08`–`0x0A`, `0x0E`–`0x0F`, `0x12`,
`0x15`–`0x16`) are consumed with their argument bytes and ignored.

Each streamed sample is one little-endian 32-bit word, exactly as the app's
`serialDevice.ts` decodes it:

| Bits | Content |
| --- | --- |
| 0–13 | ADC value: the 14-bit differential SAADC result `>> 2`, clamped at 0. The app multiplies by 4, so the EEPROM's `O` offsets are in raw 14-bit counts. |
| 14–16 | range 0–4 (1 kΩ … 0.05 Ω shunt), or 7 while the switches settle |
| 17 | VBUS present on the auxiliary USB connector J1 |
| 18–23 | six-bit counter, contiguous over emitted samples |
| 24–31 | logic port D0–D7 |

Samples go out in 512-sample (2 KiB) bulk transfers, several queued to the USB
controller at once, from a 4096-sample ring buffer.

### Measurement chain

- **SAADC** in 14-bit differential mode, gain 1/3, internal 0.6 V reference:
  `VSE_IA` (AIN7) against the analog ground reference `AGND` (AIN5), with
  acquisition-time code 7. That is the channel configuration the stock
  firmware uses and the factory calibration constants were derived with; code
  7 is not in the product specification (documented codes stop at 5 = 40 µs)
  but converts comfortably within the 10 µs period. `ppk sample tacq` changes
  it at runtime for experiments.
- **TIMER2** at 16 MHz with a 160-tick period fires `TASKS_SAMPLE` through PPI
  every 10 µs; `EVENTS_END` re-arms the one-sample EasyDMA buffer through a
  second PPI channel. The CPU only runs the END interrupt: it reads the result,
  both GPIO ports, decodes the range and packs the word.
- The **END handler is a zero-latency interrupt** (`CONFIG_ZERO_LATENCY_IRQS`,
  priority 0, above `irq_lock()`), so it makes no kernel calls. **TIMER3**
  counts END events over PPI and interrupts every 512 of them to wake the USB
  sender; that is where the block-ready semaphore comes from. **TIMER4**
  timestamps each END so the handler can measure its own latency (`ppk
  status` prints the histogram).
- **The CPU stays awake while streaming.** Waking this Cortex-M4 from `WFI`
  takes about 12.5 µs (11.5 µs in the POWER constant-latency sub-mode),
  longer than the 10 µs sample period, so an idle CPU would service only
  every other conversion. `CONFIG_PPK2_STREAM_KEEPS_CPU_AWAKE` makes the idle
  thread skip `WFI` while the stream runs (Zephyr's
  `z_arm_on_enter_cpu_idle()` hook); END-to-handler latency is then about
  1.2 µs and no conversions are missed under full USB load. The kit is USB
  powered and its own draw is not what it measures. `ppk sample selftest`
  runs the sampler without USB output and prints the END/ISR counts and the
  latency histogram.
- **Range** is read from `SW1..SW4` (P0.00, P0.01, P0.26, P0.27). The analog
  front end auto-ranges by itself; the comparators close the bypass switches
  cumulatively, so only the prefix patterns `0000, 0001, 0011, 0111, 1111` are
  valid ranges 0–4. Anything else is a transition, reported as range 7 or —
  by default — dropped without advancing the counter, so the app never sees it
  (`ppk sample discard 0` sends them instead).
- **SAADC offset calibration** runs once at boot.
- The **slow voltage monitors** (`VLDO`, `VBB`, `VIN`, `VDUT`, all through 5:1
  dividers; `VREF_IA` at gain 4) use 256× oversampled single conversions and
  are only available while the stream is stopped, because the SAADC belongs to
  the stream while it runs.

### Supply path

| Mode | Path | Lines |
| --- | --- | --- |
| Ampere meter (1) | `VIN` terminal → `VDUT+` | `VEXT_EN` on, `VLDO_EN`/`REG_EN` off |
| Source meter (2) | 5 V → ADP2504 buck-boost → ADP1708 LDO → `VDUT+` | `REG_EN` and `VLDO_EN` on, `VEXT_EN` off |

`VOUT_EN` (P1.04) connects `VDUT-` to the `VOUT` terminal in either mode; that
is the app's *Enable power output*. The kit remembers its mode and voltage in
the EEPROM but never powers the output on its own at boot.

Both regulators take their feedback through wipers of the MCP4451 (I2C
`0x2C`): wiper 0 for the buck-boost, wiper 1 for the LDO; a higher code means
a higher output (about +12 mV/step buck-boost, +16 mV/step LDO). The output
voltage is set closed-loop: the wiper is walked while the regulator output is
read back through the SAADC, re-estimating the mV-per-step slope from each
pair of readings, in bounded steps so a connected DUT sees a ramp. The 1 kΩ
calibration load is switched in while tuning so the output capacitors follow
downward steps promptly, and a final pass without it corrects for the LDO's
load regulation. The buck-boost is kept 0.5 V above the LDO (raised first,
lowered last), with a dropout guard. When the app changes the voltage
*during* streaming, the SAADC is busy, so the change is applied from the last
learned slope and refined the moment the stream stops. The `vldo`/`vdut`
monitors read through 5:1 dividers and are good to roughly ±20 mV absolute.

Wiper 3 sets the instrumentation amplifier's zero-current pedestal. The
factory `O0` constant is the raw code range 0 reads at zero current, so that
is the trim target. The pedestal drifts with temperature and with what the
terminals have been doing (a few counts per hour, ~3.6 counts per wiper
step) and enters every range at that range's gain, so it is re-trimmed
whenever current is known to be zero and the analog section is warm: just
before the DUT output is enabled in source mode, plus a first guess 1.5 s
after boot. Automatic trims stay in RAM; `ppk power ia-trim` followed by
`ppk meta save` is the only path that writes the EEPROM `IA` field, which is
otherwise just the boot-time starting point.

### Calibration metadata

Read from EEPROM offset 0 at boot, validated, and reported to the app in the
same text form the stock firmware uses (`Calibrated`, `R0..4`, `GS0..4`,
`GI0..4`, `O0..4`, `VDD`, `HW`, `mode`, `S0..4`, `I0..4`, `UG0..4`, `IA`,
`END`). The layout is the stock one: floats `R` at 0, `GS` at 20, `O` at 40,
`S` at 60, `I` at 80, `GI` at 100, `VDD` (u16) at 120, `UG` at 128, `mode`
at 251, `Calibrated` at 252 and `IA` (u16, unaligned) at 255, in a 257-byte
image; `HW` is not stored but derived from the low 16 bits of `DEVICEID[0]`.
Stock keeps its own CRC-checked backup copy at `0x600`. Range 0's `GS0`/`GI0`
are reported as `1e-19`/`1.0` regardless of what is stored, as stock does,
or the app applies a wild quadratic at range 0.

A kit whose EEPROM holds no plausible shunt values is reported as
uncalibrated with the app's own default constants. User gains, mode and
voltage are written back (debounced, field by field, only when changed) when
the app changes them; the factory constants and the unknown bytes are never
written. `ppk meta backup` keeps a CRC-protected copy of the fields this
firmware owns at offset `0x200`, clear of both stock regions. See
`CALIBRATION.md` for the meaning of each field and how to back them up.

### Shell (second serial port, 115200 8N1, waits for DTR)

```
ppk status                       state, voltages, stream metrics
ppk meta read|raw|set|save|defaults|backup
ppk power mode|vdd|out|ia|ia-trim
ppk cal load 100k|10k|1k|100|off   switch a known load across the terminals
ppk cal measure [n] [vse|vref|vldo|vbb|vin|vdut|ntc]
                                   averaged raw ADC of one channel, range, VDUT
ppk sample discard|reset
ppk sample tacq [0-7]              acquisition-time code for the next start
ppk sample selftest [s] [spin|idle] [period]
                                   run the sampler with no USB output and
                                   report END/ISR counts and latency
ppk dfu                            reboot into the bootloader
```

### LEDs

Blue breathing: no host. Blue steady: host connected. Green breathing:
streaming. Red is added whenever the DUT output is powered.

## Hardware facts the code depends on

From `../ref/netlist.NET` (the Altium netlist export) and the schematic PDF,
cross-checked against [fabiobaltieri/ppk2-eeprom](https://github.com/fabiobaltieri/ppk2-eeprom)
and the desktop app's source.

- **There is no 32.768 kHz crystal.** X1 is a 32 MHz HFXO; `P0.00/XL1` and
  `P0.01/XL2` carry `SW1`/`SW2`. LFCLK is synthesised from HFCLK
  (`CONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH=y`); Zephyr's `XTAL` default would
  hang.
- **The application links at `0x1000`** to keep the factory nRF5 bootloader
  (`CONFIG_FLASH_LOAD_OFFSET`, controlled by `BOARD_HAS_NRF5_BOOTLOADER`).
- **`P0.09`/`P0.10` are the NFCT pins** but carry `LP_EN`/`LP_D0`, hence
  `nfct-pins-as-gpios`.
- **Every MCU-driven control line first gates an N-channel MOSFET**
  (`DMN2050LFDB`, drains crossed: pin 3 = D2, pin 6 = D1). For the `CAL*`
  loads and the LED channels that is the whole story: **active high**. The
  three power switches add a second stage that inverts it: Q2/Q14/Q16 are
  N-channel high-side pairs with gates pulled to the **+8 V rail** — the only
  reason an 8 V rail exists — so they conduct when left alone, and driving
  `VOUT_EN`/`VLDO_EN`/`VEXT_EN` *high* pulls that gate down and **opens** the
  switch. Hence `GPIO_ACTIVE_LOW` for those three, and the 1 MΩ pull-ups to
  3V3 on exactly those lines that park them "off" through reset.
- **`SW1..SW4` are inputs**: pulled up by 4.7 kΩ, pulled low by Q6/Q8 while a
  shunt is in circuit. High means bypassed. The MCU cannot force a range.
- **Shunt ladder**: R41 1 kΩ always in, with 110 Ω, 11 Ω, 1 Ω and 0.051 Ω
  switched in parallel — reproducing the metadata's R0 ≈ 1003, R1 ≈ 101,
  R2 ≈ 10.3 exactly.
- **SoC power**: `VDDH` (Y2) is on P3V3 with all `VDD` pads → normal voltage
  mode; `DCC` (B3) and `DCCH` (AB2) are unconnected → the internal DC/DC cannot
  be used. Regulator nodes are left at SoC defaults.
- **I2C**: `SDA` P0.24, `SCL` P0.25. EEPROM 24CW160 at `0x50` (WP strapped
  low), MCP4451 at `0x2C`.
- **`P1.13` is `REG_EN`** (ADP2504 + ADP1708 enable); `P1.01–03, 08, 10–12,
  14, 15` are unconnected.

### Pinout

| Pin | Net | Function |
| --- | --- | --- |
| P0.00 | `SW1_ONOFF` | range switch 1 status (input) |
| P0.01 | `SW2_ONOFF` | range switch 2 status (input) |
| P0.02 / AIN0 | `VREF_IA` | ADC: instrumentation-amp reference |
| P0.03 / AIN1 | `VLDO` | ADC: LDO output, /5 |
| P0.04 / AIN2 | `VBB` | ADC: buck-boost output, /5 |
| P0.05 / AIN3 | `VIN` | ADC: internal 5 V rail (USB VBUS via Q12/Q18), /5 |
| P0.06 | `VEXT_EN` | `EXT_POWER_IN` terminal → VDUT+ (ampere mode); active low |
| P0.07 | `VLDO_EN` | LDO → VDUT+ (source mode); active low |
| P0.08 | `ANA_EN` | ADP5072 ±rail enable |
| P0.09 | `LP_EN` | logic-port translator enable (NFC1) |
| P0.10 – P0.17 | `LP_D0` – `LP_D7` | logic port channels 0–7 (P0.10 is NFC2) |
| P0.18 | `RESET` | nRESET |
| P0.19 | `RESET_TRIGGER` | on the RESET net |
| P0.20 – P0.23 | `CAL100K`, `CAL10K`, `CAL1K`, `CAL100` | calibration loads R40, R58, R33, R38 |
| P0.24 / P0.25 | `SDA` / `SCL` | I2C |
| P0.26 | `SW3_ONOFF` | range switch 3 status (input) |
| P0.27 | `SW4_ONOFF` | range switch 4 status (input) |
| P0.28 / AIN4 | `VDUT` | ADC: VDUT+, /5 |
| P0.29 / AIN5 | `AGND` | ADC: analog ground reference (negative input) |
| P0.30 / AIN6 | `NTC` | ADC: thermistor |
| P0.31 / AIN7 | `VSE_IA` | ADC: instrumentation-amp output (current) |
| P1.00 | `SWO` | trace |
| P1.04 | `VOUT_EN` | VDUT- → VOUT (DUT power); active low |
| P1.05 – P1.07 | `LEDR`, `LEDG`, `LEDB` | status LEDs (PWM0), via Q15/Q1 |
| P1.09 | `EXT_USB` | J1 VBUS detect, streamed as bit 17 |
| P1.13 | `REG_EN` | ADP2504 + ADP1708 enable |

The same map is in the devicetree as `gpio-line-names` and as the
`nordic,ppk2-control` node the code reads its pins from.

## Not done

- **Factory calibration procedure.** The calibration loads, the IA trim and
  metadata editing are all there, but there is no automated routine that
  derives `R`, `GS`, `GI`, `O`, `S`, `I` from them. Shipped kits carry factory
  values in EEPROM, so this only matters for a wiped EEPROM.
- **Learned voltage curve persistence** (`CMD_SAVE_VOLTAGE_CURVE` in the stock
  firmware). The tuner re-learns the slope each session; it converges in a
  handful of steps, so nothing is stored.
- **NTC** temperature is measurable but unused.
- **PPK1 trigger commands** are accepted and ignored; the app does not use
  them with a PPK2.
