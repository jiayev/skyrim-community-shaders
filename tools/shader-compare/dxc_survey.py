#!/usr/bin/env python3
"""Builds every game shader the FXC path built with DXC instead, outside the game, and compares the results.

Input is CS_FXC_REQUEST_DUMP's fxc-requests.jsonl (tools/cs-ab/scenarios/fxc-corpus.env) and the shader store that run used,
which holds each request's DXBC. Requests that FXC saw as the same text share one build.

Reports, per shader file: builds DXC rejects, grouped by message; spirv-val failures; constant buffer variables whose offsets
differ from FXC's; `$Globals`; resources one compiler kept and the other removed; inputs and outputs that FXC packed; and whether
FXC's register for each input and output can be predicted from the SPIR-V (fxc_layout).
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

SDK = os.environ.get('VULKAN_SDK', r'C:\VulkanSDK\1.4.357.0')
MOD = r'C:\Modding\MO2\mods\CS AB dclf'
STORE = 'C:/Modding/MO2/overwrite/ShaderCache/v2'  # with the run's variant live (cs-ab.ps1 use)
ARTIFACT_HEADER = 48  # ShaderStore.cpp, ArtifactHeader
# The game's arguments (src/Utils/DxcShaderStore.cpp).
# Each register class gets its own range of bindings here, so the comparison can tell them apart.
SHIFTS = {'b': 0, 't': 1000, 's': 2000, 'u': 3000}
ARGS = ['-spirv', '-fspv-target-env=vulkan1.3', '-fvk-use-dx-layout', '-HV', '2018', '-fvk-use-dx-position-w', '-fspv-use-unknown-image-format']
for register, shift in SHIFTS.items():
    ARGS += [f'-fvk-{register}-shift', str(shift), '0']
# D3D_SHADER_INPUT_TYPE to register class.
INPUT_CLASS = {0: 'b', 1: 't', 2: 't', 3: 's', 4: 'u', 5: 't', 6: 'u', 7: 't', 8: 'u', 9: 'u', 10: 'u', 11: 'u'}
KEEP_ERRORS = 3


def dxc_exe(out):
    """The SDK's dxc.exe beside the game's dxcompiler.dll, so the compiler's code is the game's."""
    folder = os.path.join(out, 'dxc')
    os.makedirs(folder, exist_ok=True)
    for src in [os.path.join(SDK, 'Bin', 'dxc.exe'), os.path.join(MOD, 'SKSE', 'Plugins', 'CommunityShaders', 'bin', 'dxcompiler.dll')]:
        shutil.copy2(src, folder)
    return os.path.join(folder, 'dxc.exe')


def stage_tree(out):
    """The staged mod's shaders under <out>/tree/Data/Shaders, the paths the requests name."""
    dest = os.path.join(out, 'tree', 'Data', 'Shaders')
    shutil.copytree(os.path.join(MOD, 'Shaders'), dest, dirs_exist_ok=True)
    return os.path.join(out, 'tree')


def build(request, dxc, tree, out):
    spv = os.path.join(out, 'spv', request['key'] + '.spv')
    target = request['profile'].split('_')[0] + '_6_6'
    command = [dxc, '-E', request['entry'], '-T', target] + ARGS + ['-I', os.path.dirname(request['root']), '-I', 'Data/Shaders']
    for name, value in request['defines']:
        command.append(f'-D{name}={value}' if value else f'-D{name}')
    command += ['-Fo', spv, request['root']]
    start = time.perf_counter()
    result = subprocess.run(command, cwd=tree, capture_output=True, text=True, errors='replace')
    seconds = time.perf_counter() - start
    # The optimizer drops unread inputs, even with -fspv-preserve-interface, and keeps its numbering. FXC's signature has them, and
    # they take registers, so the whole interface comes from a build that stops before it.
    interface = spv + '.interface'
    start = time.perf_counter()
    command[-3:] = ['-fcgl', '-fspv-reflect', '-Fo', interface, request['root']]
    subprocess.run(command, cwd=tree, capture_output=True, text=True, errors='replace')
    return {'request': request, 'spv': spv, 'interface': interface, 'ok': result.returncode == 0, 'seconds': seconds, 'stderr': result.stderr, 'interfaceSeconds': time.perf_counter() - start}


