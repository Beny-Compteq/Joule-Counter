#!/usr/bin/env python3
"""Talk to the PPK2's Nordic DFU-trigger interface directly over WinUSB.

Read-only: asks for DFU_INFO (0x07) and SEMVER (0x08). Never sends DETACH.
"""
import struct
import sys

import libusb_package
import usb.core
import usb.util

VID, PID = 0x1915, 0xC00A
REQ_DFU_INFO, REQ_SEMVER = 0x07, 0x08

backend = libusb_package.get_libusb1_backend()
dev = usb.core.find(idVendor=VID, idProduct=PID, backend=backend)
if dev is None:
    sys.exit("no PPK2 found")
print(f"found {dev.serial_number!r}  product {dev.product!r}  manufacturer {dev.manufacturer!r}")

cfg = dev.get_active_configuration()
trigger = None
for intf in cfg:
    print(f"  IF{intf.bInterfaceNumber}: class {intf.bInterfaceClass:#04x} sub {intf.bInterfaceSubClass:#04x} "
          f"proto {intf.bInterfaceProtocol:#04x} eps {intf.bNumEndpoints}")
    if (intf.bInterfaceClass, intf.bInterfaceSubClass, intf.bInterfaceProtocol) == (0xFF, 0x01, 0x01):
        trigger = intf
if trigger is None:
    sys.exit("no DFU trigger interface")

ifnum = trigger.bInterfaceNumber
for name, req, length in (("DFU_INFO", REQ_DFU_INFO, 24), ("SEMVER", REQ_SEMVER, 64)):
    for recipient, label in ((usb.util.CTRL_RECIPIENT_INTERFACE, "interface"),
                             (usb.util.CTRL_RECIPIENT_DEVICE, "device")):
        bm = usb.util.build_request_type(usb.util.CTRL_IN, usb.util.CTRL_TYPE_VENDOR, recipient)
        try:
            data = dev.ctrl_transfer(bm, req, 0, ifnum if recipient == usb.util.CTRL_RECIPIENT_INTERFACE else 0,
                                     length, timeout=1000)
            raw = bytes(data)
            if name == "DFU_INFO" and len(raw) >= 24:
                addr, size, vmaj, vmin, fwid, flash, page = struct.unpack("<IIHHIII", raw[:24])
                print(f"{name:9s} via {label:9s}: addr={addr:#010x} size={size:#x} v{vmaj}.{vmin} "
                      f"fwid={fwid} flash={flash:#x} page={page:#x}")
            else:
                print(f"{name:9s} via {label:9s}: {len(raw)} bytes {raw!r}")
        except usb.core.USBError as e:
            print(f"{name:9s} via {label:9s}: ERROR {e}")
