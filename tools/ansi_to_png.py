#!/usr/bin/env python3
"""Render omma-ascii's terminal output to a PNG, so it can actually be looked at.

WHY THIS EXISTS
The renderer emits ANSI escape sequences and half-block glyphs. Verifying it by
eye requires a terminal, a human, and a screenshot -- none of which are
available to a test runner, and the first two of which are not available to
whoever is reviewing a pull request at 2am.

This parses the real output stream -- the same bytes the terminal receives --
back into pixels. That makes it a genuine end-to-end check of the presentation
path, not a re-derivation of it: if the colour-run compression drops an escape,
or a half block is emitted with its halves swapped, the image is wrong in a way
you can see.

Usage:
    omma-ascii --snapshot --colour truecolour --size 120x36 | \
        python tools/ansi_to_png.py out.png --scale 6
"""

from __future__ import annotations

import argparse
import re
import sys

from PIL import Image, ImageDraw

# SGR sequences we care about, plus the ones we must skip without choking.
SGR = re.compile(r"\x1b\[([0-9;]*)m")
OTHER_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-LN-Za-ln-z]")

UPPER_HALF_BLOCK = "▀"

# The 16 ANSI colours, as Windows Terminal's default (Campbell) scheme renders
# them. Only used when the input was produced with --colour ansi16.
ANSI16 = {
    30: (12, 12, 12),    31: (197, 15, 31),   32: (19, 161, 14),   33: (193, 156, 0),
    34: (0, 55, 218),    35: (136, 23, 152),  36: (58, 150, 221),  37: (204, 204, 204),
    90: (118, 118, 118), 91: (231, 72, 86),   92: (22, 198, 12),   93: (249, 241, 165),
    94: (59, 120, 255),  95: (180, 0, 158),   96: (97, 214, 214),  97: (242, 242, 242),
}

DEFAULT_FG = (204, 204, 204)
DEFAULT_BG = (12, 12, 12)


class CellGrid:
    """Parsed terminal output: one entry per character cell."""

    def __init__(self) -> None:
        self.rows: list[list[tuple[str, tuple[int, int, int], tuple[int, int, int]]]] = [[]]

    def add(self, glyph, fg, bg) -> None:
        self.rows[-1].append((glyph, fg, bg))

    def newline(self) -> None:
        self.rows.append([])

    @property
    def width(self) -> int:
        return max((len(r) for r in self.rows), default=0)

    @property
    def height(self) -> int:
        return len(self.rows)


def parse(text: str) -> CellGrid:
    grid = CellGrid()
    fg = DEFAULT_FG
    bg = DEFAULT_BG
    i = 0

    while i < len(text):
        if text[i] == "\x1b":
            match = SGR.match(text, i)
            if match:
                params = [int(p) for p in match.group(1).split(";") if p != ""] or [0]
                j = 0
                while j < len(params):
                    p = params[j]
                    if p == 0:
                        fg, bg = DEFAULT_FG, DEFAULT_BG
                    elif p == 38 and j + 4 < len(params) and params[j + 1] == 2:
                        fg = (params[j + 2], params[j + 3], params[j + 4])
                        j += 4
                    elif p == 48 and j + 4 < len(params) and params[j + 1] == 2:
                        bg = (params[j + 2], params[j + 3], params[j + 4])
                        j += 4
                    elif p in ANSI16:
                        fg = ANSI16[p]
                    elif p - 10 in ANSI16:
                        bg = ANSI16[p - 10]
                    j += 1
                i = match.end()
                continue

            other = OTHER_ESCAPE.match(text, i)
            if other:
                i = other.end()          # cursor moves, erases: ignored
                continue
            i += 1
            continue

        ch = text[i]
        if ch == "\n":
            grid.newline()
        elif ch == "\r":
            pass
        else:
            grid.add(ch, fg, bg)
        i += 1

    return grid


def render(grid: CellGrid, scale: int) -> Image.Image:
    # Each character cell becomes 1 pixel wide and 2 tall, matching the
    # half-block encoding, then everything is scaled up with nearest-neighbour
    # so individual pixels stay square and crisp.
    width = max(1, grid.width)
    height = max(1, grid.height * 2)
    image = Image.new("RGB", (width, height), DEFAULT_BG)
    draw = ImageDraw.Draw(image)

    for row_index, row in enumerate(grid.rows):
        top = row_index * 2
        for column, (glyph, fg, bg) in enumerate(row):
            if glyph == UPPER_HALF_BLOCK:
                draw.point((column, top), fill=fg)
                draw.point((column, top + 1), fill=bg)
            elif glyph == " ":
                draw.point((column, top), fill=bg)
                draw.point((column, top + 1), fill=bg)
            else:
                # A text cell. Approximate it as the foreground colour, dimmed,
                # so HUD text reads as a legible smear rather than vanishing.
                # This tool cannot rasterise a font, and does not need to: what
                # matters is whether the text is present and contrasts.
                draw.point((column, top), fill=fg)
                draw.point((column, top + 1),
                           fill=tuple(c // 3 for c in fg))

    if scale > 1:
        image = image.resize((width * scale, height * scale), Image.NEAREST)
    return image


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

    grid = parse(text)
    if grid.width == 0:
        print("no cells parsed -- was the input empty?", file=sys.stderr)
        return 1

    render(grid, args.scale).save(args.output)
    print(f"{args.output}: {grid.width}x{grid.height} cells "
          f"({grid.width}x{grid.height * 2} pixels), scale {args.scale}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
