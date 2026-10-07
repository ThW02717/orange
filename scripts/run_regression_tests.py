#!/usr/bin/env python3
"""Run isolated RV64 regressions against the production trap and scheduler."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from run_allocator_tests import read_until

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trap-source", type=Path, default=ROOT / "kernel/src/trap_entry.S")
    args = parser.parse_args()
    compiler = os.environ.get("CROSS_COMPILE", "riscv64-unknown-elf-") + "gcc"
    with tempfile.TemporaryDirectory(prefix="orange-regression-") as directory:
        parser_test = Path(directory) / "parser-test"
        dtb = Path(directory) / "fixture.dtb"
        subprocess.run(["dtc", "-I", "dts", "-O", "dtb", "-o", str(dtb),
                        str(ROOT / "tests/parser_fixture.dts")], check=True)
        subprocess.run([
            "cc", "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-iquote", str(ROOT / "kernel/include"), str(ROOT / "tests/parser_regression.c"),
            str(ROOT / "kernel/src/cpio.c"), str(ROOT / "kernel/src/fdt.c"),
            "-o", str(parser_test),
        ], check=True)
        # LeakSanitizer cannot inspect processes under the desktop sandbox's
        # ptrace supervision; address and undefined-behaviour checks stay on.
        subprocess.run([str(parser_test), str(dtb), str(ROOT / "kernel/x1_orangepi-rv2.dtb")], check=True,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        elf = Path(directory) / "regression.elf"
        subprocess.run([
            compiler, "-march=rv64imac_zicsr", "-mabi=lp64", "-mcmodel=medany",
            "-msmall-data-limit=0", "-mno-relax", "-ffreestanding", "-nostdlib",
            "-DQEMU", "-fno-builtin", "-O2", "-Wall", "-Wextra", "-Werror",
            "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
            "-I", str(ROOT / "kernel/include"),
            "-Wl,-T," + str(ROOT / "tests/regression.ld"),
            str(ROOT / "tests/trap_roundtrip.S"), str(ROOT / "tests/kernel_regression.c"),
            str(args.trap_source), str(ROOT / "kernel/src/switch_to.S"),
            str(ROOT / "kernel/src/irq_task.c"), str(ROOT / "kernel/src/ringbuf.c"),
            str(ROOT / "kernel/src/sbi.c"), "-o", str(elf),
        ], check=True)
        proc = subprocess.Popen([
            "qemu-system-riscv64", "-M", "virt", "-smp", "1", "-m", "128M",
            "-nographic", "-bios", "default", "-kernel", str(elf),
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            prefix = read_until(proc, b"[REGRESSION] UART masked read ready", 15, False)
            assert proc.stdin is not None
            proc.stdin.write(b"Z")
            proc.stdin.flush()
            suffix, _ = proc.communicate(timeout=15)
            output = (prefix + suffix).decode(errors="replace")
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        for line in output.splitlines():
            if "[REGRESSION]" in line or "corrupted" in line or "expected" in line:
                print(line)
        if "[REGRESSION] SUMMARY: PASS" not in output or proc.returncode != 0:
            print(output)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