def error_lines(text):
    """(file:line, message) of a failed build's errors. dxc prints each with its include chain before it."""
    found = []
    for line in text.splitlines():
        match = re.match(r'(.*?):(\d+):\d+: (?:fatal )?error: (.*)', line)
        if match:
            found.append((f'{os.path.basename(match[1])}:{match[2]}', match[3]))
        elif line.startswith('error:') or line.startswith('fxc') or 'Validation failed' in line:
            found.append(('', line.strip()))
    return found or [('', text.strip()[:200])]


def message_class(message):
    """The message with names removed, so one mistake in many forms counts once."""
    return re.sub(r"'[^']*'", "'_'", message)


def dxbc_chunks(data):
    count = struct.unpack_from('<I', data, 28)[0]
    chunks = {}
    for i in range(count):
        offset = struct.unpack_from('<I', data, 32 + 4 * i)[0]
        tag = data[offset:offset + 4].decode('latin1')
        size = struct.unpack_from('<I', data, offset + 4)[0]
        chunks[tag] = data[offset + 8:offset + 8 + size]
    return chunks


def cstring(blob, offset):
    return blob[offset:blob.index(b'\0', offset)].decode('latin1')


def rdef(chunk):
    """FXC's reflection: {cbuffer: {variable: (offset, size)}} and [(name, input type, slot, count)]."""
    cb_count, cb_offset, bound_count, bound_offset = struct.unpack_from('<IIII', chunk, 0)
    sizes = {}
    buffers = {}
    for i in range(cb_count):
        name, variables, first, size = struct.unpack_from('<IIII', chunk, cb_offset + 24 * i)
        members = {}
        sizes[cstring(chunk, name)] = size
        for v in range(variables):
            var_name, start, var_size = struct.unpack_from('<III', chunk, first + 40 * v)
            members[cstring(chunk, var_name)] = (start, var_size)
        buffers[cstring(chunk, name)] = members
    bound = []
    for i in range(bound_count):
        name, kind, _, _, _, slot, count = struct.unpack_from('<IIIIIII', chunk, bound_offset + 32 * i)
        bound.append((cstring(chunk, name), kind, slot, count))
    return buffers, bound, sizes


def signature(chunk):
    """[(semantic, index, system value, register, mask)] of an ISGN or OSGN."""
    count = struct.unpack_from('<I', chunk, 0)[0]
    entries = []
    for i in range(count):
        name, index, system, _, register, mask = struct.unpack_from('<IIIIII', chunk, 8 + 24 * i)
        entries.append((cstring(chunk, name), index, system, register, mask & 0xff))
    return entries


def spirv_string(words):
    raw = b''.join(struct.pack('<I', w) for w in words)
    return raw[:raw.index(b'\0')].decode('latin1')


