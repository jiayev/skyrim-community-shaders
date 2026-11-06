"""compare_chains.py <baseline.pkl> <chains.pkl>=<run folder> ...: from texture_chains.py's files, the textures alive in each build but not
the baseline, sized from the run's last VRAM snapshot, and what holds them. The baseline's own snapshot is not read, so a build without the
ledger (dev's) can be the baseline. Materials with no property are the first thing to look at: nothing in the scene uses them."""
import glob
import json
import os
import pickle
import sys
from collections import Counter


def norm(name):
    name = name.lower().replace(chr(92), '/')
    at = name.find('textures/')
    return name[at:] if at >= 0 else name


def sizes_of(run):
    files = sorted(glob.glob(os.path.join(run, '*VRAM*.json')), key=os.path.getmtime)
    with open(files[-1], encoding='utf-8-sig') as f:
        rows = json.load(f)['rows']
    return {norm(r['name']): r['bytes'] for r in rows if r.get('type') == 'Texture' and '.dds' in r['name'].lower()}


def load(path):
    with open(path, 'rb') as f:
        return pickle.load(f)


def material_uses(chains, material):
    """Where one material is used: no property, a property with no shape, or the roots of its shapes."""
    props = chains['props'].get(material)
    if not props:
        return {'no property: ' + chains['mats'][material][0][4:44]}
    uses = set()
    for prop in props:
        shapes = chains['pshape'].get(prop)
        if not shapes:
            uses.add('property with no shape')
        for shape in shapes or ():
            root = chains['troot'][shape]
            if root == chains['world']:
                uses.add('world')
            else:
                uses.add('detached, with a reference' if chains['rinfo'][root][2] else 'detached prototype')
    return uses


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    baseline = {norm(r[2]) for r in load(sys.argv[1])['tex']}
    for arg in sys.argv[2:]:
        path, run = arg.split('=', 1)
        chains = load(path)
        sizes = sizes_of(run)
        by_name = {norm(r[2]): r for r in chains['tex']}
        extra = set(by_name) - baseline
        materials_of = {}
        for material, (_, textures) in chains['mats'].items():
            for texture in textures:
                materials_of.setdefault(texture, set()).add(material)
        print('=== %s: %d textures, %d not in the baseline (%d MiB), %d in the baseline only' % (
            os.path.basename(path), len(by_name), len(extra), sum(sizes.get(k, 0) for k in extra) >> 20, len(baseline - set(by_name))))
        orphans = Counter(cls[4:44] for m, (cls, _) in chains['mats'].items() if not chains['props'].get(m))
        print('materials with no property:', orphans.most_common(8))
        mib = Counter()
        count = Counter()
        for key in extra:
            uses = set()
            for material in materials_of.get(by_name[key][0], ()):
                uses |= material_uses(chains, material)
            label = ' + '.join(sorted(uses)) or 'no material'
            mib[label] += sizes.get(key, 0)
            count[label] += 1
        print('held by:')
        for label, size in mib.most_common(15):
            print('%6d MiB %4d  %s' % (size >> 20, count[label], label[:130]))
        print('largest:')
        for key in sorted(extra, key=lambda k: -sizes.get(k, 0))[:15]:
            row = by_name[key]
            print('%5d MiB refs %-4d %s' % (sizes.get(key, 0) >> 20, row[1], key[:90]))


if __name__ == '__main__':
    main()
