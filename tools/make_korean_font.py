#!/usr/bin/env python3
"""Reproduce the bundled static Noto Sans KR Regular (fonttools==4.61.1).

Usage: python3 tools/make_korean_font.py [--source /path/to/NotoSansKR[wght].ttf]
The optional local source is checked against the same SHA-256 as the download.
"""
import argparse
import hashlib
import io
from pathlib import Path
import urllib.request

import fontTools
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont

REV = "c30b221beb0c1da59d563feb91ec8656a1fa972a"
URL = f"https://raw.githubusercontent.com/google/fonts/{REV}/ofl/notosanskr/NotoSansKR%5Bwght%5D.ttf"
SHA256 = "194018e6b2b293a7964f037b25c0249ce1418bc9ab3c971060a03aa57861e252"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    args = parser.parse_args()
    if fontTools.__version__ != "4.61.1":
        parser.error("Use fonttools==4.61.1 for reproducible output")
    if args.source:
        data = args.source.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=60) as response:
            data = response.read()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError("Noto Sans KR source SHA-256 mismatch")
    font = TTFont(io.BytesIO(data), recalcTimestamp=False)
    font = instantiateVariableFont(font, {"wght": 400}, inplace=True)
    cmap = font.getBestCmap()
    required = set(range(0xAC00, 0xD7A4)) | set(range(0x3131, 0x3164))
    if not required.issubset(cmap):
        raise ValueError("Source font does not cover all modern Hangul syllables/jamo")
    root = Path(__file__).resolve().parents[1]
    for relative in ("data/fonts", "portmaster/ytc/data/fonts"):
        out = root / relative / "NotoSansKR-Regular.ttf"
        out.parent.mkdir(parents=True, exist_ok=True)
        font.save(out)
        print(out.relative_to(root), hashlib.sha256(out.read_bytes()).hexdigest())

if __name__ == "__main__":
    main()
