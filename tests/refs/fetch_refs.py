#!/usr/bin/env python3
"""Fetch the reference documents the conformance tests are written against.

Turns the documents into greppable text with page markers, so a test comment
can cite a page.

    python tests/refs/fetch_refs.py

Nothing fetched here is committed; the datasheets are Western Design Center's
and the tutorials Bruce Clark's.

Text extraction needs pypdf (pip install pypdf); without it only the .txt files
of the PDFs are skipped.
"""

import re
import sys
import urllib.parse
import urllib.request
from pathlib import Path

DEST = Path(__file__).resolve().parent / "downloads"

UA = {"User-Agent": "Mozilla/5.0 (x16-emulator test reference fetcher)"}

PDFS = {
    # Cited by the CPU conformance tests.
    "w65c816s.pdf": "https://www.westerndesigncenter.com/wdc/documentation/w65c816s.pdf",
    "w65c02s.pdf": "https://www.westerndesigncenter.com/wdc/documentation/w65c02s.pdf",
    # The VIA. Two of these sit at $9F00 and $9F10; the X16 uses them for the
    # PS/2 ports, the SD card select, the I2C bus and the RAM/ROM bank latches.
    "w65c22s.pdf": "https://www.westerndesigncenter.com/wdc/documentation/w65c22s.pdf",
}

PAGES = {
    "6502org-65c816-opcodes.txt": "http://6502.org/tutorials/65c816opcodes.html",
    "6502org-65c02-opcodes.txt": "http://6502.org/tutorials/65c02opcodes.html",
}

# The X16's own documentation: no hardware definition is published for the
# banking, so this is the source for the memory map. Pinned to a commit.
DOCS_COMMIT = "79a8303bc33cda4ec3e85f3239f9b3d4c8c76260"

DOCS = {
    "x16-memory-map.md": "X16 Reference - 08 - Memory Map.md",
    "x16-hardware.md": "X16 Reference - 14 - Hardware.md",
}


def fetch(url, path):
    if path.exists():
        print(f"  have {path.name}")
        return True
    print(f"  get  {path.name}")
    try:
        req = urllib.request.Request(url, headers=UA)
        with urllib.request.urlopen(req, timeout=120) as r:
            path.write_bytes(r.read())
        return True
    except Exception as exc:
        print(f"  FAIL {path.name}: {exc}")
        return False


def html_to_text(path):
    raw = path.read_bytes().decode("utf-8", "replace")
    raw = re.sub(r"(?is)<(script|style).*?</\1>", "", raw)
    raw = re.sub(r"(?i)<br\s*/?>", "\n", raw)
    raw = re.sub(r"(?i)</(p|div|tr|h\d|li)>", "\n", raw)
    text = re.sub(r"(?s)<[^>]+>", "", raw)
    for a, b in (("&lt;", "<"), ("&gt;", ">"), ("&amp;", "&"), ("&nbsp;", " ")):
        text = text.replace(a, b)
    return re.sub(r"\n{3,}", "\n\n", text)


def pdf_to_text(pdf, out):
    try:
        import pypdf
    except ImportError:
        print(f"  skip {out.name} (pip install pypdf to enable)")
        return
    reader = pypdf.PdfReader(str(pdf))
    with out.open("w", encoding="utf-8") as fh:
        for i, page in enumerate(reader.pages, 1):
            # The page marker lets a citation be found again.
            fh.write(f"\n===== page {i} =====\n")
            fh.write(page.extract_text() or "")
    print(f"  made {out.name} ({len(reader.pages)} pages)")


def main():
    DEST.mkdir(parents=True, exist_ok=True)
    ok = True

    print("WDC datasheets:")
    for name, url in PDFS.items():
        path = DEST / name
        if fetch(url, path):
            pdf_to_text(path, path.with_suffix(".txt"))
        else:
            ok = False

    print("6502.org tutorials:")
    for name, url in PAGES.items():
        raw = DEST / (name + ".html")
        if fetch(url, raw):
            (DEST / name).write_text(html_to_text(raw), encoding="utf-8")
            raw.unlink()
            print(f"  made {name}")
        else:
            ok = False

    print("X16 documentation:")
    base = f"https://raw.githubusercontent.com/X16Community/x16-docs/{DOCS_COMMIT}"
    for name, path in DOCS.items():
        quoted = urllib.parse.quote(path)
        if not fetch(f"{base}/{quoted}", DEST / name):
            ok = False

    print(f"\nreferences in {DEST}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
