#!/usr/bin/env python3
"""Fetch the Debug Adapter Protocol schema that testbench/test_dap_conformance.py
validates against.

    python tests/fetch_dap_schema.py

Not committed: someone else's repository under its own licence. Pinned to a commit.
"""

import sys
import urllib.request
from pathlib import Path

DEST = Path(__file__).resolve().parent / "dap-schema"
COMMIT = "1b52c204d48b4fcc32b196611aa7f2c1ac0298d2"
URL = ("https://raw.githubusercontent.com/microsoft/debug-adapter-protocol/"
       f"{COMMIT}/debugAdapterProtocol.json")


def main():
    schema = DEST / "debugAdapterProtocol.json"
    stamp = DEST / "COMMIT"
    if schema.exists() and stamp.exists() and stamp.read_text().strip() == COMMIT:
        print(f"have {schema.name} at {COMMIT[:10]}")
        return 0

    DEST.mkdir(parents=True, exist_ok=True)
    try:
        with urllib.request.urlopen(URL, timeout=30) as r:
            data = r.read()
    except Exception as e:
        print(f"FAILED to fetch the DAP schema: {e}", file=sys.stderr)
        return 1
    schema.write_bytes(data)
    stamp.write_text(COMMIT + "\n")
    print(f"got {schema.name} at {COMMIT[:10]} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
