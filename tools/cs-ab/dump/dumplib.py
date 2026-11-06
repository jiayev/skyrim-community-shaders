"""dumplib: a full minidump's memory (Memory64), modules and reads, and scans parallel over its ranges (a process pool; each worker
maps the dump itself). Import from a script guarded by `if __name__ == '__main__'` (Windows spawns the workers)."""
import bisect, mmap, os, struct
import numpy as np
from concurrent.futures import ProcessPoolExecutor


class Dump:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        self.mm = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        sig, ver, n, d = struct.unpack_from('<4sIII', self.mm, 0)
        st = {}
        for i in range(n):
            t, s, r = struct.unpack_from('<III', self.mm, d + 12 * i)
            st[t] = (s, r)
        self.modules = []
        _, r = st[4]
        for i in range(struct.unpack_from('<I', self.mm, r)[0]):
            o = r + 4 + 108 * i
            b, s = struct.unpack_from('<QI', self.mm, o)
            nr = struct.unpack_from('<I', self.mm, o + 20)[0]
            ln = struct.unpack_from('<I', self.mm, nr)[0]
            self.modules.append((b, s, self.mm[nr + 4:nr + 4 + ln].decode('utf-16-le').replace('\\', '/').rsplit('/', 1)[-1]))
        self.modules.sort()
        self.mod_starts = [m[0] for m in self.modules]
        _, r = st[9]
        count, off = struct.unpack_from('<QQ', self.mm, r)
        self.ranges = []
        for i in range(count):
            a, s = struct.unpack_from('<QQ', self.mm, r + 16 + 16 * i)
            self.ranges.append((a, s, off))
            off += s
        self.starts = [x[0] for x in self.ranges]

    def module(self, name):
        for b, s, n in self.modules:
            if n.lower() == name.lower():
                return b, s
        return None

    def module_of(self, a):
        k = bisect.bisect_right(self.mod_starts, a) - 1
        if k >= 0:
            b, s, n = self.modules[k]
            if a < b + s:
                return '%s+0x%x' % (n, a - b)
        return None

    def read(self, va, size):
        k = bisect.bisect_right(self.starts, va) - 1
        if k < 0:
            return None
        a, s, o = self.ranges[k]
        return self.mm[o + va - a:o + va - a + size] if va + size <= a + s else None

    def q(self, va):
        b = self.read(va, 8) if va else None
        return struct.unpack('<Q', b)[0] if b else None

    def d32(self, va):
        b = self.read(va, 4) if va else None
        return struct.unpack('<I', b)[0] if b else None

    def cstr(self, va, limit=160):
        b = self.read(va, limit) if va else None
        if not b:
            return ''
        n = b.find(b'\0')
        return b[:n if n >= 0 else limit].decode('latin-1')


def rtti(d, vtable):
    """The class name MSVC's RTTI gives a vtable (vtable-8: the locator; its +12: the type descriptor's RVA; that +0x10: the name), else None."""
    where = d.module_of(vtable or 0)
    if not where:
        return None
    base = d.module(where.split('+')[0])[0]
    locator = d.q(vtable - 8)
    if not locator or not d.module_of(locator):
        return None
    descriptor = d.d32(locator + 12)
    if not descriptor:
        return None
    name = d.cstr(base + descriptor + 0x10, 200)
    return name if name.startswith('.?A') else None


def obj_of(d, p, span=0x400):
    """(start, class, offset) of the nearest object with RTTI at or before p, else (0, 'anonymous', -1). A guess: a field past the end of a
    smaller object reads as that object's."""
    for offset in range(0, span, 8):
        v = d.q(p - offset)
        if v and d.module_of(v) and not d.module_of(v).startswith('ntdll') and d.module_of(d.q(v) or 0):
            name = rtti(d, v)
            if name:
                return (p - offset, name, offset)
    return (0, 'anonymous', -1)


def describe(d, x):
    """What a qword points at: a module address, an object with RTTI, other mapped memory, or nothing known."""
    if not x:
        return ''
    where = d.module_of(x)
    if where:
        return where
    v = d.q(x)
    if v is None:
        return ''
    return 'obj ' + (rtti(d, v) or d.module_of(v)) if v and d.module_of(v) else 'heap'


_worker = None


def _init(path):
    global _worker
    _worker = Dump(path)


def _scan_chunk(args):
    """The 8-aligned addresses in the given ranges whose qword is in values (sorted)."""
    idx, values = args
    out = []
    mm = _worker.mm
    # Fixed slices, and membership by searchsorted into the sorted values: np.isin sorts a copy of its input, which across the
    # workers' largest ranges exhausts memory.
    step = 1 << 22
    lo, hi = values[0], values[-1]
    for i in idx:
        a, s, o = _worker.ranges[i]
        al = (8 - a % 8) % 8
        n = (s - al) // 8
        for first in range(0, max(n, 0), step):
            count = min(step, n - first)
            arr = np.frombuffer(mm, dtype='<u8', count=count, offset=o + al + 8 * first)
            if len(values) == 1:
                hits = np.nonzero(arr == lo)[0]
            else:
                candidates = np.nonzero((arr >= lo) & (arr <= hi))[0]
                if not len(candidates):
                    continue
                sub = arr[candidates]
                at = np.searchsorted(values, sub)
                at[at == len(values)] = 0
                hits = candidates[values[at] == sub]
            for k in hits:
                out.append(a + al + 8 * (first + int(k)))
    return out


def _map_chunk(args):
    fn, items = args
    return [fn(_worker, x) for x in items]


class Pool:
    def __init__(self, path, workers=None):
        self.path = path
        self.workers = workers or os.cpu_count()
        self.ex = ProcessPoolExecutor(self.workers, initializer=_init, initargs=(path,))
        self.dump = Dump(path)

    def scan(self, values):
        """Every 8-aligned address whose qword is one of values, over all ranges, split by size across the workers."""
        values = np.unique(np.asarray(values, dtype='<u8'))
        order = sorted(range(len(self.dump.ranges)), key=lambda i: -self.dump.ranges[i][1])
        parts = [[] for _ in range(self.workers * 4)]
        loads = [0] * len(parts)
        for i in order:
            j = loads.index(min(loads))
            parts[j].append(i)
            loads[j] += self.dump.ranges[i][1]
        out = []
        for r in self.ex.map(_scan_chunk, [(p, values) for p in parts if p]):
            out.extend(r)
        out.sort()
        return out

    def map(self, fn, items, chunk=2000):
        """fn(dump, item) for each item, on the workers (fn must be a module-level function)."""
        items = list(items)
        chunks = [items[i:i + chunk] for i in range(0, len(items), chunk)]
        out = []
        for r in self.ex.map(_map_chunk, [(fn, c) for c in chunks]):
            out.extend(r)
        return out
