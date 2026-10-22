#!/usr/bin/env python3
"""Find DCLF's corrupt frames in a run's screen captures and show what the frame trace logged for them.

A run with CS_DCLF_FRAME_TRACE=1 logs, every frame, what it adopted, its epoch coverage, its main epochs' outcomes, the colour
epoch's culling counts and the retirement point it bound ("[DCLF] frame trace: ..."; F10 logs a user marker).
tools/dclf-capture-burst.ps1 captures the screen continuously alongside it, each capture named by its local time
(HH-mm-ss.fff.jpg), the clock the log's timestamps use. A corrupt frame is a single-capture outlier: unlike both its neighbours,
while they are like each other. This script

  1. scores every capture on 160x90 grayscale thumbnails: min(diff(prev, cur), diff(cur, next)) - diff(prev, next), the mean
     absolute difference of each pair, and reports the top N above a threshold;
  2. for each, prints the frame trace lines logged in [capture time - 250 ms, capture time], marking with '>>' those that are not
     the steady state: an epoch not submitted (withdrawn, not ready, FAILED, NOT COMMITTED...), a coverage with an epoch uncovered,
     a binding BOUND AT A RETIRED POINT or whose POINT WENT BACK, a USER MARK;
  3. with --sheets DIR, writes a prev | cur | next contact sheet per outlier.

With --key magenta (a run with CS_DCLF_TREE_LOD_TINT=1: DCLF's tree LOD draws solid magenta), only the key colour's pixels are scored,
which holds while the camera moves: a capture's score is its key pixels with none within --radius in either neighbour (appeared),
plus the key pixels both neighbours have near each other with none near in it (vanished). The sheets mark them green and red.

    python tools/dclf-corrupt-frames.py C:/runs/burst CommunityShaders.log
    python tools/dclf-corrupt-frames.py C:/runs/burst CommunityShaders.log --top 10 --threshold 3 --sheets C:/runs/sheets

The captures carry no date: a log that spans midnight is matched by time of day alone. Requires numpy and Pillow.
"""
import argparse
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

CAPTURE_NAME = re.compile(r"^(\d\d)-(\d\d)-(\d\d)\.(\d{3})$")
LOG_TIME = re.compile(r"^\[(?:\d{4}-\d\d-\d\d )?(\d\d):(\d\d):(\d\d)\.(\d{3})\]")  # CommunityShaders.log: [HH:MM:SS.mmm], or with a date
TRACE = "[DCLF] frame trace: "
COVERAGE = re.compile(r"coverage ([01]+) ")
EPOCH = re.compile(r" epoch: (.*?) \(")
THUMB = (160, 90)
KEY_SIZE = (480, 270)


def ms_of_day(h, m, s, ms):
    return ((int(h) * 60 + int(m)) * 60 + int(s)) * 1000 + int(ms)


def load_captures(directory):
    captures = []
    for path in Path(directory).glob("*.jpg"):
        match = CAPTURE_NAME.match(path.stem)
        if match:
            captures.append((ms_of_day(*match.groups()), path))
    captures.sort()
    return captures


def thumbnail(path):
    with Image.open(path) as image:
        return np.asarray(image.convert("L").resize(THUMB, Image.BILINEAR), dtype=np.float32)


def diff(a, b):
    return float(np.mean(np.abs(a - b)))


def key_mask(path):
    """The key colour's pixels (magenta: red and blue well above green) at KEY_SIZE; None for an unreadable capture."""
    try:
        with Image.open(path) as image:
            rgb = np.asarray(image.convert("RGB").resize(KEY_SIZE, Image.BILINEAR), dtype=np.int16)
    except OSError:
        return None
    return (rgb[..., 0] - rgb[..., 1] > 60) & (rgb[..., 2] - rgb[..., 1] > 60)


def dilate(mask, radius):
    out = mask.copy()
    for axis in (0, 1):
        grown = out.copy()
        for step in range(1, radius + 1):
            grown |= np.roll(out, step, axis) | np.roll(out, -step, axis)
        out = grown
    return out


def key_changes(prev, cur, nxt, radius):
    """(appeared, vanished) masks of cur against its neighbours."""
    near_prev, near_cur, near_next = dilate(prev, radius), dilate(cur, radius), dilate(nxt, radius)
    return cur & ~near_prev & ~near_next, prev & near_next & ~near_cur


def key_outliers(captures, top, threshold, radius):
    """(score, index) of the captures whose key pixels appeared or vanished, best first."""
    masks = [key_mask(path) for _, path in captures]
    scored = []
    for i in range(1, len(masks) - 1):
        if masks[i - 1] is None or masks[i] is None or masks[i + 1] is None:
            continue
        appeared, vanished = key_changes(masks[i - 1], masks[i], masks[i + 1], radius)
        score = int(appeared.sum() + vanished.sum())
        if score > threshold:
            scored.append((score, i))
    scored.sort(reverse=True)
    return scored[:top]


def outliers(captures, top, threshold):
    """(score, index) of the single-capture outliers, best first."""
    thumbs = [thumbnail(path) for _, path in captures]
    scored = []
    for i in range(1, len(thumbs) - 1):
        prev, cur, nxt = thumbs[i - 1], thumbs[i], thumbs[i + 1]
        score = min(diff(prev, cur), diff(cur, nxt)) - diff(prev, nxt)
        if score > threshold:
            scored.append((score, i))
    scored.sort(reverse=True)
    return scored[:top]


def load_trace(log_path):
    """(ms of day, line) of every frame trace line, in log order."""
    lines = []
    with open(log_path, encoding="utf-8", errors="replace") as log:
        for line in log:
            if TRACE not in line:
                continue
            match = LOG_TIME.match(line)
            if match:
                lines.append((ms_of_day(*match.groups()), line.rstrip("\n")))
    return lines


