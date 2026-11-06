#!/usr/bin/env python3
"""Compares cs-ab.ps1 runs: kernel memory at each mark, and who holds it from each run's VRAM dumps.

    compare.py <run folder> <run folder> ...        One table per run set, columns in the order given.
    compare.py --owners <mark> <run folder> ...     Largest owners at that mark, side by side.
    compare.py --peaks <run folder> ...             Peak of the once-a-second rows, overall and per stretch.

A run folder is <Root>/runs/<id>. Kernel numbers come from CS-AB-memory.csv (the same counter in every build); owner
numbers come from the dump written just after a mark (the nth dump belongs to the nth mark that has one).
"""
import csv
import glob
import json
import os
import re
import sys
from collections import Counter, OrderedDict

MIB = 1024 * 1024


def read_rows(folder):
    with open(os.path.join(folder, "CS-AB-memory.csv"), newline="", encoding="utf-8", errors="replace") as f:
        return [
            {**r, **{k: int(r[k]) for k in ("epochMs", "frame", "localMiB", "budgetMiB", "nonLocalMiB", "privateMiB", "workingSetMiB")}}
            for r in csv.DictReader(f)
        ]


def marks(folder):
    result = OrderedDict()
    for row in read_rows(folder):
        if row["label"]:
            result[row["label"].split(" @")[0]] = row
    return result


def dumps(folder):
    """Each dump, by the mark just before it in time."""
    table = marks(folder)
    result = {}
    for path in sorted(glob.glob(os.path.join(folder, "CommunityShaders-VRAM-*.json"))):
        stamp = int(re.search(r"VRAM-(\d+)", path).group(1))
        best = None
        for name, row in table.items():
            if row["epochMs"] <= stamp + 2000 and (best is None or row["epochMs"] > table[best]["epochMs"]):
                best = name
        if best:
            with open(path, encoding="utf-8", errors="replace") as f:
                result[best] = json.load(f)
    return result


def owner_group(row):
    """A resource class that means the same in every build."""
    owner = row["owner"]
    parts = owner.split("/")
    if row["source"] == "ORG":
        return "ORG/" + parts[1] if len(parts) > 1 else "ORG"
    if row["source"] == "DXVK":
        return "DXVK internal"
    if parts[0] == "Game" and len(parts) > 1:
        return "Game " + parts[1]
    if parts[0] == "CS" and len(parts) > 1:
        return "CS " + parts[1]
    return owner


def tally(dump, group=owner_group):
    totals = Counter()
    for row in dump["rows"]:
        if row["deviceLocal"] and not (row["flags"] & 2):
            totals[group(row)] += row["bytes"]
    return totals


def table_marks(folders):
    names = [os.path.basename(f.rstrip("\\/")) for f in folders]
    tables = [marks(f) for f in folders]
    order = []
    for t in tables:
        for k in t:
            if k not in order:
                order.append(k)
    print("mark".ljust(16) + "".join(n[-26:].rjust(28) for n in names))
    for key, title in (("localMiB", "VRAM use, kernel (MiB)"), ("nonLocalMiB", "Non-local GPU use (MiB)"), ("privateMiB", "Private bytes (MiB)")):
        print(f"-- {title}")
        for mark in order:
            print(mark.ljust(16) + "".join((str(t[mark][key]) if mark in t else "-").rjust(28) for t in tables))


def table_owners(mark, folders, top=45):
    names = [os.path.basename(f.rstrip("\\/")) for f in folders]
    totals = []
    for folder in folders:
        table = dumps(folder)
        totals.append(tally(table[mark]) if mark in table else Counter())
    keys = Counter()
    for t in totals:
        for k, v in t.items():
            keys[k] = max(keys[k], v)
    print(f"-- owners at {mark} (MiB)")
    print("owner".ljust(38) + "".join(n[-22:].rjust(24) for n in names))
    for k, _ in keys.most_common(top):
        print(k[:37].ljust(38) + "".join(f"{t[k] / MIB:.0f}".rjust(24) for t in totals))
    print("TOTAL listed".ljust(38) + "".join(f"{sum(t.values()) / MIB:.0f}".rjust(24) for t in totals))
    for folder, name in zip(folders, names):
        table = dumps(folder)
        if mark in table:
            for line in table[mark].get("reconciliation", []):
                if line["depth"] <= 2 and line["bytes"]:
                    print(f"  [{name[-22:]}] " + "  " * line["depth"] + f"{line['label']}: {line['bytes'] / MIB:.0f}")


def table_peaks(folders):
    print("run".ljust(50) + "peak VRAM".rjust(12) + "peak priv".rjust(12) + "peak nonlocal".rjust(14))
    for folder in folders:
        rows = read_rows(folder)
        print(os.path.basename(folder.rstrip("\\/"))[-49:].ljust(50) + str(max(r["localMiB"] for r in rows)).rjust(12) + str(max(r["privateMiB"] for r in rows)).rjust(12) + str(max(r["nonLocalMiB"] for r in rows)).rjust(14))


def main(argv):
    if argv and argv[0] == "--owners":
        table_owners(argv[1], argv[2:])
    elif argv and argv[0] == "--peaks":
        table_peaks(argv[1:])
    else:
        table_marks(argv)
        table_peaks(argv)


if __name__ == "__main__":
    main(sys.argv[1:])
