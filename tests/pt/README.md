# ProcessorTests fixtures

`wdc65c02.bin` and `816-emu.bin` are committed. They are the converted form of
SingleStepTests/ProcessorTests: 100 cases per opcode, taken as a prefix of each
opcode's file.

They are committed rather than downloaded because the conversion is 2.6 MB and
the JSON it comes from is 3.6 GB across the suites. Downloading that on every CI
run cost eight minutes and a cache dependency, and made the tests impossible to
run offline.

## Provenance

- 65C02: https://github.com/SingleStepTests/65x02 — `wdc65c02/v1`, MIT licensed,
  Copyright (c) 2024 Thomas Harte et al.
- 65816: https://github.com/SingleStepTests/65816 — `v1`

The cases are measurements of what the hardware does with a given starting
state. What is committed here is our own encoding of those measurements, in the
format documented in `pt_convert.py`, not a copy of the upstream files.

## Regenerating

    python tests/fetch_pt.py --force
    python tests/fetch_pt.py --force --variant 816-emu

The JSON lands in `tests/pt/<variant>/` and is not committed. Cases are taken as
a prefix rather than sampled, so any machine produces the same fixture and the
exact per-opcode counts in the baselines stay meaningful.

Deeper runs are a local matter: `--cases 10000` uses everything upstream has.
Do not commit the result, because a baseline built for one case count cannot be
compared against a run at another, and the test says so rather than guessing.
