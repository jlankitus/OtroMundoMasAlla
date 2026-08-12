#!/usr/bin/env python3
"""A minimal virtual terminal, so recorded frames can be replayed the way a real
terminal would see them.

WHY THIS IS THE LOAD-BEARING PIECE

tools/ansi_to_png.py parses one frame as if it were self-contained. A real
terminal is not: it keeps a cell buffer, and `\\033[H` followed by overwriting
means frame N inherits whatever frame N-1 left in any cell frame N does not
touch.

Every stale-pixel bug lives precisely in that gap, and a stateless parser is
structurally incapable of seeing one. This keeps the buffer, so it can answer the
question that matters:

    "did this frame write every cell it is responsible for,
     or is something on screen left over from two frames ago?"

Scope is deliberately small. This is not a terminal emulator; it understands only
the escapes omma-ascii emits, and asserts loudly on anything else rather than
guessing. A permissive parser here would hide the very bugs it exists to find.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

SGR = re.compile(r"\x1b\[([0-9;]*)m")
CSI = re.compile(r"\x1b\[([0-9;]*)([A-Za-z])")

DEFAULT_FG = (204, 208, 214)
DEFAULT_BG = (12, 14, 18)

# The sixteen ANSI colours as Windows Terminal's Campbell scheme renders them.
ANSI16 = {
    30: (12, 12, 12),    31: (197, 15, 31),   32: (19, 161, 14),   33: (193, 156, 0),
    34: (0, 55, 218),    35: (136, 23, 152),  36: (58, 150, 221),  37: (204, 204, 204),
    90: (118, 118, 118), 91: (231, 72, 86),   92: (22, 198, 12),   93: (249, 241, 165),
    94: (59, 120, 255),  95: (180, 0, 158),   96: (97, 214, 214),  97: (242, 242, 242),
}

UPPER_HALF_BLOCK = "▀"


@dataclass
class Cell:
    glyph: str = " "
    fg: tuple = DEFAULT_FG
    bg: tuple = DEFAULT_BG

    def key(self):
        return (self.glyph, self.fg, self.bg)


@dataclass
class Terminal:
    columns: int
    rows: int
    cells: list = field(default_factory=list)
    written: list = field(default_factory=list)
    row: int = 0
    column: int = 0
    fg: tuple = DEFAULT_FG
    bg: tuple = DEFAULT_BG
    unknown_escapes: list = field(default_factory=list)
    overflow_writes: int = 0

    def __post_init__(self):
        self.cells = [[Cell() for _ in range(self.columns)] for _ in range(self.rows)]
        self.written = [[False] * self.columns for _ in range(self.rows)]

    # ── frame lifecycle ─────────────────────────────────────────────────────
    def begin_frame(self) -> None:
        """Clear the coverage map, NOT the cells.

        The cells persist on purpose: that is the whole point. A frame that fails
        to write a cell leaves the previous frame's content there, exactly as a
        real terminal would, and `unwritten_cells()` will say so.
        """
        for row in self.written:
            for i in range(len(row)):
                row[i] = False

    def unwritten_cells(self) -> list:
        return [(x, y)
                for y in range(self.rows)
                for x in range(self.columns)
                if not self.written[y][x]]

    def snapshot(self) -> list:
        return [[cell.key() for cell in row] for row in self.cells]

    # ── parsing ─────────────────────────────────────────────────────────────
    def feed(self, text: str) -> None:
        i = 0
        while i < len(text):
            ch = text[i]

            if ch == "\x1b":
                match = CSI.match(text, i)
                if not match:
                    self.unknown_escapes.append(repr(text[i:i + 8]))
                    i += 1
                    continue
                params, final = match.group(1), match.group(2)
                self._apply_csi(params, final, match.group(0))
                i = match.end()
                continue

            if ch == "\r":
                self.column = 0
            elif ch == "\n":
                self.row += 1
                # omma-ascii always pairs \n with \r, but a bare \n in a raw-mode
                # terminal does NOT return the carriage. Modelling that faithfully
                # is how a missing \r would show up as a staircase.
            else:
                self._put(ch)
            i += 1

    def _apply_csi(self, params: str, final: str, raw: str) -> None:
        if final == "m":
            self._apply_sgr(params)
        elif final == "H":
            self.row = 0
            self.column = 0
        elif final == "K":
            # Erase from the cursor to the end of the line, using the CURRENT
            # background — which is why a missing reset before this shows up as a
            # coloured streak to the right edge.
            for x in range(self.column, self.columns):
                self.cells[self.row][x] = Cell(" ", self.fg, self.bg)
                self.written[self.row][x] = True
        elif final == "J":
            start_row = self.row if params in ("", "0") else 0
            for y in range(start_row, self.rows):
                for x in range(self.columns):
                    self.cells[y][x] = Cell(" ", self.fg, self.bg)
                    self.written[y][x] = True
        else:
            self.unknown_escapes.append(repr(raw))

    def _apply_sgr(self, params: str) -> None:
        values = [int(p) for p in params.split(";") if p != ""] or [0]
        i = 0
        while i < len(values):
            v = values[i]
            if v == 0:
                self.fg, self.bg = DEFAULT_FG, DEFAULT_BG
            elif v == 38 and i + 4 < len(values) and values[i + 1] == 2:
                self.fg = (values[i + 2], values[i + 3], values[i + 4])
                i += 4
            elif v == 48 and i + 4 < len(values) and values[i + 1] == 2:
                self.bg = (values[i + 2], values[i + 3], values[i + 4])
                i += 4
            elif v in ANSI16:
                self.fg = ANSI16[v]
            elif v - 10 in ANSI16:
                self.bg = ANSI16[v - 10]
            elif v in (1, 2, 22):
                pass                      # bold/dim: ignored, we use real colours
            else:
                self.unknown_escapes.append(f"SGR {v}")
            i += 1

    def _put(self, glyph: str) -> None:
        if 0 <= self.row < self.rows and 0 <= self.column < self.columns:
            self.cells[self.row][self.column] = Cell(glyph, self.fg, self.bg)
            self.written[self.row][self.column] = True
        else:
            self.overflow_writes += 1
        self.column += 1

    # ── rasterising ─────────────────────────────────────────────────────────
    def to_pixels(self) -> list:
        """Cells -> a pixel grid, two rows per cell, matching the half-block encoding.

        Text cells are rendered as their foreground over the background, so HUD
        text shows as a bright smear. That is enough to tell present from absent
        and legible from invisible; it is not a font rasteriser and does not need
        to be.
        """
        pixels = [[DEFAULT_BG] * self.columns for _ in range(self.rows * 2)]
        for y, row in enumerate(self.cells):
            for x, cell in enumerate(row):
                if cell.glyph == UPPER_HALF_BLOCK:
                    pixels[y * 2][x] = cell.fg
                    pixels[y * 2 + 1][x] = cell.bg
                elif cell.glyph == " ":
                    pixels[y * 2][x] = cell.bg
                    pixels[y * 2 + 1][x] = cell.bg
                else:
                    pixels[y * 2][x] = cell.fg
                    pixels[y * 2 + 1][x] = tuple(c // 3 for c in cell.fg)
        return pixels
