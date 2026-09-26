"""Wire protocol for the PPK2 data port.

Mirrors baseline/src/ppk2.h exactly -- this is not a general PPK2 client, it
is a test harness for our own firmware. If the two drift apart, trust the
firmware header and fix this file.
"""
from __future__ import annotations

import dataclasses
import struct
import time
from enum import IntEnum

import serial

# --- sample word layout (baseline/src/ppk2.h) -------------------------------

SAMPLE_ADC_MASK = 0x3FFF
SAMPLE_RANGE_POS = 14
SAMPLE_EXT_USB_POS = 17
SAMPLE_COUNTER_POS = 18
SAMPLE_COUNTER_MASK = 0x3F
SAMPLE_LOGIC_POS = 24

RANGE_SWITCHING = 7


class Cmd(IntEnum):
    """Opcodes the data port accepts (baseline/src/ppk2.h: enum ppk2_cmd)."""

    TRIGGER_SET = 0x01
    AVG_NUM_SET = 0x02
    TRIGGER_WINDOW_SET = 0x03
    TRIGGER_INTERVAL_SET = 0x04
    TRIGGER_SINGLE_SET = 0x05
    AVERAGE_START = 0x06
    AVERAGE_STOP = 0x07
    RANGE_SET = 0x08
    LCD_SET = 0x09
    TRIGGER_STOP = 0x0A
    DEVICE_RUNNING_SET = 0x0C
    REGULATOR_SET = 0x0D
    SWITCH_POINT_DOWN = 0x0E
    SWITCH_POINT_UP = 0x0F
    SET_POWER_MODE = 0x11
    RES_USER_SET = 0x12
    SPIKE_FILTERING_ON = 0x15
    SPIKE_FILTERING_OFF = 0x16
    GET_METADATA = 0x19
    RESET = 0x20
    SET_USER_GAINS = 0x25


class Mode(IntEnum):
    AMPERE = 1
    SOURCE = 2


@dataclasses.dataclass
class Sample:
    adc: int
    range: int
    ext_usb: bool
    counter: int
    logic: int
    word: int = 0
    # Joule Counter firmware only: raw VDUT ADC from the second word.
    voltage_adc: int | None = None


def decode_sample(word: int, vword: int | None = None) -> Sample:
    return Sample(
        word=word,
        voltage_adc=None if vword is None else vword & SAMPLE_ADC_MASK,
        adc=word & SAMPLE_ADC_MASK,
        range=(word >> SAMPLE_RANGE_POS) & 0x7,
        ext_usb=bool((word >> SAMPLE_EXT_USB_POS) & 0x1),
        counter=(word >> SAMPLE_COUNTER_POS) & SAMPLE_COUNTER_MASK,
        logic=(word >> SAMPLE_LOGIC_POS) & 0xFF,
    )


class PPKDataPort:
    """The custom CDC ACM port carrying the PPK2 binary protocol.

    Not the shell port -- see tools/identify.py to tell them apart on a
    given machine, since Windows does not assign COM numbers by USB
    interface order.
    """

    def __init__(self, port: str, timeout: float = 2.0) -> None:
        self._ser = serial.Serial(port, baudrate=115200, timeout=timeout)
        # DTR gates whether our firmware's link-state callback (and so
        # protocol.c's CMD_INTERNAL_LINK) sees this as "connected".
        self._ser.dtr = True
        time.sleep(0.05)

    def close(self) -> None:
        self._ser.close()

    def __enter__(self) -> "PPKDataPort":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def send_command(self, cmd: Cmd, *args: int) -> None:
        self._ser.write(bytes([int(cmd), *args]))

    def get_metadata(self, timeout: float = 1.0) -> str:
        """Sends GetMetadata and reads the text reply up to 'END'.

        Safe to call whether or not a stream is running: the firmware stops
        any running stream before replying (protocol.c: handle_get_metadata).
        """
        self._ser.timeout = timeout
        self.send_command(Cmd.GET_METADATA)
        buf = b""
        deadline = time.monotonic() + timeout * 5
        while b"END" not in buf and time.monotonic() < deadline:
            chunk = self._ser.read(256)
            if not chunk:
                break
            buf += chunk
        return buf.decode("ascii", errors="replace")

    def average_start(self) -> None:
        self.send_command(Cmd.AVERAGE_START)

    def average_stop(self) -> None:
        self.send_command(Cmd.AVERAGE_STOP)

    def device_running_set(self, on: bool) -> None:
        self.send_command(Cmd.DEVICE_RUNNING_SET, 1 if on else 0)

    def set_power_mode(self, mode: Mode) -> None:
        self.send_command(Cmd.SET_POWER_MODE, int(mode))

    def regulator_set_mv(self, mv: int) -> None:
        # RegulatorSet takes mV as two bytes, high byte first
        # (baseline/src/protocol.c: (arg[0] << 8) | arg[1]).
        self.send_command(Cmd.REGULATOR_SET, (mv >> 8) & 0xFF, mv & 0xFF)

    def read_samples(self, duration_s: float, words: int = 1):
        """Yields decoded Sample objects for duration_s of streaming.

        words is the number of 32-bit words per sample: 1 for PPK2-format
        firmware (stock, baseline/), 2 for the Joule Counter firmware, whose
        second word carries the DUT voltage.

        Sends AverageStart on entry and AverageStop on exit/exception, so a
        Ctrl-C mid-stream still leaves the device idle.
        """
        size = 4 * words
        self._ser.timeout = 0.5
        self.average_start()
        try:
            remainder = b""
            deadline = time.monotonic() + duration_s
            while time.monotonic() < deadline:
                chunk = self._ser.read(4096)
                if not chunk:
                    continue
                buf = remainder + chunk
                n = len(buf) - (len(buf) % size)
                for off in range(0, n, size):
                    (word,) = struct.unpack_from("<I", buf, off)
                    vword = None
                    if words > 1:
                        (vword,) = struct.unpack_from("<I", buf, off + 4)
                    yield decode_sample(word, vword)
                remainder = buf[n:]
        finally:
            self.average_stop()
