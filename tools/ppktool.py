#!/usr/bin/env python3
"""Command-line utilities for talking to a PPK2 running our firmware.

Windows does not assign COM port numbers by USB interface order, so which of
the two enumerated ports is the binary data port and which is the shell can
change between machines or reboots. `identify` tells them apart empirically;
the other subcommands assume you already know (pass it as an argument).

Examples:
    python ppktool.py identify COM20 COM21
    python ppktool.py metadata COM21
    python ppktool.py stream COM21 --seconds 2
    python ppktool.py shell COM20
"""
from __future__ import annotations

import argparse
import sys
import time
from collections import Counter

import serial

from ppk_protocol import PPKDataPort, RANGE_SWITCHING, Mode


def probe_port(port: str, timeout: float = 1.0) -> str:
    """Returns 'data', 'shell', or 'unknown' for the given COM port.

    Tries the data-port GetMetadata command first: it is a single byte and
    the shell will just count it as noise (protocol.c and shell_cmds.c both
    ignore bytes they don't recognize), so probing this way first can never
    accidentally trigger a shell command.
    """
    try:
        with PPKDataPort(port, timeout=timeout) as dev:
            reply = dev.get_metadata(timeout=timeout)
    except serial.SerialException as exc:
        return f"error opening port: {exc}"

    if "END" in reply and ("R0:" in reply or "Calibrated" in reply):
        return "data"

    # Not the data port. See if it is a live shell instead.
    try:
        with serial.Serial(port, baudrate=115200, timeout=timeout) as ser:
            ser.dtr = True
            time.sleep(0.1)
            ser.reset_input_buffer()
            ser.write(b"\r\n")
            time.sleep(0.2)
            banner = ser.read(512).decode("ascii", errors="replace")
    except serial.SerialException as exc:
        return f"error opening port: {exc}"

    if "$" in banner or "ppk2" in banner.lower():
        return "shell"

    return "unknown (no recognizable reply)"


def cmd_identify(args: argparse.Namespace) -> None:
    for port in args.ports:
        result = probe_port(port, timeout=args.timeout)
        print(f"{port}: {result}")


def cmd_metadata(args: argparse.Namespace) -> None:
    with PPKDataPort(args.port) as dev:
        print(dev.get_metadata(), end="")


def cmd_stream(args: argparse.Namespace) -> None:
    ranges: Counter[int] = Counter()
    adc_sum: Counter[int] = Counter()
    adc_sq: Counter[int] = Counter()
    v_sum = 0.0
    v_sq = 0.0
    v_min = None
    v_max = None
    n = 0
    lost = 0
    expected_counter = None
    dump = open(args.dump, "wb") if args.dump else None

    print(f"streaming for {args.seconds}s ...", file=sys.stderr)
    with PPKDataPort(args.port) as dev:
        for s in dev.read_samples(args.seconds, words=args.words):
            n += 1
            ranges[s.range] += 1
            # The app scales the 12-bit field by 4 back to 14-bit counts.
            raw = s.adc * 4
            adc_sum[s.range] += raw
            adc_sq[s.range] += raw * raw
            if s.voltage_adc is not None:
                vraw = s.voltage_adc * 4
                v_sum += vraw
                v_sq += vraw * vraw
                v_min = vraw if v_min is None else min(v_min, vraw)
                v_max = vraw if v_max is None else max(v_max, vraw)
            if dump:
                dump.write(s.word.to_bytes(4, "little"))
                if s.voltage_adc is not None:
                    dump.write(s.voltage_adc.to_bytes(4, "little"))
            if expected_counter is not None and s.counter != expected_counter:
                lost += (s.counter - expected_counter) & 0x3F
            expected_counter = (s.counter + 1) & 0x3F
    if dump:
        dump.close()

    print(f"samples: {n}")
    if n:
        print(f"implied rate: {n / args.seconds:.0f} Hz "
              f"(expect ~{100000 if args.words == 1 else 50000}, minus start-up)")
    print(f"counter gaps (lost/dropped): {lost}")
    for r in sorted(ranges):
        label = "switching" if r == RANGE_SWITCHING else str(r)
        cnt = ranges[r]
        mean = adc_sum[r] / cnt
        sd = (adc_sq[r] / cnt - mean * mean) ** 0.5
        print(f"  range {label}: {cnt} samples ({100 * cnt / n:.1f}%), "
              f"raw adc mean {mean:.1f} sd {sd:.1f} (14-bit counts)")
    if n and args.words > 1:
        # Uncalibrated VDUT: 0.6 V reference at gain 1/3, 13 bits of
        # magnitude, through the 120k/30k divider.
        mv_per_count = 1800.0 / 8192.0 * 5.0
        mean = v_sum / n
        sd = (v_sq / n - mean * mean) ** 0.5
        print(f"  voltage: raw adc mean {mean:.1f} sd {sd:.1f} min {v_min} max {v_max} "
              f"(14-bit counts) = {mean * mv_per_count:.1f} mV, sd {sd * mv_per_count:.1f} mV")


