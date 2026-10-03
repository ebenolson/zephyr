#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Stream a USB CDC ACM console to stdout, and reopen it after each reset.

The port disappears while a board is in its bootloader, so a plain
--device-serial loses the start of the test output. Use this script as a
twister --device-serial-pty, or run it by hand:

    cdc_console.py ["/dev/serial/by-id/usb-*CDC_ACM_serial_backend*"]

The port name can change between enumerations (the manufacturer part is
sometimes missing), so the default pattern matches the product name only.
Opening the port asserts DTR, which releases the testbench's console wait.
"""

import glob
import sys
import time

import serial

DEFAULT_PATTERN = "/dev/serial/by-id/usb-*CDC_ACM_serial_backend*"


def main() -> None:
    pattern = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_PATTERN
    out = sys.stdout.buffer

    while True:
        ports = sorted(glob.glob(pattern))
        if not ports:
            time.sleep(0.2)
            continue
        try:
            with serial.Serial(ports[0], 115200, timeout=0.5) as port:
                port.dtr = True
                while True:
                    data = port.read(256)
                    if data:
                        out.write(data)
                        out.flush()
        except (serial.SerialException, OSError):
            time.sleep(0.2)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
