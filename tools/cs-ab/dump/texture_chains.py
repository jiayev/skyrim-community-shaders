"""texture_chains.py <dump> <out.pkl>: what keeps each texture alive in a full process dump (procdump -ma), for compare_chains.py and
climb_holders.py: NiSourceTexture -> material -> shader property (+0x78) -> shape (+0x128) -> root (+0x30, the parents). The vtables are
AE 1.6.1170's, rebased to where the game loaded."""
import pickle
import sys
import time
from collections import Counter

import dumplib

VT_SOURCE_TEXTURE = 0x1419adbb8
VT_TRISHAPE = 0x1419ad4a0
IMAGE_BASE = 0x140000000


def texture_row(d, s):
    name = d.q(s + 0x20)
    return (s, d.d32(s + 8), d.cstr(name, 260) if name else '', d.q(s + 0x48) or 0)


def holder_row(d, p):
    start, cls, offset = dumplib.obj_of(d, p)
    return (p, d.q(p), start, cls, offset, d.q(p - 8) or 0, d.q(p + 8) or 0)


def top_of(d, shape):
    a = shape
    for _ in range(64):
        parent = d.q(a + 0x30)
        if not parent:
            break
        a = parent
    return a


def root_row(d, root):
    return (root, d.d32(root + 8), d.q(root + 0xf8) or 0, d.cstr(d.q(root + 0x10) or 0, 80), dumplib.rtti(d, d.q(root) or 0) or '')


def ref_row(d, x):
    return (x, d.d32(x + 8))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    t0 = time.time()
    dump, out = sys.argv[1:3]
    pool = dumplib.Pool(dump)
    d = pool.dump
    base, _ = d.module('SkyrimSE.exe')
    rebase = base - IMAGE_BASE

    textures = [r for r in pool.map(texture_row, pool.scan([VT_SOURCE_TEXTURE + rebase])) if r[2] and r[1] is not None and r[1] < 100000]
    known = {r[0] for r in textures}
    print('%d textures (%.0fs)' % (len(textures), time.time() - t0), flush=True)
    # A texture's +0x30 and +0x38 link it to its neighbours: not holders.
    holders = [r for r in pool.map(holder_row, pool.scan(sorted(known))) if not (r[3].startswith('.?AVNiSourceTexture') and r[4] in (0x30, 0x38))]
    materials = {}
    texture_holders = {}
    for p, texture, start, cls, offset, _, _ in holders:
        if texture not in known:
            continue
        if 'ShaderMaterial' in cls and offset >= 0:
            materials.setdefault(start, (cls, set()))[1].add(texture)
        texture_holders.setdefault(texture, []).append((p, start, cls, offset))
    material_refs = dict(pool.map(ref_row, list(materials)))

    properties = {}
    material_others = {}
    for p, material, start, cls, offset, before, after in pool.map(holder_row, pool.scan(sorted(materials))):
        if material not in materials:
            continue
        if offset == 0x78 and 'ShaderProperty' in cls:
            properties.setdefault(material, set()).add(start)
        else:
            material_others.setdefault(material, []).append((p, cls, offset, before, after))
    print('%d materials, %d with no property (%.0fs)' % (len(materials), sum(1 for m in materials if not properties.get(m)), time.time() - t0), flush=True)

    property_list = sorted({x for v in properties.values() for x in v})
    property_refs = dict(pool.map(ref_row, property_list))
    property_shapes = {}
    property_others = {}
    for p, prop, start, cls, offset, before, after in pool.map(holder_row, pool.scan(property_list)):
        if prop not in property_refs:
            continue
        if offset == 0x128 or 'TriShape' in cls or 'Geometry' in cls:
            property_shapes.setdefault(prop, set()).add(start)
        else:
            property_others.setdefault(prop, []).append((p, cls, offset, before, after))

    tops = Counter(pool.map(top_of, pool.scan([VT_TRISHAPE + rebase])))
    world = tops.most_common(1)[0][0]
    shapes = sorted({x for v in property_shapes.values() for x in v})
    shape_roots = dict(zip(shapes, pool.map(top_of, shapes)))
    roots = {r[0]: r for r in pool.map(root_row, sorted(set(shape_roots.values())))}
    print('%d shapes, %d under the world root, %d roots (%.0fs)' % (sum(tops.values()), tops[world], len(tops), time.time() - t0), flush=True)

    with open(out, 'wb') as f:
        pickle.dump(dict(tex=textures, texh=texture_holders, mats=materials, mref=material_refs, props=properties, mother=material_others,
                         pref=property_refs, pshape=property_shapes, pother=property_others, troot=shape_roots, rinfo=roots, world=world, tops=tops), f)
    pool.ex.shutdown()


if __name__ == '__main__':
    main()
