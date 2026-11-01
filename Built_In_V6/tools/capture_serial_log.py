"""Capture a bounded diagnostic log from an ESP serial port without sending data."""

import argparse
import time
from pathlib import Path

import serial


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("port")
    parser.add_argument("output", type=Path)
    parser.add_argument("--seconds", type=int, default=120)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    deadline = time.monotonic() + args.seconds
    # A P4 reset can momentarily remove its USB Serial/JTAG interface from
    # Windows.  Keep this observer alive and reopen the same port; it never
    # writes to the device or changes DTR/RTS deliberately.
    with args.output.open("wb") as output:
        device = None
        while time.monotonic() < deadline:
            try:
                if device is None:
                    device = serial.Serial(args.port, 115200, timeout=0.25)
                chunk = device.read(device.in_waiting or 1)
                if chunk:
                    output.write(chunk)
                    output.flush()
            except serial.SerialException:
                if device is not None:
                    device.close()
                    device = None
                time.sleep(0.25)
        if device is not None:
            device.close()


if __name__ == "__main__":
    main()
