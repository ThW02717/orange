#!/usr/bin/env python3
import argparse
import struct
import time
from pathlib import Path

#  python3 bootloader/tools/uart_load.py /dev/ttyUSB0 kernel/kernel.bin --initrd kernel/initramfs.cpio --load-cmd never

BUNDLE_MAGIC = 0x324F4F42  # "BOO2"


def checksum32(data: bytes) -> int:
    return sum(data) & 0xFFFFFFFF


def resolve_initrd_path(image_path: Path, initrd_arg: str | None) -> Path:
    if initrd_arg:
        return Path(initrd_arg)
    return image_path.with_name("initramfs.cpio")


def main() -> int:
    parser = argparse.ArgumentParser(description="Send kernel image to bootloader load command")
    parser.add_argument("port", help="serial port path, e.g. /dev/ttyUSB0 or /dev/pts/N")
    parser.add_argument("image", help="kernel binary image path")
    parser.add_argument(
        "--initrd",
        default=None,
        help="initramfs path for kernel+initrd bundle (default: <image_dir>/initramfs.cpio)",
    )
    parser.add_argument("--stage", choices=["header", "payload", "all"], default="all",
                        help="send only header (for 'w'), only payload (for 's'), or both")
    parser.add_argument(
        "--load-cmd",
        choices=["auto", "always", "never"],
        default="auto",
        help=(
            "send 'load' command before transfer: "
            "auto=only when --stage all, always=always send, never=never send"
        ),
    )
    parser.add_argument(
        "--load-delay",
        type=float,
        default=0.20,
        help="seconds to wait after sending 'load' command (default: 0.20)",
    )
    args = parser.parse_args()

    image_path = Path(args.image)
    initrd_path = resolve_initrd_path(image_path, args.initrd)
    if not initrd_path.is_file():
        raise SystemExit(
            f"initrd not found: {initrd_path} (use --initrd <path> to specify explicitly)"
        )

    with open(image_path, "rb") as f:
        kernel = f.read()

    with open(initrd_path, "rb") as f:
        initrd = f.read()
    payload = (
        struct.pack("<IIII", len(kernel), len(initrd), checksum32(kernel), checksum32(initrd))
        + kernel
        + initrd
    )
    magic = BUNDLE_MAGIC

    header = struct.pack("<III", magic, len(payload), checksum32(payload))

    send_load_cmd = (args.load_cmd == "always") or (
        args.load_cmd == "auto" and args.stage == "all"
    )

    with open(args.port, "r+b", buffering=0) as tty:
        if send_load_cmd:
            tty.write(b"\rload\r")
            tty.flush()
            time.sleep(max(args.load_delay, 0.0))
        if args.stage in ("header", "all"):
            tty.write(header)
        if args.stage in ("payload", "all"):
            tty.write(payload)
        tty.flush()

    if args.stage == "header":
        print(f"sent header to {args.port}")
    elif args.stage == "payload":
        print(f"sent payload ({len(payload)} bytes) to {args.port}")
    else:
        print(
            f"sent bundle header + payload ({len(payload)} bytes, "
            f"kernel={len(kernel)}, initrd={len(initrd)} @ {initrd_path}) to {args.port}; "
            f"load_cmd={'on' if send_load_cmd else 'off'}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
