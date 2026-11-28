#!/usr/bin/env python3
"""Check that the tests actually fail when the emulator is wrong.

Break the emulator on purpose, one small change at a time, and check the test
that should care goes red. A mutation that survives means the test is not
covering what it claims, or the code it changed does not matter.

    python tests/mutation_check.py --build-dir build
    python tests/mutation_check.py --build-dir build --changed
    python tests/mutation_check.py --build-dir build --only VIA --only "bus cycle"
    python tests/mutation_check.py --build-dir build --check
    python tests/mutation_check.py --build-dir build --shard 2/4
    python tests/mutation_check.py --build-dir build --workdir ../mutwork

--workdir mutates a mirror of the tree, with its own build directory, so this
checkout stays usable meanwhile; with --shard, several can run side by side,
each in its own DIR.

--check builds nothing: it checks each pattern still matches, each test exists,
and, where the build has a compile_commands.json, that each mutant compiles.
The mutations themselves are in tests/mutations/, one module per area.

Each mutation rebuilds only the executables its caught_by tests run.
Every mutation is reverted afterwards, including on failure or interrupt.
"""

import argparse
import json
import os
import shlex
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
    try:
        r = run([CTEST, "--test-dir", build_dir, "-C", config, "--show-only=json-v1"])
    except OSError:
        return {}
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


def list_problems(mutations):
    """What is wrong with the list itself, before any source is read.

    Two entries with one name, or making one change, are what a merge that
    adds the same entries on both sides leaves behind.
    """
    problems = []
    names, edits = {}, {}
    for m in mutations:
        missing = [k for k in ("name", "file", "find", "into", "caught_by") if not m.get(k) and k != "into"]
        if "into" not in m:
            missing.append("into")
        if missing:
            problems.append(f"{m.get('name', '?')!r} has no {', '.join(missing)}")
            continue
        if m["name"] in names:
            problems.append(f"two mutations are named {m['name']!r}")
        names[m["name"]] = True
        edit = (m["file"], m["find"], m["into"])
        if edit in edits:
            problems.append(f"{m['name']!r} makes the same change as {edits[edit]!r}")
        edits.setdefault(edit, m["name"])
    return problems


def compile_commands(build_dir):
    """compile_commands.json as [(directory, args, source)], or None without one."""
    path = ROOT / build_dir / "compile_commands.json"
    if not path.exists():
        return None
    out = []
    for e in json.loads(path.read_text(encoding="utf-8")):
        args = e.get("arguments") or shlex.split(e["command"])
        out.append((e["directory"], args, str(Path(e["directory"], e["file"]).resolve())))
    return out


def without_output(args):
    """A compile command with its -c, -o and dependency-file options removed."""
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
        elif a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif a in ("-c", "-MD", "-MMD") or a.startswith("-o"):
            pass
        else:
            out.append(a)
    return out


def includers(commands):
    """Each repository file mapped to the translation units that include it."""
    users = {}
    for directory, args, source in commands:
        users.setdefault(source, set()).add(source)
        r = subprocess.run(without_output(args) + ["-MM", "-MG"], cwd=directory,
                           capture_output=True, text=True)
        deps = r.stdout.replace("\\\n", " ").split(":", 1)[-1].split()
        for d in deps:
            users.setdefault(str(Path(directory, d).resolve()), set()).add(source)
    return users


