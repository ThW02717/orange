#!/usr/bin/env python3
"""Upload a QEMU kernel bundle through the real polling bootloader shell."""
from pathlib import Path
import os
import select
import struct
import subprocess
import time

from run_allocator_tests import read_until
from run_system_tests import command_output

ROOT = Path(__file__).resolve().parents[1]


def send_bytes(proc, data, timeout=20):
    assert proc.stdin is not None
    fd = proc.stdin.fileno()
    os.set_blocking(fd, False)
    pending = memoryview(data)
    deadline = time.monotonic() + timeout
    try:
        while pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or proc.poll() is not None:
                raise TimeoutError("bootloader upload stalled")
            if select.select([], [fd], [], min(remaining, 1))[1]:
                try:
                    sent = os.write(fd, pending[:4096])
                    pending = pending[sent:]
                except BlockingIOError:
                    pass
    finally:
        os.set_blocking(fd, True)


def main():
    subprocess.run(["make", "-C", str(ROOT / "bootloader"),
                    "build/bootloader_qemu.elf", "-j4"], check=True)
    subprocess.run(["make", "-C", str(ROOT / "kernel"), "qemu-bin",
                    "build/initramfs_qemu.cpio", "-j4"], check=True)
    kernel = (ROOT / "kernel/build/kernel_qemu.bin").read_bytes()
    initrd = (ROOT / "kernel/build/initramfs_qemu.cpio").read_bytes()
    payload = struct.pack("<IIII", len(kernel), len(initrd),
                          sum(kernel) & 0xffffffff, sum(initrd) & 0xffffffff) + kernel + initrd
    header = struct.pack("<III", 0x324f4f42, len(payload), sum(payload) & 0xffffffff)
    proc = subprocess.Popen([
        "qemu-system-riscv64", "-M", "virt", "-smp", "1", "-m", "256M",
        "-nographic", "-monitor", "none", "-bios", "default",
        "-kernel", str(ROOT / "bootloader/build/bootloader_qemu.elf"),
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        read_until(proc, b"> ", 15, False)
        command_output(proc, "help", b"Load kernel+initrd bundle")
        command_output(proc, "help", b"Load kernel+initrd bundle")
        command_output(proc, "load", b"then payload")
        send_bytes(proc, header)
        read_until(proc, b" bytes to ", 5, False)
        send_bytes(proc, payload)
        output = read_until(proc, b"> ", 20, False)
        assert b"bundle ok:" in output and b"interrupt-driven I/O armed" in output, output
        command_output(proc, "ls", b"bin/test.bin")
        command_output(proc, "runu test", b"user task finished, back to shell")
        command_output(proc, "hello", b"Hello World!")
        print("[BOOTLOADER] repeated shell commands, bundle upload, initrd hints, user launch: PASS")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    main()
