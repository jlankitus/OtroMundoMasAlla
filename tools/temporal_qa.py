#!/usr/bin/env python3
"""Temporal QA: find the bugs that only exist between frames.

THE PROBLEM THIS SOLVES

Every check we had was on a single frame. Flicker, stale pixels, tearing and
frame pacing are all properties of a SEQUENCE, and none of them were testable —
worse, `--snapshot` bypasses the game loop entirely, so the code where those bugs
live had never been executed by any test at all.

HOW MOTION BECOMES INSPECTABLE WITHOUT WATCHING IT

You cannot review a video in a text log, and you cannot eyeball 300 frames. But
every temporal artifact projects into a SPATIAL pattern in a still image:

  space-time slice   take one screen row from every frame and stack them, so the
                     vertical axis is TIME. A stationary body is a vertical line;
                     a moving planet is a diagonal; a flickering pixel is a
                     DASHED vertical line; tearing is a horizontal break. This is
                     the highest-signal view by a wide margin, and it is the same
                     trick as a slit-scan photograph or an oscilloscope trace.

  change heatmap     how many times each cell changed across the run. In a paused
                     recording the answer must be zero everywhere.

  filmstrip          frames in a grid, for ordinary "does this look right".

THE TWO ASSERTIONS THAT DO THE REAL WORK

  paused frames are byte-identical
      Nothing is moving, the reported frame rate is pinned, so any difference at
      all is nondeterminism in the render path. This turns "does it flicker" into
      an exact comparison rather than a judgement call.

  every cell is written every frame
      omma-ascii redraws the whole grid, so no cell should ever be inherited from
      a previous frame. A cell the frame did not write is a stale pixel waiting to
      happen. Only checkable with a virtual terminal that keeps state across
      frames, which is what tools/vterm.py is for.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from vterm import Terminal  # noqa: E402


def record(binary: str, out_dir: pathlib.Path, frames: int, extra: list) -> None:
    if out_dir.exists():
        shutil.rmtree(out_dir)
    argv = [binary, "--record", str(frames), "--record-dir", str(out_dir)] + extra
    result = subprocess.run(argv, capture_output=True)
    if result.returncode != 0:
        raise SystemExit(f"record failed: {result.stderr.decode(errors='replace')}")


def load(out_dir: pathlib.Path) -> list:
    paths = sorted(out_dir.glob("frame_*.bin"))
    return [p.read_bytes() for p in paths]


def replay(raw_frames: list, columns: int, rows: int):
    """Feed every frame through one persistent terminal.

    Returns per-frame snapshots, per-frame pixel grids, and the problems found.
    """
    term = Terminal(columns=columns, rows=rows)
    snapshots, pixel_frames, problems = [], [], []

    for index, raw in enumerate(raw_frames):
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            problems.append(f"frame {index} is not valid UTF-8: {exc}")
            continue

        term.begin_frame()
        term.feed(text)

        missed = term.unwritten_cells()
        if missed:
            problems.append(
                f"frame {index} left {len(missed)} cells unwritten "
                f"(first at {missed[0]}) — those show the PREVIOUS frame's content")

        snapshots.append(term.snapshot())
        pixel_frames.append(term.to_pixels())

    if term.unknown_escapes:
        unique = sorted(set(term.unknown_escapes))[:6]
        problems.append(f"unrecognised escapes: {', '.join(unique)}")
    if term.overflow_writes:
        problems.append(f"{term.overflow_writes} writes landed outside the grid")

    return snapshots, pixel_frames, problems


def change_counts(snapshots: list, columns: int, rows: int):
    counts = [[0] * columns for _ in range(rows)]
    for previous, current in zip(snapshots, snapshots[1:]):
        for y in range(rows):
            for x in range(columns):
                if previous[y][x] != current[y][x]:
                    counts[y][x] += 1
    return counts


def save_heatmap(counts: list, path: pathlib.Path, scale: int = 5) -> int:
    rows, columns = len(counts), len(counts[0])
    peak = max((max(row) for row in counts), default=0)
    image = Image.new("RGB", (columns, rows), (0, 0, 0))
    if peak:
        for y in range(rows):
            for x in range(columns):
                t = counts[y][x] / peak
                # Black -> blue -> orange -> white. Anything not black changed.
                image.putpixel((x, y), (int(255 * min(1.0, t * 1.6)),
                                        int(200 * t * t),
                                        int(255 * max(0.0, 0.7 - t))))
    image.resize((columns * scale, rows * scale), Image.NEAREST).save(path)
    return peak


def save_spacetime(pixel_frames: list, pixel_row: int, path: pathlib.Path,
                   scale: int = 4) -> None:
    """One screen row from every frame, stacked. Vertical axis is TIME."""
    width = len(pixel_frames[0][0])
    image = Image.new("RGB", (width, len(pixel_frames)), (0, 0, 0))
    for t, pixels in enumerate(pixel_frames):
        row = pixels[min(pixel_row, len(pixels) - 1)]
        for x, colour in enumerate(row):
            image.putpixel((x, t), colour)
    image.resize((width * scale, len(pixel_frames) * scale), Image.NEAREST).save(path)


def save_filmstrip(pixel_frames: list, path: pathlib.Path, count: int = 6,
                   scale: int = 3) -> None:
    picks = [pixel_frames[i * (len(pixel_frames) - 1) // max(1, count - 1)]
             for i in range(min(count, len(pixel_frames)))]
    height, width = len(picks[0]), len(picks[0][0])
    sheet = Image.new("RGB", (width, height * len(picks)), (0, 0, 0))
    for n, pixels in enumerate(picks):
        for y, row in enumerate(pixels):
            for x, colour in enumerate(row):
                sheet.putpixel((x, n * height + y), colour)
    sheet.resize((width * scale, height * len(picks) * scale),
                 Image.NEAREST).save(path)


def busiest_cell_row(counts: list) -> int:
    return max(range(len(counts)), key=lambda y: sum(counts[y]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("--out", default="temporal_qa")
    parser.add_argument("--frames", type=int, default=90)
    parser.add_argument("--size", default="120x36")
    args = parser.parse_args()

    columns, rows = (int(v) for v in args.size.split("x"))
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    common = ["--size", args.size, "--colour", "truecolour", "--tilt", "30"]
    failures = 0

    # ── 1. paused: nothing moves, so nothing may change ─────────────────────
    print("[1] paused recording — frames must be byte-identical")
    paused_dir = out / "paused"
    record(args.binary, paused_dir, 30, common + ["--paused", "--zoom", "2"])
    paused = load(paused_dir)

    differing = [i for i in range(1, len(paused)) if paused[i] != paused[0]]
    if differing:
        print(f"    FAIL — {len(differing)} of {len(paused) - 1} frames differ "
              f"from the first (first at frame {differing[0]})")
        failures += 1
    else:
        print(f"    ok — all {len(paused)} frames identical ({len(paused[0])} bytes each)")

    snapshots, pixels, problems = replay(paused, columns, rows)
    for problem in problems:
        print(f"    FAIL — {problem}")
    failures += bool(problems)

    counts = change_counts(snapshots, columns, rows)
    peak = save_heatmap(counts, out / "paused_changes.png")
    if peak:
        print(f"    FAIL — a cell changed {peak} times while paused")
        failures += 1
    else:
        print("    ok — zero cell changes across the paused run")

    # ── 2. running with a scripted tour ─────────────────────────────────────
    # One key per frame. Zoom out, warp up, change focus, tilt, spin: the states
    # a user actually moves through, and the transitions where a renderer that
    # only ever redraws part of the screen falls apart.
    print("\n[2] scripted tour — coverage, and motion made visible")
    tour = ("4" + "." * 5 + "=" * 3 + "." * 10 +
            "\t" + "." * 10 + "t" * 4 + "." * 10 +
            "x" * 4 + "." * 10 + "\t" + "." * 10 + "o" + "." * 5 + "o" + "." * 10)
    tour_dir = out / "tour"
    record(args.binary, tour_dir, args.frames, common + ["--keys", tour])
    frames = load(tour_dir)
    print(f"    {len(frames)} frames, {sum(len(f) for f in frames) // 1024} KB total, "
          f"{sum(len(f) for f in frames) // max(1, len(frames))} bytes/frame")

    snapshots, pixels, problems = replay(frames, columns, rows)
    for problem in problems[:5]:
        print(f"    FAIL — {problem}")
    if problems:
        failures += 1
    else:
        print("    ok — every frame wrote every cell; no stale pixels possible")

    counts = change_counts(snapshots, columns, rows)
    save_heatmap(counts, out / "tour_changes.png")
    busy = busiest_cell_row(counts)
    save_spacetime(pixels, busy * 2, out / "tour_spacetime.png")
    save_filmstrip(pixels, out / "tour_filmstrip.png")
    print(f"    wrote tour_spacetime.png (cell row {busy}, the most active), "
          f"tour_changes.png, tour_filmstrip.png")

    # ── 3. determinism: the same script twice must be identical ─────────────
    print("\n[3] determinism — the same script must produce the same bytes")
    again_dir = out / "tour_again"
    record(args.binary, again_dir, args.frames, common + ["--keys", tour])
    if load(again_dir) == frames:
        print("    ok — byte-identical re-run")
    else:
        print("    FAIL — re-running the same script produced different frames")
        failures += 1

    print()
    print("FAILURES:" if failures else "all temporal checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
