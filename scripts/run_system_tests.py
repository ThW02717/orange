#!/usr/bin/env python3
"""Boot the real QEMU kernel and exercise syscall, fault, fork and preemption paths."""
from pathlib import Path
import re
import subprocess
import time

from run_allocator_tests import read_until

ROOT = Path(__file__).resolve().parents[1]


def command_output(proc, command, done, timeout=30, include_prior=False):
    assert proc.stdin is not None
    proc.stdin.write(command.encode() + b"\r")
    proc.stdin.flush()
    output = read_until(proc, command.encode(), timeout, False)
    if not include_prior:
        output = output[output.find(command.encode()):]
    if done not in output:
        output += read_until(proc, done, timeout, False)
    if b"fatal kernel fault" in output or b"FAIL" in output:
        raise AssertionError(output.decode(errors="replace"))
    return output


def test_lab_commands(proc):
    # Shorter timeouts added later must fire first. Each command reuses the
    # shell input buffer, so this also checks that messages were copied.
    output = b""
    for command in (
        "setTimeout 0.6 late", "setTimeout 0.1 early",
        "setTimeout 0.3 message with spaces",
    ):
        output += command_output(proc, command, b"setTimeout: queued", include_prior=True)
    output += command_output(proc, "hello", b"Hello World!", include_prior=True)
    if b"message=late" not in output:
        output += read_until(proc, b"message=late", 5, False)
    events = re.findall(rb"\[timeout\] submitted=(\d+\.\d+)s fired=(\d+\.\d+)s message=([^\r\n]+)", output)
    assert [event[2] for event in events] == [b"early", b"message with spaces", b"late"], output
    assert output.index(b"Hello World!") < output.index(b"[timeout]"), output
    for submitted, fired, message in events:
        delay = {b"early": 0.1, b"message with spaces": 0.3, b"late": 0.6}[message]
        assert float(fired) - float(submitted) >= delay - 0.002, output
    print("[SYSTEM] setTimeout order, copied messages, timestamps, nonblocking shell: PASS")

    for command in (
        "setTimeout . bad", "setTimeout -1 bad", "setTimeout 1",
        "setTimeout 18446744073709551617 bad",
    ):
        command_output(proc, command, b"usage: setTimeout")
    command_output(proc, "setTimeout 0 immediate", b"message=immediate")
    print("[SYSTEM] setTimeout invalid input and zero delay: PASS")

    command_output(proc, "timer periodic on", b"timer: periodic on")
    output = b""
    while len(re.findall(rb"\[timer\] uptime=(\d+) seconds", output)) < 2:
        output += read_until(proc, b" seconds", 5, False)
    times = [int(t) for t in re.findall(rb"\[timer\] uptime=(\d+) seconds", output)]
    assert times[1] - times[0] == 2, output
    command_output(proc, "timer periodic off", b"timer: periodic off")
    time.sleep(2.1)
    output = command_output(proc, "hello", b"Hello World!", include_prior=True)
    assert b"[timer] uptime=" not in output, output
    print("[SYSTEM] two-second uptime demo enable/disable: PASS")

    # Real UART external IRQs: wait until the demo requests its RX byte.
    command_output(proc, "demo uart", b"type one printable key:")
    assert proc.stdin is not None
    proc.stdin.write(b"Z")
    proc.stdin.flush()
    output = read_until(proc, b"uartdemo: PASS", 5, False)
    assert b"rx_fallback=0" in output and b"tx_poll=0" in output, output
    print("[SYSTEM] UART RX/TX through PLIC, no polling fallback: PASS")

    # The supplied image ends in an infinite U-mode loop. Run it last and
    # verify that timer interrupts still arrive after all five ecall returns.
    command_output(proc, "timer periodic on", b"timer: periodic on")
    output = command_output(proc, "exec prog", b"[trap] lab ecall a7=1")
    assert re.findall(rb"\[trap\] lab ecall a7=(\d+)", output) == [b"5", b"4", b"3", b"2", b"1"], output
    assert len(re.findall(rb"\[trap\] scause=(?:0x)?0*8 ", output)) == 5, output
    output += read_until(proc, b" seconds", 5, False)
    assert b"[timer] uptime=" in output and b"fatal kernel fault" not in output, output
    print("[SYSTEM] supplied Lab 4 image: five ecalls and timer IRQ during U-mode loop: PASS")


def main() -> int:
    subprocess.run(["make", "-C", str(ROOT / "kernel"), "build/kernel_qemu.elf",
                    "build/initramfs_qemu.cpio", "-j4"], check=True)
    # Byte-for-byte match with OSC2026/uploads/prog.bin (14 bytes).
    assert (ROOT / "kernel/build/user/qemu_prog.bin").read_bytes() == bytes.fromhex(
        "954873000000fd18e31d10ff01a0")
    proc = subprocess.Popen([
        "qemu-system-riscv64", "-M", "virt", "-smp", "1", "-m", "256M",
        "-nographic", "-bios", "default", "-kernel", str(ROOT / "kernel/build/kernel_qemu.elf"),
        "-initrd", str(ROOT / "kernel/build/initramfs_qemu.cpio"),
    ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        read_until(proc, b"> ", 15, False)
        cases = [
            ("demo test", b"[test]: done", b"pid="),
            ("exec test", b"user task finished, back to shell", b"[trap] scause="),
            ("demo tasks", b"[tasks] PASS", b"low:start -> timer:top -> low:resume -> timer:bottom -> high -> equal -> lower"),
            ("demo badinst", b"[badinst]: done", b"[trap] user fault, terminate"),
            ("demo fork", b"[fork]: done", b"child2:"),
            ("demo preempt", b"[preempt]: done", b"busy: pid="),
            ("demo threads", b"[threads]: done", b"[threads] stop group by original PID"),
            ("runu threads", b"user task finished, back to shell", b"[threads] stop group by original PID"),
            ("demo test", b"[test]: done", b"pid="),
            ("addtimer 18446744073709551617", b"> ", b"usage: addtimer"),
            ("demo stress 18446744073709551617", b"> ", b"usage:"),
            ("addtimer 0.01", b"[T1]", b"[T1]"),
            ("kmtest all", b"[TEST] SUMMARY: PASS", b"[TEST] large: PASS"),
            ("mem", b"total_free_pages:", b"Slab caches:"),
        ]
        for command, done, required in cases:
            output = command_output(proc, command, done)
            if required not in output or b"fatal kernel fault" in output or b"FAIL" in output:
                raise AssertionError(output.decode(errors="replace"))
            if command == "demo preempt":
                pids = re.findall(rb"busy: pid=(\d+)", output)
                assert len(pids) == 8 and len(set(pids[:4])) == 2, output.decode(errors="replace")
            if command == "demo test":
                assert re.search(rb"pid=\d+\r?\n", output), output.decode(errors="replace")
            if command in ("demo threads", "runu threads"):
                for marker in (
                    b"invalid entry/return: PASS", b"shared stack buffer: PASS",
                    b"same PID, different TID, shared global: PASS",
                    b"multithreaded fork/exec rejection: PASS",
                    b"fork gets a new PID/TID: PASS",
                    b"cross-process image replacement rejected: PASS",
                    b"exit_group with a live sibling",
                    b"returning worker and single-thread fork: PASS",
                    b"leader exit and nonleader create: PASS",
                ):
                    assert marker in output, output.decode(errors="replace")
            print(f"[SYSTEM] {command}: PASS")
        test_lab_commands(proc)
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