def flagged(text):
    """Whether a frame trace line is not the steady state."""
    if "BOUND AT A RETIRED POINT" in text or "POINT WENT BACK" in text or "USER MARK" in text:
        return True
    coverage = COVERAGE.search(text)
    if coverage and "0" in coverage.group(1):
        return True
    epoch = EPOCH.search(text)
    return bool(epoch and epoch.group(1) != "submitted")


def clock(ms):
    return f"{ms // 3600000:02d}:{ms // 60000 % 60:02d}:{ms // 1000 % 60:02d}.{ms % 1000:03d}"


def contact_sheet(paths, labels, out):
    images = [Image.open(path).convert("RGB") for path in paths]
    width, height = images[0].size
    sheet = Image.new("RGB", (width * len(images), height + 20), "black")
    draw = ImageDraw.Draw(sheet)
    for k, (image, label) in enumerate(zip(images, labels)):
        sheet.paste(image.resize((width, height)), (k * width, 20))
        draw.text((k * width + 4, 4), label, fill="white")
        image.close()
    sheet.save(out, quality=90)


def key_sheet(paths, labels, radius, out):
    """contact_sheet's, with cur's appeared key pixels green and vanished ones red."""
    images = [Image.open(path).convert("RGB").resize(KEY_SIZE, Image.BILINEAR) for path in paths]
    masks = [key_mask(path) for path in paths]
    appeared, vanished = key_changes(*masks, radius)
    marked = np.asarray(images[1]).copy()
    marked[appeared] = (0, 255, 0)
    marked[vanished] = (255, 0, 0)
    images.append(Image.fromarray(marked))
    width, height = KEY_SIZE
    sheet = Image.new("RGB", (width * 4, height + 20), "black")
    draw = ImageDraw.Draw(sheet)
    for k, (image, label) in enumerate(zip(images, labels + ["changes: appeared green, vanished red"])):
        sheet.paste(image, (k * width, 20))
        draw.text((k * width + 4, 4), label, fill="white")
    sheet.save(out, quality=90)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("captures", help="the capture directory (dclf-capture-burst.ps1's -Out)")
    parser.add_argument("log", help="the run's CommunityShaders.log")
    parser.add_argument("--top", type=int, default=20, help="how many outliers to report (default 20)")
    parser.add_argument("--threshold", type=float, default=2.0, help="the least outlier score reported, in mean grey levels (default 2)")
    parser.add_argument("--window-ms", type=int, default=250, help="how far before a capture its frame trace lines are taken from (default 250)")
    parser.add_argument("--flagged-only", action="store_true", help="print only the flagged lines of each window")
    parser.add_argument("--sheets", metavar="DIR", help="write a prev | cur | next contact sheet per outlier into DIR")
    parser.add_argument("--key", choices=["magenta"], help="score only the key colour's pixels (CS_DCLF_TREE_LOD_TINT=1's tree LOD); "
                        "the threshold is then in pixels at 480x270")
    parser.add_argument("--radius", type=int, default=4, help="--key: how far (pixels at 480x270) a key pixel may move between captures (default 4)")
    args = parser.parse_args()

    captures = load_captures(args.captures)
    if len(captures) < 3:
        sys.exit(f"{args.captures}: fewer than three captures named HH-mm-ss.fff.jpg")
    trace = load_trace(args.log)
    if not trace:
        print(f"{args.log}: no frame trace lines (was the run made with CS_DCLF_FRAME_TRACE=1?)", file=sys.stderr)
    times = np.array([t for t, _ in trace], dtype=np.int64)
    if args.sheets:
        Path(args.sheets).mkdir(parents=True, exist_ok=True)

    found = key_outliers(captures, args.top, args.threshold, args.radius) if args.key else outliers(captures, args.top, args.threshold)
    print(f"{len(captures)} captures ({clock(captures[0][0])} - {clock(captures[-1][0])}), {len(trace)} frame trace lines, "
          f"{len(found)} outliers above {args.threshold}")
    for rank, (score, i) in enumerate(found, 1):
        at, path = captures[i]
        first, last = np.searchsorted(times, at - args.window_ms, "left"), np.searchsorted(times, at, "right")
        window = trace[first:last]
        marks = sum(1 for _, text in window if flagged(text))
        print(f"\n#{rank} {path.name} score {score:.2f} ({len(window)} lines in [-{args.window_ms} ms, 0], {marks} flagged)")
        for t, text in window:
            if flagged(text):
                print(f"  >> {clock(t)} {text[text.index(TRACE) + len(TRACE):]}")
            elif not args.flagged_only:
                print(f"     {clock(t)} {text[text.index(TRACE) + len(TRACE):]}")
        if args.sheets:
            paths = [captures[i - 1][1], path, captures[i + 1][1]]
            if args.key:
                key_sheet(paths, [f"prev {paths[0].stem}", f"CUR {path.stem} ({score:.0f})", f"next {paths[2].stem}"], args.radius,
                          Path(args.sheets) / f"{rank:02d}_{path.stem}.jpg")
                continue
            contact_sheet(paths, [f"prev {paths[0].stem}", f"CUR {path.stem} ({score:.2f})", f"next {paths[2].stem}"],
                          Path(args.sheets) / f"{rank:02d}_{path.stem}.jpg")

    user_marks = [(t, text) for t, text in trace if "USER MARK" in text]
    if user_marks:
        print("\nuser marks (F10):")
        for t, text in user_marks:
            print(f"  {clock(t)} {text[text.index(TRACE) + len(TRACE):]}")


if __name__ == "__main__":
    main()
