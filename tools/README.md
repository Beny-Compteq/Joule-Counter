# PPK2 host utilities

Small scripts for talking to a PPK2 running community firmware
directly over USB serial, without going through nRF Connect. Useful for
quick checks and for anything scripted. They work with both firmwares in
this repository (`baseline/`, PPK2 wire-compatible; `firmware/`, Joule
Counter) and, for the read-only commands, with Nordic's stock firmware.

| Script | Purpose |
| --- | --- |
| `ppktool.py` | Command-line front end: identify ports, read metadata, stream and summarise samples, set mode/voltage/output, back up calibration, open the shell |
| `ppk_protocol.py` | The reusable piece (`PPKDataPort`, `Cmd`, `decode_sample`) for one-off scripting |
| `dfu_pkg.py` | Wraps a built `zephyr.hex` into the nRF5 DFU zip that `nrfutil device program` needs |
| `dfu_trigger_probe.py` | Reads the DFU-trigger interface (firmware size, version string) over WinUSB; read-only |

## Setup

```powershell
cd tools
python -m venv .venv
.\.venv\Scripts\pip install -r requirements.txt
```

## The two COM ports

The kit enumerates one DFU-trigger interface (no COM port) and two CDC ACM
ports. The layout is fixed by Nordic's Windows driver, which binds the DFU
trigger by hardware ID `…&MI_00`:

| Interface | Role |
| --- | --- |
| MI_00 | DFU trigger (WinUSB, "nRF Connect DFU Trigger") |
| MI_01 | Binary protocol (data port) |
| MI_03 | Shell (`ppk` commands, logging) |

Windows does not hand out COM numbers in interface order, and they change
whenever the device's serial number changes, so do not assume. Either look
under Device Manager → device properties → Details → *Device instance path*
for `MI_01` vs `MI_03`, or run `identify`, which tells the two apart by
talking to them.

The VID/PID (`1915:C00A`) is the same for stock firmware and for both of
ours. To tell them apart, open the shell port: ours answers with a `ppk2:~$`
prompt and a single `ppk` command tree; stock has a `shell:~$` prompt and
`dfu version` reports `power_profiler_kit_2 …`.

## Usage

```powershell
# Which port is which (safe against any candidate ports)
python ppktool.py identify COM20 COM21

# Calibration metadata, as the desktop app receives it
python ppktool.py metadata COM21

# Save that reply to a file (read-only; see ../baseline/CALIBRATION.md)
python ppktool.py backup-cal COM21 -o my-kit.txt

# Stream for 2 s: sample count, counter gaps, per-range raw ADC mean/spread
python ppktool.py stream COM21 --seconds 2            # baseline/ or stock: one word per sample
python ppktool.py stream COM21 --seconds 2 --words 2  # firmware/: adds the voltage channel
python ppktool.py stream COM21 --seconds 2 --words 2 --dump raw.bin   # keep the raw words

# Mode, source-mode voltage, DUT output
python ppktool.py output COM21 --mode source --vdd 3300 --on 1

# Maintenance shell (miniterm; Ctrl-] to exit)
python ppktool.py shell COM20
```

With `--words 2` the stream summary also prints the voltage word's raw mean
and spread and its uncalibrated conversion to millivolts (`raw × 1800 / 8192
× 5`, matching the `VFS`/`VDIV` lines in the Joule Counter firmware's
metadata reply).

## Flashing over USB

No debugger is needed: the factory nRF5 bootloader stays in place and the
kit is reprogrammed through it.

```powershell
# 1. Package the build
python dfu_pkg.py ..\firmware\build\firmware\zephyr\zephyr.hex -o pkg.zip

# 2. Put the kit into the bootloader
#    - our firmware: on the shell port, run `ppk dfu`
#    - stock firmware: skip this; nrfutil triggers it through the DFU interface
python ppktool.py shell COM20

# 3. Program (serial number from `nrfutil device list`)
nrfutil device program --firmware pkg.zip --serial-number <serial>
```

`nrfutil device` is an nrfutil sub-command that has to be installed
separately. If you have nRF Connect for Desktop, its bundled copy can be run
directly:
`%LOCALAPPDATA%\Programs\nrfconnect\resources\app.asar.unpacked\resources\nrfutil-sandboxes\<ver>\device\<ver>\bin\nrfutil-device.exe program …`

The desktop app does the same thing when it offers to update a kit; the
manual route is for builds the app does not bundle.

## Checking the measurement chain without the app

The shell's `ppk cal load 100k|10k|1k|100|off` switches an on-board
calibration resistor across the terminals, and `ppk cal measure [n]
[vse|vref|vldo|vbb|vin|vdut|ntc]` returns an averaged raw ADC reading of one
channel together with the range and the DUT voltage. Combined with the raw
per-range statistics from `ppktool.py stream`, that is enough to check the
current chain against known currents (V/R at the reported VDUT) and the
voltage channel against the oversampled monitor, with no app involved.
