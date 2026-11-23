#!/usr/bin/env python3
"""Replay VERA register traffic through RTL and src/video.c; any difference fails.

    python tests/rtl_diff/run_fx.py <replay executable>

Exit 77 (CTest: skipped) without iverilog or the RTL, unless
X16_VERA_DIFF_REQUIRED is set.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RTL = HERE.parent / "vera-rtl"
RTL_SOURCES = ("addr_data.v", "vram_if.v")
SKIP = 77

# seed, sequences, ops per sequence, generator flag
CORPORA = (
    (7, 150, 400, None),
    (101, 150, 400, None),
    (31, 150, 400, "--no-fx"),
    (103, 100, 500, "--special"),
)


def unavailable(why):
    required = os.environ.get("X16_VERA_DIFF_REQUIRED", "")
    if required not in ("", "0"):
        print(f"FAIL: {why} (X16_VERA_DIFF_REQUIRED is set)")
        return 1
    print(f"skip: {why}")
    return SKIP


def apply_v49(source):
    """RTL v48.0.1 plus the VERA 49 line-draw nibble step."""
    text, n = re.subn(
        r"(wire \[16:0\] vram_addr_1_incr_decr_10 = )vram_addr_1_incr_decr_1 \+ "
        r"\{ \{6\{incr_decr_0\[10\]\}\}, incr_decr_0\};",
        "wire v49_nib_step = fx_4bit_mode_r && vram_addr_nib_incr_0_r && !vram_addr_incr_0_r;\n"
        "    wire v49_nib_after = vram_addr_nib_1_incr_decr_1;\n"
        "    wire signed [10:0] v49_step = !v49_nib_step ? incr_decr_0 :\n"
        "        vram_addr_decr_0_r ? (v49_nib_after ? 11'd0 : -11'd1) : (v49_nib_after ? 11'd1 : 11'd0);\n"
        r"    \1vram_addr_1_incr_decr_1 + { {6{v49_step[10]}}, v49_step};",
        source,
    )
    if n != 1:
        raise RuntimeError("v49 address patch did not apply")
    text, n = re.subn(
        r"(vram_addr_1_next = vram_addr_1_incr_decr_10;\s+vram_addr_nib_1_next = )"
        r"vram_addr_nib_1_incr_decr_1;",
        r"\1vram_addr_nib_1_incr_decr_1 ^ v49_nib_step;",
        text,
    )
    if n != 1:
        raise RuntimeError("v49 nibble patch did not apply")
    return text


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    replay = sys.argv[1]

    for tool in ("iverilog", "vvp"):
        if not shutil.which(tool):
            return unavailable(f"{tool} not found")
    for name in RTL_SOURCES:
        if not (RTL / name).exists():
            return unavailable(f"{name} not fetched (run tests/fetch_vera_rtl.py)")

    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        tb = tmp / "tb.vvp"
        addr_data = tmp / "addr_data.v"
        addr_data.write_text(apply_v49((RTL / "addr_data.v").read_text()))
        subprocess.run(
            ["iverilog", "-g2005", "-o", str(tb), "-s", "tb", str(HERE / "tb_fx.v"), str(HERE / "models.v"),
             str(addr_data), str(RTL / "vram_if.v")],
            check=True,
        )
        for seed, seqs, ops, flag in CORPORA:
            ops_file, rtl_file, emu_file = tmp / "ops.txt", tmp / "rtl.txt", tmp / "emu.txt"
            gen = [sys.executable, str(HERE / "gen_ops.py"), str(seed), str(seqs), str(ops), str(ops_file)]
            subprocess.run(gen + ([flag] if flag else []), check=True)
            subprocess.run([replay, str(ops_file), str(emu_file)], check=True)
            subprocess.run(
                ["vvp", str(tb), f"+ops={ops_file}", f"+out={rtl_file}"],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            print(f"seed {seed} {flag or '--fx'}:", flush=True)
            result = subprocess.run(
                [sys.executable, str(HERE / "compare.py"), str(ops_file), str(rtl_file), str(emu_file), "20"]
            )
            failed |= result.returncode != 0
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
