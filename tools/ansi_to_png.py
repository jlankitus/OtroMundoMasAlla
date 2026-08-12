#!/usr/bin/env python3
"""Render omma-ascii's terminal output to a PNG, so it can actually be looked at.

This parses the real output stream — the same bytes the terminal receives —
back into pixels, which makes it an end-to-end check of the presentation path
rather than a re-derivation of it.

One parser, two front-ends: the parsing lives in vterm.Terminal, the same
virtual terminal the temporal harness replays recordings through. This file
only sizes the terminal and rasterises its cells to a PNG.

Usage:
    omma-ascii --snapshot --colour truecolour --size 120x36 | \
        python tools/ansi_to_png.py out.png --scale 6
"""

from __future__ import annotations

import argparse
import re
import sys

from PIL import Image

from vterm import Terminal

ANY_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def geometry(text: str) -> tuple[int, int]:
    """Cell columns and rows of a frame, measured from its visible characters."""
    lines = ANY_ESCAPE.sub("", text).split("\n")
    while lines and lines[-1].strip("\r") == "":
        lines.pop()
    columns = max((len(line.rstrip("\r")) for line in lines), default=0)
    return columns, len(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="PNG path to write")
    parser.add_argument("--scale", type=int, default=6)
    parser.add_argument("--input", help="read from a file instead of stdin")
    args = parser.parse_args()

    if args.input:
        with open(args.input, "r", encoding="utf-8", errors="replace") as handle:
            text = handle.read()
    else:
        text = sys.stdin.buffer.read().decode("utf-8", errors="replace")

    columns, rows = geometry(text)
    if columns == 0:
        print("no cells parsed -- was the input empty?", file=sys.stderr)
        return 1

    terminal = Terminal(columns, rows)
    terminal.feed(text)
    if terminal.unknown_escapes:
        print(f"warning: {len(terminal.unknown_escapes)} unrecognised escapes, "
              f"first {terminal.unknown_escapes[0]}", file=sys.stderr)

    pixels = terminal.to_pixels()
    image = Image.new("RGB", (columns, rows * 2))
    for y, row in enumerate(pixels):
        for x, colour in enumerate(row):
            image.putpixel((x, y), colour)

    if args.scale > 1:
        image = image.resize((columns * args.scale, rows * 2 * args.scale),
                             Image.NEAREST)
    image.save(args.output)
    print(f"{args.output}: {columns}x{rows} cells "
          f"({columns}x{rows * 2} pixels), scale {args.scale}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
