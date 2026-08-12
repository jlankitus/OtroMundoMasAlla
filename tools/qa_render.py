#!/usr/bin/env python3
"""Render QA: exercise every output mode and assert the stream is well formed.

WHAT THIS CATCHES THAT ctest DOES NOT
The unit tests check Canvas and Camera in isolation. They cannot check the thing
that actually broke: whether the BYTES the terminal receives are valid, complete
and self-consistent once a whole scene has been composed. A dropped colour
escape, an unterminated SGR, a half block emitted as three Latin-1 characters, a
row that is one cell short -- all of those pass every unit test and destroy the
display.

So this drives the real binary across the real option matrix and asserts
properties of the real output:

  * decodes as UTF-8 with no replacement characters
  * every line has exactly the requested cell count
  * every escape sequence is one we recognise and deliberately emit
  * SGR state is closed at end of frame (no colour leaking into the shell)
  * the frame contains actual scene content, not just a HUD on black
  * frame size stays within a sane budget for a 30 Hz redraw
  * ascii mode emits no escapes at all
  * full-cell mode emits no multi-byte glyphs
  * every half block has an explicit foreground set since the last reset

Optionally writes a PNG per case so the frames can be looked at.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

SGR = re.compile(r"\x1b\[([0-9;]*)m")

# A foreground-setting escape: 24-bit, or one of the sixteen ANSI colours.
FG_ESCAPE = re.compile(r"\x1b\[(?:38;2;\d+;\d+;\d+|3[0-7]|9[0-7])m")
RESET = "\x1b[0m"
ANY_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
ERASE_LINE = "\x1b[K"
CURSOR_HOME = "\x1b[H"
HALF_BLOCK = "▀"

# Every escape this renderer is allowed to produce. Anything else is a bug,
# including something harmless-looking: an escape we did not mean to emit means
# a byte went somewhere unintended.
ALLOWED_ESCAPES = re.compile(r"\x1b\[(?:[0-9;]*m|H|K|0J)")


class Case:
    def __init__(self, name: str, args: list[str], columns: int, rows: int, **checks):
        self.name = name
        self.args = args
        self.columns = columns
        self.rows = rows
        self.checks = checks


def strip_escapes(text: str) -> str:
    return ALLOWED_ESCAPES.sub("", text)


def check(case: Case, raw: bytes) -> list[str]:
    problems: list[str] = []

    # ── 1. valid UTF-8 ──────────────────────────────────────────────────────
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        return [f"output is not valid UTF-8: {exc}"]
    if "�" in text:
        problems.append("output contains U+FFFD replacement characters")

    # ── 2. only escapes we deliberately emit ────────────────────────────────
    for match in re.finditer(r"\x1b\[[^a-zA-Z]*[a-zA-Z]", text):
        if not ALLOWED_ESCAPES.fullmatch(match.group(0)):
            problems.append(f"unexpected escape sequence {match.group(0)!r}")
            break

    # ── 3. geometry ─────────────────────────────────────────────────────────
    body = text.rstrip("\n")
    lines = body.split("\n")
    if len(lines) != case.rows:
        problems.append(f"expected {case.rows} rows, got {len(lines)}")

    for index, line in enumerate(lines):
        visible = strip_escapes(line)
        if len(visible) != case.columns:
            problems.append(
                f"row {index} has {len(visible)} cells, expected {case.columns}")
            break

    # ── 4. SGR state closed at the end ──────────────────────────────────────
    if case.checks.get("colour", True):
        if not body.rstrip().endswith("\x1b[0m"):
            problems.append("frame does not end with a reset; colour leaks to the shell")
    else:
        if "\x1b" in text:
            problems.append("ascii mode emitted escape sequences")

    # ── 5. glyph expectations ───────────────────────────────────────────────
    if case.checks.get("half_blocks") is True and HALF_BLOCK not in text:
        problems.append("half-block mode emitted no U+2580 at all")
    if case.checks.get("half_blocks") is False and HALF_BLOCK in text:
        problems.append("full-cell mode emitted U+2580, which needs UTF-8")

    # ── 6. there is actually a scene, not just a HUD ────────────────────────
    if case.checks.get("colour", True):
        distinct = set(SGR.findall(text))
        if len(distinct) < 8:
            problems.append(f"only {len(distinct)} distinct SGR params: scene looks empty")
    else:
        # In ascii mode, look for density-ramp characters away from the HUD.
        interior = "".join(lines[2:-3]) if len(lines) > 6 else body
        if not any(c in interior for c in ".:-=+*#%@"):
            problems.append("ascii mode produced no density-ramp glyphs")

    # ── 7. every half block has an explicit foreground ──────────────────────
    # A '▀' paints its top pixel with the terminal's CURRENT foreground. If no
    # foreground escape has been emitted since the last reset, that colour is the
    # terminal's DEFAULT -- a light grey -- and the block renders as a bright
    # artifact instead of the pixel it represents.
    #
    # This shipped. It produced grey blocks scattered along every orbit, moving
    # whenever the window was resized (because resizing changes which cells are
    # full-cell versus half-block). It is a property of the byte stream, so it is
    # checkable here and invisible to any unit test of Canvas in isolation.
    if case.checks.get("colour", True) and case.checks.get("half_blocks") is not False:
        fg_known = False
        orphans = 0
        i = 0
        while i < len(text):
            if text.startswith(RESET, i):
                fg_known = False
                i += len(RESET)
                continue
            match = FG_ESCAPE.match(text, i)
            if match:
                fg_known = True
                i = match.end()
                continue
            match = ANY_ESCAPE.match(text, i)
            if match:
                i = match.end()           # some other escape: skip it whole
                continue
            if text[i] == HALF_BLOCK and not fg_known:
                orphans += 1
            i += 1
        if orphans:
            problems.append(f"{orphans} half blocks with no foreground set "
                            f"(they render in the terminal default colour)")

    # ── 8. size budget ──────────────────────────────────────────────────────
    budget = case.checks.get("max_bytes")
    if budget and len(raw) > budget:
        problems.append(f"frame is {len(raw)} bytes, budget {budget}")

    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", help="path to omma-ascii")
    parser.add_argument("--png-dir", help="also write a PNG per case")
    args = parser.parse_args()

    W, H = 120, 36
    size = f"{W}x{H}"

    cases = [
        Case("truecolour / half blocks / dimetric",
             ["--colour", "truecolour", "--blocks", "half", "--tilt", "30", "--zoom", "4"],
             W, H, colour=True, half_blocks=True, max_bytes=60_000),
        Case("truecolour / full cells",
             ["--colour", "truecolour", "--blocks", "full", "--tilt", "30", "--zoom", "4"],
             W, H, colour=True, half_blocks=False, max_bytes=60_000),
        Case("ansi16 / half blocks",
             ["--colour", "ansi16", "--blocks", "half", "--tilt", "30", "--zoom", "4"],
             W, H, colour=True, half_blocks=True, max_bytes=40_000),
        Case("ascii density ramp",
             ["--colour", "ascii", "--tilt", "30", "--zoom", "4"],
             W, H, colour=False),
        Case("top-down (tilt 90)",
             ["--colour", "truecolour", "--tilt", "90", "--zoom", "4"],
             W, H, colour=True, half_blocks=True, max_bytes=60_000),
        Case("near edge-on (tilt 3)",
             ["--colour", "truecolour", "--tilt", "3", "--zoom", "4"],
             W, H, colour=True, half_blocks=True, max_bytes=60_000),
        Case("spun 137 degrees",
             ["--colour", "truecolour", "--tilt", "30", "--spin", "137", "--zoom", "4"],
             W, H, colour=True, half_blocks=True, max_bytes=60_000),
        Case("Earth-Moon, focused on Earth",
             ["--colour", "truecolour", "--tilt", "35", "--zoom", "2", "--focus", "Earth"],
             W, H, colour=True, half_blocks=True, max_bytes=60_000),
        Case("everything, focused on Pluto",
             ["--colour", "truecolour", "--tilt", "30", "--zoom", "6", "--focus", "Pluto"],
             W, H, colour=True, half_blocks=True, max_bytes=60_000),
        Case("tiny terminal 40x12",
             ["--colour", "truecolour", "--size", "40x12", "--zoom", "4"],
             40, 12, colour=True, max_bytes=20_000),
        Case("wide terminal 200x50",
             ["--colour", "truecolour", "--size", "200x50", "--zoom", "4"],
             200, 50, colour=True, half_blocks=True, max_bytes=160_000),
        Case("far future date",
             ["--colour", "truecolour", "--date", "2049-12-31", "--zoom", "4"],
             W, H, colour=True, half_blocks=True, max_bytes=60_000),
        Case("low orbit with a satellite fleet",
             ["--colour", "truecolour", "--tilt", "35", "--zoom", "1",
              "--focus", "Earth", "--launch", "4"],
             W, H, colour=True, half_blocks=True, max_bytes=90_000),
        Case("far past date",
             ["--colour", "truecolour", "--date", "1801-01-01", "--zoom", "4"],
             W, H, colour=True, half_blocks=True, max_bytes=60_000),
    ]

    png_dir = pathlib.Path(args.png_dir) if args.png_dir else None
    if png_dir:
        png_dir.mkdir(parents=True, exist_ok=True)

    failures = 0
    print(f"{'case':<38} {'bytes':>8}  result")
    print(f"{'-' * 38} {'-' * 8}  ------")

    for index, case in enumerate(cases):
        argv = [args.binary, "--snapshot"] + case.args
        if "--size" not in case.args:
            argv += ["--size", size]

        result = subprocess.run(argv, capture_output=True)
        if result.returncode != 0:
            print(f"{case.name:<38} {'-':>8}  EXIT {result.returncode}: "
                  f"{result.stderr.decode(errors='replace').strip()}")
            failures += 1
            continue

        problems = check(case, result.stdout)
        status = "ok" if not problems else "FAIL"
        print(f"{case.name:<38} {len(result.stdout):>8}  {status}")
        for problem in problems:
            print(f"    - {problem}")
        failures += bool(problems)

        if png_dir:
            slug = re.sub(r"[^a-z0-9]+", "_", case.name.lower()).strip("_")
            text_path = png_dir / f"{index:02d}_{slug}.txt"
            text_path.write_bytes(result.stdout)
            subprocess.run([sys.executable,
                            str(pathlib.Path(__file__).parent / "ansi_to_png.py"),
                            str(png_dir / f"{index:02d}_{slug}.png"),
                            "--scale", "6", "--input", str(text_path)],
                           capture_output=True)

    print()
    print(f"{len(cases) - failures}/{len(cases)} cases passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