def cmd_output(args: argparse.Namespace) -> None:
    with PPKDataPort(args.port) as dev:
        if args.mode:
            dev.set_power_mode(Mode.SOURCE if args.mode == "source" else Mode.AMPERE)
            time.sleep(0.2)
        if args.vdd is not None:
            dev.regulator_set_mv(args.vdd)
            time.sleep(0.2)
        if args.on is not None:
            dev.device_running_set(args.on)
    print("done")


def cmd_backup_cal(args: argparse.Namespace) -> None:
    """Read-only capture of the unit's calibration, for ../calibration/."""
    with PPKDataPort(args.port) as dev:
        text = dev.get_metadata()

    if "END" not in text:
        sys.exit(f"no complete metadata reply from {args.port}; got {len(text)} bytes")

    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    body = (
        f"PPK2 calibration capture\n"
        f"Captured {stamp} from {args.port} via GetMetadata (0x19), read-only.\n"
        f"Per-unit data -- do not load onto a different PPK2.\n"
        f"See calibration/README.md for field meanings and restore procedure.\n\n"
        f"{text}"
    )

    if args.output:
        with open(args.output, "w", encoding="ascii") as fh:
            fh.write(body)
        print(f"wrote {args.output}")
    else:
        print(body, end="")


def cmd_shell(args: argparse.Namespace) -> None:
    """Thin wrapper around pyserial's miniterm for the shell port."""
    from serial.tools import miniterm

    sys.argv = ["miniterm", args.port, "115200"]
    miniterm.main()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("identify", help="figure out which COM port is which")
    p.add_argument("ports", nargs="+", help="e.g. COM20 COM21")
    p.add_argument("--timeout", type=float, default=1.0)
    p.set_defaults(func=cmd_identify)

    p = sub.add_parser("metadata", help="read calibration metadata (data port)")
    p.add_argument("port")
    p.set_defaults(func=cmd_metadata)

    p = sub.add_parser("stream", help="start sampling, decode, report stats (data port)")
    p.add_argument("port")
    p.add_argument("--seconds", type=float, default=2.0)
    p.add_argument("--dump", metavar="FILE", help="also write the raw 32-bit sample words here")
    p.add_argument("--words", type=int, choices=[1, 2], default=1,
                   help="32-bit words per sample: 1 = PPK2/baseline format, 2 = Joule Counter (adds voltage)")
    p.set_defaults(func=cmd_stream)

    p = sub.add_parser("output", help="set mode / vdd / DUT power (data port)")
    p.add_argument("port")
    p.add_argument("--mode", choices=["ampere", "source"])
    p.add_argument("--vdd", type=int, metavar="MV")
    p.add_argument("--on", type=int, choices=[0, 1], help="DUT output on/off")
    p.set_defaults(func=cmd_output)

    p = sub.add_parser("backup-cal", help="capture calibration to a file (read-only)")
    p.add_argument("port")
    p.add_argument("-o", "--output", help="file to write; prints to stdout if omitted")
    p.set_defaults(func=cmd_backup_cal)

    p = sub.add_parser("shell", help="open the maintenance shell port (miniterm)")
    p.add_argument("port")
    p.set_defaults(func=cmd_shell)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
