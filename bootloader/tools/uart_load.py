#!/usr/bin/env python3
import argparse
import struct

MAGIC = 0x544F4F42  # "BOOT"


def checksum32(data: bytes) -> int:
    return sum(data) & 0xFFFFFFFF


def main() -> int:
    parser = argparse.ArgumentParser(description="Send kernel image to bootloader load command")
    parser.add_argument("port", help="serial port path, e.g. /dev/ttyUSB0 or /dev/pts/N")
    parser.add_argument("image", help="kernel binary image path")
    parser.add_argument("--stage", choices=["header", "payload", "all"], default="all",
                        help="send only header (for 'w'), only payload (for 's'), or both")
    args = parser.parse_args()

    with open(args.image, "rb") as f:
        payload = f.read()

    header = struct.pack("<III", MAGIC, len(payload), checksum32(payload))

    with open(args.port, "wb", buffering=0) as tty:
        if args.stage in ("header", "all"):
            tty.write(header)
        if args.stage in ("payload", "all"):
            tty.write(payload)

    if args.stage == "header":
        print(f"sent header to {args.port}")
    elif args.stage == "payload":
        print(f"sent payload ({len(payload)} bytes) to {args.port}")
    else:
        print(f"sent header + payload ({len(payload)} bytes) to {args.port}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
