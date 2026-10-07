#!/usr/bin/env python3
"""Boot the real QEMU kernel and exercise syscall, fault, fork and preemption paths."""
from pathlib import Path
import re
import subprocess

from run_allocator_tests import read_until

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    subprocess.run(["make", "-C", str(ROOT / "kernel"), "build/kernel_qemu.elf",
                    "build/initramfs_qemu.cpio", "-j4"], check=True)
    proc = subprocess.Popen([
        "qemu-system-riscv64", "-M", "virt", "-smp", "1", "-m", "256M",
        "-nographic", "-bios", "default", "-kernel", str(ROOT / "kernel/build/kernel_qemu.elf"),
        "-initrd", str(ROOT / "kernel/build/initramfs_qemu.cpio"),
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        read_until(proc, b"> ", 15, False)
        cases = [
            ("demo test", b"[test]: done", b"pid="),
            ("demo badinst", b"[badinst]: done", b"[trap] user fault, terminate"),
            ("demo fork", b"[fork]: done", b"child2:"),
            ("demo preempt", b"[preempt]: done", b"busy: pid="),
            ("addtimer 18446744073709551617", b"> ", b"usage: addtimer"),
            ("demo stress 18446744073709551617", b"> ", b"usage:"),
            ("addtimer 0.01", b"[T1]", b"[T1]"),
            ("kmtest all", b"[TEST] SUMMARY: PASS", b"[TEST] large: PASS"),
            ("mem", b"total_free_pages:", b"Slab caches:"),
        ]
        for command, done, required in cases:
            assert proc.stdin is not None
            proc.stdin.write(command.encode() + b"\r")
            proc.stdin.flush()
            # Synchronize on this command's echo, not a leftover prompt from
            # the previous command. Preserve any response in the same chunk.
            output = read_until(proc, command.encode(), 30, False)
            output = output[output.find(command.encode()):]
            if done not in output:
                output += read_until(proc, done, 30, False)
            if required not in output or b"fatal kernel fault" in output or b"FAIL" in output:
                raise AssertionError(output.decode(errors="replace"))
            if command == "demo preempt":
                pids = re.findall(rb"busy: pid=(\d+)", output)
                assert len(pids) == 8 and len(set(pids[:4])) == 2, output.decode(errors="replace")
            print(f"[SYSTEM] {command}: PASS")
        print("[SYSTEM] SUMMARY: PASS")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    raise SystemExit(main())