def check(mutations, build_dir, config):
    """Check each mutation without building: its pattern matches as often as it
    says, its tests exist, and, given compile_commands.json, every translation
    unit that sees the change still compiles with the build's own flags."""
    problems = list_problems(mutations)
    known = test_targets(build_dir, config)
    if not known:
        print(f"note: {build_dir} is not configured; test names are not checked")
    commands = compile_commands(build_dir)
    users = None
    if commands is None:
        print(f"note: {build_dir} has no compile_commands.json; mutants are not compiled")
    else:
        users = includers(commands)
        by_source = {source: (directory, args) for directory, args, source in commands}

    for m in mutations:
        path = ROOT / m["file"]
        if not path.exists():
            problems.append(f"{m['name']!r}: {m['file']} does not exist")
            continue
        original = path.read_text(encoding="utf-8")
        count = original.count(m["find"])
        if count != m.get("count", 1):
            problems.append(f"{m['name']!r}: pattern matches {count} times in "
                            f"{m['file']}, expected {m.get('count', 1)}")
            continue
        if known:
            for name in m["caught_by"]:
                if name not in known:
                    problems.append(f"{m['name']!r}: no test is named {name!r}")
        if users is None or path.suffix == ".opcodes":
            continue
        units = sorted(users.get(str(path.resolve()), ()))
        if not units:
            problems.append(f"{m['name']!r}: no translation unit includes {m['file']}")
            continue
        raw = path.read_bytes()
        try:
            path.write_text(original.replace(m["find"], m["into"]), encoding="utf-8")
            for unit in units:
                directory, args = by_source[unit]
                r = subprocess.run(without_output(args) + ["-fsyntax-only"], cwd=directory,
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    first = next((l for l in r.stderr.splitlines() if "error" in l), r.stderr[:200])
                    problems.append(f"{m['name']!r} does not compile in "
                                    f"{Path(unit).relative_to(ROOT)}: {first.strip()[:200]}")
                    break
        finally:
            path.write_bytes(raw)
            os.utime(path, None)

    for line in problems:
        print(f"FAIL  {line}")
    print(f"\n{len(mutations)} mutations checked, {len(problems)} problems")
    return 1 if problems else 0


SYNC_MANIFEST = ".mutation_sync.json"


def sync_tree(src, dst):
    """Mirror the source tree at src into dst, for mutating away from it.

    Everything is copied, ignored fixtures included, except .git, out/, any
    CMakeFiles/ and any directory below the top holding a CMakeCache.txt: build
    trees, which dst keeps its own of. A copied file gets the current time, not
    its source's, so a file reverted to older contents still rebuilds. Only
    files an earlier sync copied are ever deleted from dst.
    """
    src, dst = src.resolve(), dst.resolve()
    manifest_path = dst / SYNC_MANIFEST
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        manifest = {}
    seen = {}
    copied = 0
    for here, dirs, files in os.walk(src):
        here = Path(here)
        top = here == src
        keep = []
        for d in dirs:
            full = here / d
            if full == dst or d in ("CMakeFiles", "__pycache__"):
                continue
            if top and d in (".git", "out"):
                continue
            if (full / "CMakeCache.txt").exists():
                continue
            keep.append(d)
        dirs[:] = keep
        for f in files:
            if top and f in ("CMakeCache.txt", SYNC_MANIFEST):
                continue
            full = here / f
            rel = full.relative_to(src).as_posix()
            st = full.stat()
            stamp = [st.st_size, st.st_mtime_ns]
            seen[rel] = stamp
            target = dst / rel
            if manifest.get(rel) == stamp and target.exists():
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(full, target)
            os.utime(target, None)
            copied += 1
    removed = 0
    for rel in manifest:
        if rel not in seen:
            (dst / rel).unlink(missing_ok=True)
            removed += 1
    manifest_path.write_text(json.dumps(seen), encoding="utf-8")
    return copied, removed


def main():
    global ROOT
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
    ap.add_argument("--check", action="store_true",
                    help="check the selected mutations without building, then stop")
    ap.add_argument("--shard", metavar="I/N",
                    help="of the selected mutations, take every Nth from the Ith")
    ap.add_argument("--workdir", metavar="DIR",
                    help="mirror this tree into DIR and mutate there; --build-dir is then inside DIR")
    args = ap.parse_args()

    shard = None
    if args.shard:
        try:
            shard = tuple(int(v) for v in args.shard.split("/"))
        except ValueError:
            shard = ()
        if len(shard) != 2 or not 1 <= shard[0] <= shard[1]:
            ap.error(f"--shard takes I/N with 1 <= I <= N, not {args.shard!r}")

    problems = list_problems(MUTATIONS)
    if problems:
        for line in problems:
            print(f"FAIL  {line}")
        return 2

    changed = changed_files() if args.changed else None
    selected = [m for m in MUTATIONS
                if (not args.only or any(o in m["name"] for o in args.only))
                and (changed is None or m["file"] in changed)]
    if shard:
        selected = selected[shard[0] - 1::shard[1]]

    if args.workdir:
        here, ROOT = ROOT, Path(args.workdir).resolve()
        ROOT.mkdir(parents=True, exist_ok=True)
        copied, removed = sync_tree(here, ROOT)
        print(f"{ROOT}: {copied} files copied, {removed} removed")
        if not (ROOT / args.build_dir / "CMakeCache.txt").exists():
            print(f"{ROOT / args.build_dir} is not configured. Configure it once, as for this "
                  f"checkout (on Windows, .\\build.ps1 -Tests run in {ROOT}), then run this again.")
            return 2

    if args.check:
        return check(selected, args.build_dir, args.config)

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
