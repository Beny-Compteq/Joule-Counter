#!/usr/bin/env python3
"""Build an nRF5 SDK DFU package (.zip) for the PPK2's Open Bootloader.

`nrfutil device program` needs an SdfuZip for devices carrying the nordicDfu
trait; the desktop app's device library builds one internally, the CLI does
not, and Nordic's `nrf5sdk-tools` packager is a separate install. This is the
unsigned (Open Bootloader) variant of the format:

    manifest.json   {"manifest": {"application": {"bin_file", "dat_file"}}}
    app.bin         raw application image, as the bootloader writes it
    app.dat         init packet: protobuf-encoded dfu-cc.proto `Packet`

Stdlib only. Accepts Intel HEX (what Zephyr emits and what Joule-Counter
ships) or a raw .bin.

    python dfu_pkg.py ..\\Joule-Counter\\firmware\\pca63100_ppk2_1.2.4_baseline.hex -o pkg.zip
    nrfutil device program --firmware pkg.zip --serial-number <sn>
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import zipfile
from pathlib import Path

# Where the Open Bootloader on this board expects the application to start:
APP_START = 0x1000

# dfu-cc.proto enums
OP_INIT = 1
FW_TYPE_APPLICATION = 0
HASH_SHA256 = 3


def parse_ihex(path: Path) -> tuple[int, bytes]:
    """Returns (start_address, contiguous image), gaps filled with 0xFF."""
    mem: dict[int, int] = {}
    base = 0
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if line[0] != ":":
            raise ValueError(f"{path}:{lineno}: not an Intel HEX record")
        rec = bytes.fromhex(line[1:])
        count, addr, rtype = rec[0], (rec[1] << 8) | rec[2], rec[3]
        data = rec[4 : 4 + count]
        if (sum(rec) & 0xFF) != 0:
            raise ValueError(f"{path}:{lineno}: checksum mismatch")
        if rtype == 0x00:
            for k, b in enumerate(data):
                mem[base + addr + k] = b
        elif rtype == 0x01:
            break
        elif rtype == 0x02:
            base = ((data[0] << 8) | data[1]) << 4
        elif rtype == 0x04:
            base = ((data[0] << 8) | data[1]) << 16
        elif rtype in (0x03, 0x05):
            pass  # start-address records, not part of the image
        else:
            raise ValueError(f"{path}:{lineno}: unsupported record type {rtype:#04x}")

    if not mem:
        raise ValueError(f"{path}: no data records")
    lo, hi = min(mem), max(mem)
    image = bytes(mem.get(a, 0xFF) for a in range(lo, hi + 1))
    return lo, image


# --- minimal protobuf (proto2) encoder ---------------------------------------

def _varint(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def _field_varint(num: int, value: int) -> bytes:
    return _varint((num << 3) | 0) + _varint(value)


def _field_bytes(num: int, payload: bytes) -> bytes:
    return _varint((num << 3) | 2) + _varint(len(payload)) + payload


def build_init_packet(app: bytes, fw_version: int, hw_version: int, sd_req: list[int],
                      reversed_hash: bool = True) -> bytes:
    """Encodes dfu-cc.proto `Packet{ command{ op_code=INIT, init{...} } }`.

    The SHA-256 is stored byte-reversed, as Nordic's own packager does for the
    bootloader's little-endian crypto backend. A wrong order is rejected by the
    bootloader with a hash error, never applied, so this is safe to get wrong.
    """
    digest = hashlib.sha256(app).digest()
    if reversed_hash:
        digest = digest[::-1]

    hash_msg = _field_varint(1, HASH_SHA256) + _field_bytes(2, digest)

    packed_sd_req = b"".join(_varint(v) for v in sd_req)
    init = (
        _field_varint(1, fw_version)
        + _field_varint(2, hw_version)
        + _field_bytes(3, packed_sd_req)
        + _field_varint(4, FW_TYPE_APPLICATION)
        + _field_varint(5, 0)          # sd_size
        + _field_varint(6, 0)          # bl_size
        + _field_varint(7, len(app))   # app_size
        + _field_bytes(8, hash_msg)
        + _field_varint(9, 0)          # is_debug
    )
    command = _field_varint(1, OP_INIT) + _field_bytes(2, init)
    return _field_bytes(1, command)


def build_package(app: bytes, out: Path, fw_version: int, hw_version: int, sd_req: list[int],
                  reversed_hash: bool) -> None:
    manifest = {"manifest": {"application": {"bin_file": "app.bin", "dat_file": "app.dat"}}}
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=4))
        z.writestr("app.bin", app)
        z.writestr("app.dat", build_init_packet(app, fw_version, hw_version, sd_req, reversed_hash))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("firmware", type=Path, help=".hex or .bin application image")
    p.add_argument("-o", "--output", type=Path, required=True, help="package .zip to write")
    p.add_argument("--fw-version", type=lambda s: int(s, 0), default=0xFFFFFFFF,
                   help="application version; the maximum always passes downgrade checks")
    p.add_argument("--hw-version", type=lambda s: int(s, 0), default=52, help="52 for nRF52840")
    p.add_argument("--sd-req", type=lambda s: int(s, 0), action="append",
                   help="required SoftDevice ID(s); default 0x00 = none")
    p.add_argument("--natural-hash", action="store_true",
                   help="store the SHA-256 in natural byte order instead of reversed")
    args = p.parse_args()

    if args.firmware.suffix.lower() == ".hex":
        start, app = parse_ihex(args.firmware)
        if start != APP_START:
            sys.exit(f"image starts at {start:#x}, expected {APP_START:#x}: this is not an "
                     f"application built for the bootloader at 0x0-0xFFF")
    else:
        app = args.firmware.read_bytes()

    build_package(app, args.output, args.fw_version, args.hw_version,
                  args.sd_req or [0x00], not args.natural_hash)
    print(f"wrote {args.output}: {len(app)} byte application, "
          f"sha256 {hashlib.sha256(app).hexdigest()[:16]}...")


if __name__ == "__main__":
    main()
