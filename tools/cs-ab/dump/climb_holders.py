"""climb_holders.py <dump> <chains.pkl> [class substring] [count]: for the materials with no property (the most textures first), every
pointer to each, its surrounding memory, then a climb through who points at that pointer until a module's static memory. Reading it: a module
static names a global, and a vtable just after the pointer means the record copied the object's bytes. The climb follows one hit at each
level, so a container's links can lead it sideways: judge from the printed memory, and do not read a climb that ends nowhere as no owner."""
import pickle
import sys

import dumplib


def show(d, p, low=-0x20, high=0x48):
    for offset in range(low, high, 8):
        x = d.q(p + offset)
        print('   %+6x 0x%016x %s' % (offset, x or 0, dumplib.describe(d, x)[:110]))


def climb(pool, p, depth=6):
    d = pool.dump
    current = p
    for level in range(depth):
        hits = [h for h in pool.scan([current - o for o in range(0, 0x80, 8)]) if not current - 0x80 <= h <= current + 0x80]
        print('  level %d, 0x%x: %d pointers' % (level, current, len(hits)))
        best = None
        for h in hits[:12]:
            start, cls, offset = dumplib.obj_of(d, h)
            print('    0x%x -> 0x%x (-0x%x) in %s +0x%x %s' % (h, d.q(h), current - d.q(h), cls[:80], max(offset, 0), d.module_of(h) or ''))
            if d.module_of(h):
                print('    reached a static')
                return
            if best is None or (start and not best[0]):
                best = (start, h)
        if not best:
            return
        current = best[0] or best[1]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pool = dumplib.Pool(sys.argv[1])
    d = pool.dump
    with open(sys.argv[2], 'rb') as f:
        chains = pickle.load(f)
    wanted = sys.argv[3] if len(sys.argv) > 3 else ''
    count = int(sys.argv[4]) if len(sys.argv) > 4 else 2
    orphans = [m for m, (cls, _) in chains['mats'].items() if wanted in cls and not chains['props'].get(m)]
    orphans.sort(key=lambda m: -len(chains['mats'][m][1]))
    print('%d materials with no property' % len(orphans))
    for material in orphans[:count]:
        print('=== material 0x%x %s, refs %s, %d textures' % (material, chains['mats'][material][0][4:50], d.d32(material + 8), len(chains['mats'][material][1])))
        for p, cls, offset, _, _ in chains['mother'].get(material, []):
            print(' pointer at 0x%x (%s)' % (p, cls[:60]))
            show(d, p)
            climb(pool, p)
    pool.ex.shutdown()


if __name__ == '__main__':
    main()
