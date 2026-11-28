#!/usr/bin/env python3
"""Check that the tests actually fail when the emulator is wrong.

Break the emulator on purpose, one small change at a time, and check the test
that should care goes red. A mutation that survives means the test is not
covering what it claims, or the code it changed does not matter.

    python tests/mutation_check.py --build-dir build
    python tests/mutation_check.py --build-dir build --changed
    python tests/mutation_check.py --build-dir build --only VIA --only "bus cycle"

Each mutation rebuilds only the executables its caught_by tests run.
Every mutation is reverted afterwards, including on failure or interrupt.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

CMAKE = os.environ.get("CMAKE", "cmake")
CTEST = os.environ.get("CTEST", "ctest")

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mutations import MUTATIONS  # noqa: E402


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def test_targets(build_dir, config):
    """Map each ctest name to the executable target it runs, from ctest itself.

    A test whose command runs no <name>_test executable maps to None.
    """
    r = run([CTEST, "--test-dir", build_dir, "-C", config, "--show-only=json-v1"])
    if r.returncode != 0:
        return {}
    try:
        tests = json.loads(r.stdout).get("tests", [])
    except ValueError:
        return {}
    out = {}
    for t in tests:
        target = None
        for arg in t.get("command", []):
            stem = Path(arg).stem
            if stem.endswith("_test"):
                target = stem
        out[t["name"]] = target
    return out


def targets_for(names, known):
    """The targets that build the named tests; the whole suite if any is unknown."""
    found = [known.get(n) for n in names]
    if not found or None in found:
        return ["unit_tests"]
    return sorted(set(found))


def changed_files():
    """Files that differ from origin/main, committed or not."""
    out = set()
    for cmd in (["git", "diff", "--name-only", "origin/main...HEAD"],
                ["git", "diff", "--name-only", "HEAD"]):
        r = run(cmd)
        out |= {l.strip() for l in r.stdout.splitlines() if l.strip()}
    return out


def build(build_dir, config, passes=1, targets=("unit_tests",)):
    """Build the given targets, optionally more than once.

    An .opcodes mutation regenerates src/cpu/tables.h during the build, and the
    objects that include it are only recompiled on the following pass.
    """
    out = ""
    for _ in range(passes):
        r = run([CMAKE, "--build", build_dir, "--config", config, "--target", *targets])
        out += r.stdout + r.stderr
        if r.returncode != 0:
            return False, out
    return True, out


def test_result(build_dir, config, name):
    """'passed', 'failed' or 'not run', from ctest's JUnit report.

    ctest's exit code is no use alone: a test it cannot find, or one with no
    executable for this configuration, exits non-zero as "Not Run".
    """
    fd, report = tempfile.mkstemp(suffix=".xml")
    os.close(fd)
    try:
        run([CTEST, "--test-dir", build_dir, "-C", config, "-R", f"^{name}$",
             "--output-junit", report])
        try:
            cases = [c for c in ET.parse(report).getroot().iter("testcase")
                     if c.get("name") == name]
        except (ET.ParseError, OSError):
            cases = []
    finally:
        os.unlink(report)
    if len(cases) != 1:
        return "not run"
    return {"run": "passed", "fail": "failed"}.get(cases[0].get("status"), "not run")


def tests_fail(build_dir, config, names):
    """(caught, name): caught when every named test fails; else the first that did not."""
    for name in names:
        result = test_result(build_dir, config, name)
        if result != "failed":
            return False, f"{name} {result}"
    return True, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--config", default="Debug",
                    help="build configuration, for multi-config generators")
    ap.add_argument("--only", action="append", default=[],
                    help="run the mutations whose name contains this; repeatable")
    ap.add_argument("--changed", action="store_true",
                    help="run the mutations whose file differs from origin/main")
    ap.add_argument("--list", action="store_true",
                    help="print the selected mutations and their targets, then stop")
    args = ap.parse_args()

    changed = changed_files() if args.changed else None
    selected = [m for m in MUTATIONS
                if (not args.only or any(o in m["name"] for o in args.only))
                and (changed is None or m["file"] in changed)]

    known = test_targets(args.build_dir, args.config)
    plan = [(m, targets_for(m["caught_by"], known)) for m in selected]
    if args.list:
        for m, t in plan:
            print(f"{m['name']}\n      {m['file']} -> {' '.join(t)}")
        print(f"\n{len(plan)} mutations")
        return 0

    every = sorted({t for _, ts in plan for t in ts}) or ["unit_tests"]
    if "unit_tests" in every:
        every = ["unit_tests"]
    ok, out = build(args.build_dir, args.config, targets=every)
    if not ok:
        print("the tree does not build before any mutation:\n" + out[-2000:])
        return 2

    # A test that fails, or does not run, on clean source catches every mutation.
    baseline = sorted({n for m, _ in plan
                       if not (m.get("requires") and not (ROOT / m["requires"]).exists())
                       for n in m["caught_by"]})
    broken = [(n, r) for n in baseline
              if (r := test_result(args.build_dir, args.config, n)) != "passed"]
    if broken:
        print("these tests do not pass before any mutation:")
        for n, r in broken:
            print(f"  {n}: {r}")
        return 2

    survived = []
    not_applicable = []
    for m, targets in plan:

        # Without its fixture the test skips and looks like the mutation survived.
        need = m.get("requires")
        if need and not (ROOT / need).exists():
            print(f"n/a   {m['name']}\n      needs {need}, which is not present")
            not_applicable.append(m["name"])
            continue

        path = ROOT / m["file"]
        # Universal newlines: \n patterns match a CRLF checkout; bytes are restored after.
        original = path.read_text(encoding="utf-8")

        want = m.get("count", 1)
        count = original.count(m["find"])
        if count != want:
            print(f"SKIP  {m['name']}\n      pattern matches {count} times in "
                  f"{m['file']}, expected {want} -- the source has moved on")
            survived.append(m["name"] + " (pattern stale)")
            continue

        # Building an .opcodes mutation rewrites these, and they are tracked, so
        # they have to be put back with everything else.
        GENERATED = ("src/cpu/tables.h", "src/cpu/mnemonics.h")

        backup = Path(tempfile.gettempdir()) / (path.name + ".mutation_backup")
        shutil.copy2(path, backup)
        gen_backups = {}
        for rel in GENERATED:
            gen = ROOT / rel
            if gen.exists():
                gen_backups[gen] = Path(tempfile.gettempdir()) / (gen.name + ".mutation_backup")
                shutil.copy2(gen, gen_backups[gen])
        try:
            path.write_text(original.replace(m["find"], m["into"]),
                            encoding="utf-8")

            # Delete generated headers: a restore inside one mtime tick skips regeneration.
            if path.suffix == ".opcodes":
                for gen in gen_backups:
                    gen.unlink(missing_ok=True)

            built, out = build(args.build_dir, args.config, passes=2, targets=targets)
            if not built:
                # A mutation that will not compile proves nothing; show enough to fix it.
                first = next((l for l in out.splitlines()
                              if "error" in l.lower()), "")
                print(f"SKIP  {m['name']}\n      does not compile: {first.strip()[:120]}")
                survived.append(m["name"] + " (did not compile)")
                continue

            caught, missed = tests_fail(args.build_dir, args.config, m["caught_by"])
            if caught:
                print(f"caught  {m['name']}")
            else:
                print(f"SURVIVED  {m['name']}\n"
                      f"          {missed} with this broken")
                survived.append(m["name"])
        finally:
            shutil.copy2(backup, path)
            # Restore mtime after copy2, or the build skips it and the mutation stays in.
            os.utime(path, None)
            backup.unlink(missing_ok=True)
            for gen, saved in gen_backups.items():
                shutil.copy2(saved, gen)
                os.utime(gen, None)
                saved.unlink(missing_ok=True)

    # Leave the tree built from clean source.
    ok, _ = build(args.build_dir, args.config, passes=2, targets=every)
    if not ok:
        print("the tree does not build after restoring the sources")
        return 2

    total = len(plan) - len(not_applicable)
    print(f"\n{total - len(survived)}/{total} mutations caught")
    if not_applicable:
        print(f"{len(not_applicable)} not applicable here (missing fixtures)")
    if survived:
        print("survived:")
        for s in survived:
            print(f"  {s}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
