#!/usr/bin/env python3
"""Compare the graph schedules two CommunityShaders.log files recorded (CS_DCLF_SCHEDULE_DUMP=1).

Each epoch's last logged pass list ("[DCLF schedule] epoch N: name/type/queue | ...") must match exactly, as must the frame's
epoch order and async lists ("[ORG] epoch order", "[ORG] async epochs"). The passes' C++ types ("[DCLF schedule types]") differing
is only a warning: ORG names a recording's recipe by them, which changes no GPU work.

    python tools/dclf-schedule-diff.py baseline.log current.log
Exit status 0: the same schedule; 1: a difference; 2: a log without a dump.
"""

import re
import sys

PASSES = re.compile(r"\[DCLF schedule\] epoch (\d+): (.*)$")
TYPES = re.compile(r"\[DCLF schedule types\] epoch (\d+): (.*)$")
ORDER = re.compile(r"\[ORG\] (epoch order|async epochs): (.*)$")


def read(path):
    passes, types, order = {}, {}, {}
    with open(path, encoding="utf-8", errors="replace") as log:
        for line in log:
            line = line.rstrip("\r\n")
            if m := PASSES.search(line):
                passes[int(m.group(1))] = m.group(2).split(" | ")
            elif m := TYPES.search(line):
                types[int(m.group(1))] = m.group(2).split(" | ")
            elif m := ORDER.search(line):
                order[m.group(1)] = m.group(2)
    return passes, types, order


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    base, current = read(sys.argv[1]), read(sys.argv[2])
    for path, (passes, _, _) in zip(sys.argv[1:], (base, current)):
        if not passes:
            print(f"{path}: no schedule dump (run with CS_DCLF_SCHEDULE_DUMP=1)")
            return 2
    failed = False
    for key in sorted(set(base[2]) | set(current[2])):
        if base[2].get(key) != current[2].get(key):
            failed = True
            print(f"{key}: {base[2].get(key)} -> {current[2].get(key)}")
    for epoch in sorted(set(base[0]) | set(current[0])):
        was, now = base[0].get(epoch), current[0].get(epoch)
        if was == now:
            print(f"epoch {epoch}: {len(now)} passes, same")
            continue
        failed = True
        print(f"epoch {epoch}: DIFFERS")
        print(f"  was: {' | '.join(was) if was else '(none)'}")
        print(f"  now: {' | '.join(now) if now else '(none)'}")
    for epoch in sorted(set(base[1]) & set(current[1])):
        if base[1][epoch] != current[1][epoch]:
            for i, (a, b) in enumerate(zip(base[1][epoch], current[1][epoch])):
                if a != b:
                    print(f"warning: epoch {epoch} pass {i}: type {a} -> {b}")
    print("schedule: DIFFERS" if failed else "schedule: same")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
