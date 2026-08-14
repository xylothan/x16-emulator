#!/usr/bin/env python3
"""Fetch SingleStepTests/ProcessorTests and convert them for test_processor_tests.

Not committed: ~1 GB of JSON per CPU variant, one file per opcode, 10,000 cases
each. Takes a fixed prefix of each instead.

Prefix, not a random sample: the baseline records exact per-opcode counts, so
every machine must convert the same cases.

    python tests/fetch_pt.py                 # 100 cases/opcode, wdc65c02
    python tests/fetch_pt.py --cases 10000   # everything
    python tests/fetch_pt.py --variant 65816 # needs 65816 fixture support
"""

import argparse
import json
import pathlib
import sys
import time
import urllib.error
import urllib.request

import pt_convert

RAW = "https://raw.githubusercontent.com/SingleStepTests/{repo}/main/{sub}/{op}.json"

VARIANTS = {
    # wdc65c02 is the superset: the 32 Rockwell bit instructions we implement
    # plus WAI and STP. The rockwell and synertek suites are subsets of it.
    "wdc65c02": {"repo": "65x02", "sub": "wdc65c02/v1"},
    "65816": {"repo": "65816", "sub": "v1"},
}

ATTEMPTS = 4


def fetch_opcode(variant: str, op: int, dest: pathlib.Path) -> bool:
    spec = VARIANTS[variant]
    out = dest / f"{op:02x}.json"
    if out.exists() and out.stat().st_size > 0:
        return True
    url = RAW.format(repo=spec["repo"], sub=spec["sub"], op=f"{op:02x}")
    # 256 requests per run, so a transient failure is likely rather than
    # exceptional. Retry anything that is not a definite 404.
    for attempt in range(ATTEMPTS):
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                data = r.read()
            out.write_bytes(data)
            return True
        except urllib.error.HTTPError as e:
            if e.code == 404:
                return False
            last = e
        except Exception as e:
            last = e
        if attempt < ATTEMPTS - 1:
            time.sleep(2 ** attempt)
    raise SystemExit(f"${op:02X}: giving up after {ATTEMPTS} attempts: {last}")


def load_cases(path: pathlib.Path):
    """Cases in one opcode's file, or [] if it holds none.

    $CB (WAI) and $DB (STP) are empty upstream: they stop the processor, so
    there is no state after one instruction.
    """
    if path.stat().st_size == 0:
        return []
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        path.unlink()
        raise SystemExit(f"{path.name} is not valid JSON; deleted, run again")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="wdc65c02", choices=sorted(VARIANTS))
    ap.add_argument(
        "--cases",
        type=int,
        default=100,
        help="cases per opcode, taken as a prefix (default 100, max 10000)",
    )
    ap.add_argument(
        "--opcodes",
        default="",
        help="comma-separated hex opcodes; default is all 256",
    )
    ap.add_argument(
        "--force",
        action="store_true",
        help="re-fetch and re-convert even if the fixture already exists",
    )
    args = ap.parse_args()

    here = pathlib.Path(__file__).resolve().parent
    out = here / "pt" / f"{args.variant}.bin"

    # The fixture is ~1 MB and the JSON it came from is ~1 GB, so CI caches
    # only the fixture. Nothing to do when the cache hit.
    if out.exists() and out.stat().st_size > 0 and not args.force:
        print(f"{out} already present ({out.stat().st_size / 1e6:.1f} MB); "
              f"pass --force to rebuild")
        return 0

    dest = here / "pt" / args.variant
    dest.mkdir(parents=True, exist_ok=True)

    if args.opcodes:
        wanted = [int(x, 16) for x in args.opcodes.split(",")]
    else:
        wanted = list(range(256))

    have = []
    for op in wanted:
        sys.stdout.write(f"\rfetching ${op:02X}...")
        sys.stdout.flush()
        if fetch_opcode(args.variant, op, dest):
            have.append(op)
    print(f"\r{len(have)} opcodes available          ")

    cases = []
    empty = []
    short = []
    for op in have:
        raw = load_cases(dest / f"{op:02x}.json")
        if not raw:
            empty.append(op)
            continue
        if len(raw) < args.cases:
            short.append((op, len(raw)))
        cases.extend((op, c) for c in raw[: args.cases])

    if empty:
        print("no cases: " + " ".join(f"${o:02X}" for o in empty))
    for op, n in short:
        print(f"  ${op:02X}: only {n} cases, wanted {args.cases}")

    out = here / "pt" / f"{args.variant}.bin"
    pt_convert.write_fixture(out, cases)
    print(f"wrote {out} -- {len(cases)} cases, {out.stat().st_size / 1e6:.1f} MB")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