def spirv_reflect(path):
    """What the comparison needs from a module: its uniform blocks' member offsets, resources by binding, and I/O by location."""
    data = open(path, 'rb').read()
    words = struct.unpack(f'<{len(data) // 4}I', data)
    names, member_names, offsets, decorations, pointers, variables, types = {}, {}, {}, {}, {}, [], {}
    semantics, builtins, locations, interpolation = {}, {}, {}, {}
    scalars, vectors, arrays, constants, member_types, strides = {}, {}, {}, {}, {}, {}
    row_major = set()
    i = 5
    while i < len(words):
        length, op = words[i] >> 16, words[i] & 0xffff
        operands = words[i + 1:i + length]
        if op == 5:  # OpName
            names[operands[0]] = spirv_string(operands[1:])
        elif op == 6:  # OpMemberName
            member_names[(operands[0], operands[1])] = spirv_string(operands[2:])
        elif op == 71:  # OpDecorate
            decoration = operands[1]
            if decoration == 33:
                decorations.setdefault(operands[0], {})['binding'] = operands[2]
            elif decoration == 34:
                decorations.setdefault(operands[0], {})['set'] = operands[2]
            elif decoration == 30:
                locations[operands[0]] = operands[2]
            elif decoration == 11:
                builtins[operands[0]] = operands[2]
            elif decoration == 6:
                strides[operands[0]] = operands[2]
            elif decoration in (13, 14, 16, 17):  # NoPerspective, Flat, Centroid, Sample
                interpolation[operands[0]] = decoration
        elif op == 72 and operands[2] == 35:  # OpMemberDecorate Offset
            offsets[(operands[0], operands[1])] = operands[3]
        elif op == 72 and operands[2] == 4:  # OpMemberDecorate RowMajor
            row_major.add((operands[0], operands[1]))
        elif op == 5632:  # OpDecorateString UserSemantic
            semantics[operands[0]] = spirv_string(operands[2:])
        elif op == 20:  # OpTypeBool
            scalars[operands[0]] = (4, False)
        elif op in (21, 22):  # OpTypeInt, OpTypeFloat
            scalars[operands[0]] = (operands[1] // 8, op == 22)
        elif op in (23, 24):  # OpTypeVector, OpTypeMatrix
            vectors[operands[0]] = (operands[1], operands[2], op == 24)
        elif op == 28:  # OpTypeArray
            arrays[operands[0]] = (operands[1], operands[2])
        elif op == 43:  # OpConstant
            constants[operands[1]] = operands[2]
        elif op == 32:  # OpTypePointer
            pointers[operands[0]] = operands[2]
        elif op in (25, 26, 27, 30):  # image, sampler, sampled image, struct
            types[operands[0]] = op
            if op == 30:
                member_types[operands[0]] = operands[1:]
        elif op == 59:  # OpVariable
            variables.append((operands[1], operands[2], pointers.get(operands[0])))
        elif op == 54:  # OpFunction: nothing the comparison reads follows
            break
        i += length
    def shape(type_id):
        """(components of one register, registers, is float) of an I/O variable's type."""
        if type_id in arrays:
            components, registers, is_float = shape(arrays[type_id][0])
            return components, registers * constants[arrays[type_id][1]], is_float
        if type_id in vectors:
            element, count, matrix = vectors[type_id]
            if matrix:
                components, _, is_float = shape(element)
                return components, count, is_float
            return count, 1, shape(element)[2]
        return 1, 1, scalars[type_id][1]

    def size_of(type_id, is_row_major=False):
        """A constant buffer member's size in bytes, in the dx layout: the last row or element is not padded."""
        if type_id in arrays:
            element = arrays[type_id][0]
            return strides.get(type_id, 16) * (constants[arrays[type_id][1]] - 1) + size_of(element, is_row_major)
        if type_id in vectors:
            element, count, matrix = vectors[type_id]
            if not matrix:
                return count * size_of(element)
            column_size, _, _ = shape(element)
            # A row major matrix is stored as its rows: one vector per component of a column.
            vectors_stored, vector_size = (column_size, count) if is_row_major else (count, column_size)
            return 16 * (vectors_stored - 1) + 4 * vector_size
        if type_id in member_types:
            last = len(member_types[type_id]) - 1
            return offsets.get((type_id, last), 0) + size_of(member_types[type_id][last], (type_id, last) in row_major)
        return scalars[type_id][0]

    blocks, resources, inputs, outputs, block_sizes = {}, [], [], [], {}
    for variable, storage, pointee in variables:
        if storage == 2:  # Uniform
            name = names.get(pointee, '').removeprefix('type.')
            members = {member: offset for (struct_id, index), offset in offsets.items() if struct_id == pointee
                       for member in [member_names.get((pointee, index), str(index))]}
            blocks[name] = members
            block_sizes[name] = size_of(pointee) if pointee in member_types else 0
        if storage in (0, 2, 12):  # UniformConstant, Uniform, StorageBuffer
            resources.append((names.get(variable, str(variable)), storage, decorations.get(variable, {}).get('binding')))
        elif storage in (1, 3):  # Input, Output
            entry = {'semantic': semantics.get(variable, names.get(variable, '')), 'location': locations.get(variable), 'builtin': builtins.get(variable),
                     'interpolation': interpolation.get(variable), 'shape': shape(pointee)}
            (inputs if storage == 1 else outputs).append(entry)
    return blocks, resources, inputs, outputs, block_sizes


def split_semantic(semantic):
    match = re.match(r'(.*?)(\d*)$', semantic)
    return match[1].upper(), int(match[2] or 0)


def fxc_layout(variables, packs=True):
    """The (register, first component) FXC gives each I/O variable, in declaration order: a variable goes in the first earlier register
    it fits and that interpolates the same, arrays and matrices start their own, and system values take a whole register. Vertex
    inputs are not packed."""
    layout = {}
    registers_used = []  # per register: [components used, interpolation]
    for variable in variables:
        components, registers, is_float = variable['shape']
        mode = variable['interpolation'] if is_float else 14
        name, index = split_semantic(variable['semantic'])
        if name in ('SV_DEPTH', 'SV_COVERAGE', 'SV_DEPTHGREATEREQUAL', 'SV_DEPTHLESSEQUAL'):
            continue  # no register of its own: a special output
        if name == 'SV_TARGET':  # numbered by its index, whatever else the stage writes
            layout[(name, index)] = (index, 0)
            continue
        system = variable['builtin'] is not None or name.startswith('SV_')
        target = None
        if packs and registers == 1 and not system:
            target = next((r for r, (used, m) in enumerate(registers_used) if m == mode and used + components <= 4), None)
        if target is not None:
            layout[(name, index)] = (target, registers_used[target][0])
            registers_used[target][0] += components
            continue
        for r in range(registers):
            layout[(name, index + r)] = (len(registers_used), 0)
            registers_used.append([4 if registers > 1 or system else components, mode])
    return layout


def layout_differences(entries, variables, packs=True):
    """How FXC's signature differs from fxc_layout's prediction. Only variables FXC kept are compared."""
    predicted = fxc_layout(variables, packs)
    differences = []
    for name, index, _, register, mask in entries:
        if register == 0xffffffff:  # SV_Depth and the like
            continue
        first = (mask & -mask).bit_length() - 1 if mask else 0
        expected = predicted.get((name.upper(), index))
        if expected is None:
            differences.append(f'{name}{index}: only in FXC')
        elif expected[0] != register or (mask and expected[1] != first):
            differences.append(f'{name}{index}: FXC {register}.{first}, predicted {expected[0]}.{expected[1]}')
    return differences


def packed_registers(entries):
    """Whether FXC put more than one variable in a register (a location per variable would then differ from FXC's numbering)."""
    per_register = collections.Counter(e[3] for e in entries if not e[2])
    return any(n > 1 for n in per_register.values())


def compare(done, store):
    """Reflection differences of one successful build against its DXBC."""
    request = done['request']
    key = request['key']
    path = os.path.join(store, 'cas', key[:2], key + '.bin')
    result = {'offsets': [], 'sizes': [], 'globals': False, 'fxcOnly': [], 'dxcOnly': [], 'packed': False, 'layout': [], 'compared': 0}
    with open(path, 'rb') as f:
        dxbc = f.read()[ARTIFACT_HEADER:]
    chunks = dxbc_chunks(dxbc)
    buffers, bound, sizes = rdef(chunks['RDEF'])
    blocks, resources, _, _, block_sizes = spirv_reflect(done['spv'])
    _, _, inputs, outputs, _ = spirv_reflect(done['interface'])
    result['globals'] = any(name.endswith('Globals') for name in blocks) or '$Globals' in buffers
    for name, members in buffers.items():
        block = blocks.get(name)
        if block is None:
            continue
        if (block_sizes[name] + 15) // 16 != (sizes[name] + 15) // 16:
            result['sizes'].append(f'{name}: {sizes[name]} against {block_sizes[name]}')
        for member, (offset, _) in members.items():
            result['compared'] += member in block
            if member in block and block[member] != offset:
                result['offsets'].append(f'{name}.{member}: {offset} against {block[member]}')
    fxc_slots = {f'{INPUT_CLASS.get(kind, "?")}{slot + i}' for _, kind, slot, count in bound for i in range(count or 1)}
    dxc_slots = set()
    for _, _, binding in resources:
        if binding is not None:
            register = max((r for r in SHIFTS if SHIFTS[r] <= binding), key=SHIFTS.get)
            dxc_slots.add(f'{register}{binding - SHIFTS[register]}')
    result['fxcOnly'] = sorted(fxc_slots - dxc_slots)
    result['dxcOnly'] = sorted(dxc_slots - fxc_slots)
    for tag, variables in (('ISGN', inputs), ('OSGN', outputs)):
        if tag in chunks:
            entries = signature(chunks[tag])
            result['packed'] = result['packed'] or packed_registers(entries)
            result['layout'] += [f'{tag} {d}' for d in layout_differences(entries, variables, not (tag == 'ISGN' and request['profile'].startswith('vs')))]
    return result


def validate(done, spirv_val):
    result = subprocess.run([spirv_val, '--target-env', 'vulkan1.3', '--scalar-block-layout', done['spv']], capture_output=True, text=True, errors='replace')
    return result.stdout + result.stderr if result.returncode else ''


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--requests', default=r'C:\Modding\CS-AB\fxc-corpus\fxc-requests.jsonl')
    parser.add_argument('--store', default=STORE)
    parser.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'dxc-survey'))
    parser.add_argument('--only', default='', help='a regular expression on the root path')
    args = parser.parse_args()
    out = os.path.abspath(args.out)
    os.makedirs(os.path.join(out, 'spv'), exist_ok=True)
    unique = {}
    with open(args.requests, encoding='utf-8') as f:
        for line in f:
            request = json.loads(line)
            if re.search(args.only, request['root']):
                unique.setdefault(request['key'], request)
    dxc, tree = dxc_exe(out), stage_tree(out)
    spirv_val = os.path.join(SDK, 'Bin', 'spirv-val.exe')
    print(f'{len(unique)} builds')

    start = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as pool:
        results = list(pool.map(lambda r: build(r, dxc, tree, out), unique.values()))
        print(f'compiled in {time.perf_counter() - start:.0f} s, {sum(r["seconds"] for r in results):.0f} s of compiler time, and {sum(r["interfaceSeconds"] for r in results):.0f} s for the interfaces')
        passed = [r for r in results if r['ok']]
        invalid = dict(zip((r['request']['key'] for r in passed), pool.map(lambda r: validate(r, spirv_val), passed)))
        compared = dict(zip((r['request']['key'] for r in passed), pool.map(lambda r: compare(r, args.store), passed)))

    per_root = collections.defaultdict(lambda: collections.Counter())
    errors = collections.defaultdict(lambda: {'count': 0, 'files': collections.Counter(), 'where': collections.Counter(), 'example': ''})
    for r in results:
        root = os.path.basename(r['request']['root'])
        stage = r['request']['profile'][:2]
        per_root[(root, stage)]['builds'] += 1
        per_root[(root, stage)]['seconds'] += r['seconds']
        if not r['ok']:
            per_root[(root, stage)]['rejected'] += 1
            for where, message in error_lines(r['stderr'])[:KEEP_ERRORS]:
                e = errors[message_class(message)]
                e['count'] += 1
                e['files'][root] += 1
                e['where'][where] += 1
                e['example'] = e['example'] or message
            continue
        c = compared[r['request']['key']]
        key = (root, stage)
        per_root[key]['invalid'] += bool(invalid[r['request']['key']])
        per_root[key]['offsets'] += bool(c['offsets'])
        per_root[key]['globals'] += c['globals']
        per_root[key]['packed'] += c['packed']
        per_root[key]['fxcOnly'] += bool(c['fxcOnly'])
        per_root[key]['dxcOnly'] += bool(c['dxcOnly'])
        per_root[key]['layout'] += bool(c['layout'])
        per_root[key]['sizes'] += bool(c['sizes'])

    print(f'\n{"file":44} {"":3} {"builds":>6} {"reject":>6} {"inval":>6} {"offset":>6} {"$glob":>6} {"packed":>6} {"fxc+":>6} {"dxc+":>6} {"layout":>6} {"size":>6} {"avg s":>6}')
    for (root, stage), c in sorted(per_root.items(), key=lambda e: -e[1]['builds']):
        print(f'{root:44} {stage:3} {c["builds"]:6} {c["rejected"]:6} {c["invalid"]:6} {c["offsets"]:6} {c["globals"]:6} {c["packed"]:6} {c["fxcOnly"]:6} {c["dxcOnly"]:6} {c["layout"]:6} {c["sizes"]:6} {c["seconds"] / c["builds"]:6.1f}')
    total = collections.Counter()
    for c in per_root.values():
        total.update(c)
    print(f'{"total":44} {"":3} {total["builds"]:6} {total["rejected"]:6} {total["invalid"]:6} {total["offsets"]:6} {total["globals"]:6} {total["packed"]:6} {total["fxcOnly"]:6} {total["dxcOnly"]:6} {total["layout"]:6} {total["sizes"]:6}')

    print('\nRejections, by message:')
    for _, e in sorted(errors.items(), key=lambda item: -item[1]['count']):
        print(f'{e["count"]:5}  {e["example"]}')
        print(f'       at {", ".join(f"{w} ({n})" for w, n in e["where"].most_common(4))}; in {", ".join(f"{w} ({n})" for w, n in e["files"].most_common(4))}')

    report = {r['request']['key']: {'root': r['request']['root'], 'profile': r['request']['profile'], 'defines': r['request']['defines'], 'ok': r['ok'], 'seconds': r['seconds'], 'stderr': r['stderr'][:4000],
                                    'invalid': invalid.get(r['request']['key'], '')[:2000], **compared.get(r['request']['key'], {})} for r in results}
    with open(os.path.join(out, 'survey.json'), 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=0)
    print(f'\nDetails: {os.path.join(out, "survey.json")}')


if __name__ == '__main__':
    sys.exit(main())
