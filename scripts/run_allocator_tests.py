#!/usr/bin/env python3
import argparse
import selectors
import subprocess
import sys
import time
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
KERNEL_DIR = REPO_ROOT / "kernel"
QEMU_ELF = KERNEL_DIR / "build" / "kernel_qemu.elf"
PROMPT = b"> "
PASS_TOKEN = b"[TEST] SUMMARY: PASS"
FAIL_TOKEN = b"[TEST] "
TEST_LINE_RE = re.compile(rb"^\[TEST\].*$", re.MULTILINE)


def build_kernel() -> None:
    subprocess.run(
        ["make", "-C", str(KERNEL_DIR), "build/kernel_qemu.elf"],
        check=True,
    )


def default_qemu_cmd() -> list[str]:
    return [
        "qemu-system-riscv64",
        "-M",
        "virt",
        "-smp",
        "1",
        "-m",
        "256M",
        "-nographic",
        "-bios",
        "default",
        "-kernel",
        str(QEMU_ELF),
    ]


def read_until(proc: subprocess.Popen[bytes], token: bytes, timeout: float, stream_output: bool) -> bytes:
    sel = selectors.DefaultSelector()
    assert proc.stdout is not None
    sel.register(proc.stdout, selectors.EVENT_READ)
    buf = bytearray()
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if proc.poll() is not None:
            break

        remaining = max(0.0, deadline - time.monotonic())
        events = sel.select(remaining)
        if not events:
            continue

        for key, _ in events:
            chunk = key.fileobj.read1(4096)
            if not chunk:
                continue
            if stream_output:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
            buf.extend(chunk)
            if token in buf:
                sel.close()
                return bytes(buf)

    sel.close()
    raise TimeoutError(f"timed out waiting for token: {token!r}")


def emit_compact_summary(output: bytes) -> None:
    lines = TEST_LINE_RE.findall(output)
    if lines:
        for line in lines:
            print(line.decode("utf-8", errors="replace"))
        return
    print("[runner] no [TEST] lines found")


def run_allocator_tests(timeout: float, verbose: bool) -> int:
    proc = subprocess.Popen(
        default_qemu_cmd(),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        cwd=REPO_ROOT,
    )

    try:
        boot_output = read_until(proc, PROMPT, timeout, stream_output=verbose)
        assert proc.stdin is not None
        proc.stdin.write(b"kmtest all\r")
        proc.stdin.flush()
        output = read_until(proc, PASS_TOKEN, timeout, stream_output=verbose)
        if PASS_TOKEN not in output:
            if not verbose:
                sys.stdout.buffer.write(boot_output + output)
                sys.stdout.buffer.flush()
            return 1
        if not verbose:
            emit_compact_summary(output)
        return 0
    except TimeoutError as exc:
        if not verbose:
            print("[runner] timed out; rerun with --verbose to inspect full UART log", file=sys.stderr)
        print(f"\n[runner] {exc}", file=sys.stderr)
        return 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2.0)


def main() -> int:
    parser = argparse.ArgumentParser(description="Boot QEMU and run kmtest all")
    parser.add_argument(
        "--timeout",
        type=float,
        default=20.0,
        help="seconds to wait for boot and test completion (default: 20)",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="use existing kernel/build/kernel_qemu.elf without rebuilding",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="stream full QEMU UART output instead of only [TEST] lines",
    )
    args = parser.parse_args()

    if not args.skip_build:
        build_kernel()

    if not QEMU_ELF.is_file():
        print(f"missing QEMU kernel image: {QEMU_ELF}", file=sys.stderr)
        return 1

    rc = run_allocator_tests(args.timeout, args.verbose)
    if rc == 0:
        print("\n[runner] allocator tests passed")
    else:
        print("\n[runner] allocator tests failed", file=sys.stderr)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
