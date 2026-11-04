"""shader_compare.py: a DCLF stage against its native one, compiled offline the way the game compiles each, compared in SPIR-V.

  python tools/shader-compare/shader_compare.py --dump <dir> [--pixel <descriptor> ...] [--kind pulled-ps] [--define NAME[=v] ...]
         [--tree repo|deployed] [--native fxc|cache] [--out <dir>] [--keep]

The dump is an in-game run's CS_DCLF_STAGE_DUMP=<dir> (stages.jsonl): each DCLF stage request's defines (Community Shaders' own,
and DCLF's), its extra DXC arguments, the Lighting programs' descriptor pairs and the pulled rewrite's options. Per stage:
- native: FXC (ps_5_0/vs_5_0, main, /O3 /Ges as ShaderCache's D3DCompile) on the Community Shaders defines, or CS's cached DXBC
  (--native cache: the newest <descriptor>_*.pso/.vso in the MO2 overwrite's ShaderCache), then dxbc-spirv's dxbc_compiler, the
  translator DXVK runs;
- DCLF: dclf-shadertool's rewrite for a pulled stage (the game's PulledSource), then DXC (the game's dxcompiler.dll, through the
  Vulkan SDK's dxc.exe) with ORGModuleServices' arguments.
--tree repo (default) stages package/Shaders and features/*/Shaders as the deploy lays them out, so source edits are compiled at
once; deployed uses the MO2 mod's Shaders. --define adds defines to the DCLF build (as dclf:define does in game).
Prints, per stage and in total, SPIR-V counts that tell the builds apart: instructions, loads by storage class, samples, branches.

  python tools/shader-compare/shader_compare.py --dxbc <shader.dxbc> --spv <shader.spv> [--vertex <vertex.spv>] [--reference <other.dxbc>]
  python tools/shader-compare/shader_compare.py --dxbc <dump folder> --reference <cached .pso/.vso>

One FXC-built shader against what DXVK made of it. A run's DXVK_SHADER_DUMP_PATH=<dir> writes both files, named by the shader's hash, with
reflection stripped; the second form finds a stage in the dump by its signature chunks. Prints the DXBC's count and opcode mix beside the
SPIR-V's, and with --vertex the driver's statistics of the module and of variants with one thing removed. Compiled size is static: confirm
with an NvPerf capture in game (smsp__inst_executed_shader_ps over sm__warps_launched_shader_ps), since the executed count is what costs time.
"""
import argparse
import collections
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SDK = os.environ.get('VULKAN_SDK', r'C:\VulkanSDK\1.4.357.0')
FXC = r'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe'
MO2_MOD = r'C:\Modding\MO2\mods\Community Shaders DXVK ORG'
MO2_CACHE = r'C:\Modding\MO2\overwrite\ShaderCache'
SHADERTOOL = os.path.join(ROOT, 'build', 'shader-tools', 'dclf-shadertool.exe')
DXBC_COMPILER = os.path.join(ROOT, 'build', 'dxbc-spirv-tools', 'tools', 'dxbc_compiler.exe')
STATS = os.path.join(ROOT, 'build', 'shader-tools', 'spv-pipeline-stats.exe')
# ORGModuleServices' SPIR-V arguments (BasicRHI rhi_shader_abi.h AppendVulkanDxcSpirvArguments), in its order.
ABI = ['-spirv', '-fvk-use-dx-layout', '-fspv-target-env=vulkan1.3', '-fvk-bind-resource-heap', '1000000', '0',
       '-fvk-bind-sampler-heap', '1000001', '0', '-fvk-bind-counter-heap', '1000002', '0']
EAGER = []  # --eager: the pulled rewrite with every block member a static (CS_DCLF_LAZY_ROWS=0)
EXTRA_DXC = []  # --dxc-arg: appended to every DCLF build's arguments
KINDS = {'ps': (True, False, False), 'pulled-ps': (True, False, True), 'depth-ps': (True, True, False),
         'pulled-depth-ps': (True, True, True), 'vs': (False, False, False), 'pulled-vs': (False, False, True)}


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace')
    if r.returncode:
        sys.exit(f'failed ({r.returncode}): {" ".join(cmd)}\n{r.stdout[-3000:]}\n{r.stderr[-3000:]}')
    return r.stdout


def stage_tree(out, which):
    """The Data/Shaders tree the game compiles from, under <out>/tree; only changed files are copied."""
    dest = os.path.join(out, 'tree', 'Data', 'Shaders')
    if which == 'deployed':
        roots = [os.path.join(MO2_MOD, 'Shaders')]
    else:
        roots = [os.path.join(ROOT, 'package', 'Shaders')] + sorted(
            os.path.join(ROOT, 'features', f, 'Shaders') for f in os.listdir(os.path.join(ROOT, 'features'))
            if os.path.isdir(os.path.join(ROOT, 'features', f, 'Shaders')))
    for root in roots:
        for d, _, files in os.walk(root):
            for f in files:
                src = os.path.join(d, f)
                dst = os.path.join(dest, os.path.relpath(src, root))
                if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src) or os.path.getsize(dst) != os.path.getsize(src):
                    os.makedirs(os.path.dirname(dst), exist_ok=True)
                    shutil.copy2(src, dst)
    return os.path.join(out, 'tree')


def dxc_exe(out):
    """The SDK's dxc.exe beside the game's dxcompiler.dll (and dxil.dll), so DXC's code is the game's."""
    bindir = os.path.join(out, 'dxc')
    os.makedirs(bindir, exist_ok=True)
    game_bin = os.path.join(MO2_MOD, 'SKSE', 'Plugins', 'CommunityShaders', 'bin')
    for src in [os.path.join(SDK, 'Bin', 'dxc.exe'), os.path.join(game_bin, 'dxcompiler.dll'), os.path.join(game_bin, 'dxil.dll')]:
        dst = os.path.join(bindir, os.path.basename(src))
        if os.path.exists(src) and (not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src)):
            shutil.copy2(src, dst)
    return os.path.join(bindir, 'dxc.exe')


def load_dump(path):
    stages, lighting, options = {}, [], None
    for line in open(os.path.join(path, 'stages.jsonl'), encoding='utf-8'):
        e = json.loads(line)
        if e['kind'] == 'options':
            options = e
        elif e['kind'] == 'lighting':
            lighting.append((e['vertex'], e['pixel']))
        elif e['kind'] == 'stage':
            stages[(e['source'], e['pixel'], e['depth'], e['pulled'], e['family'], e['descriptor'])] = e
    return stages, lighting, options


def native_spirv(stage, tree, out, mode):
    name = os.path.splitext(os.path.basename(stage['source']))[0]
    pixel = stage['pixel']
    tag = f"native-{name}-{'ps' if pixel else 'vs'}-{stage['descriptor']}"
    dxbc = os.path.join(out, tag + '.dxbc')
    if mode == 'cache':
        d = stage['descriptor'].lstrip('0') or '0'
        folder = os.path.join(MO2_CACHE, name)
        found = sorted((f for f in os.listdir(folder) if f.upper().startswith(d.upper() + '_') and f.endswith('.pso' if pixel else '.vso')),
                       key=lambda f: os.path.getmtime(os.path.join(folder, f)))
        if not found:
            return None
        shutil.copyfile(os.path.join(folder, found[-1]), dxbc)
    else:
        cmd = [FXC, '/nologo', '/T', 'ps_5_0' if pixel else 'vs_5_0', '/E', 'main', '/O3', '/Ges', '/Fo', dxbc]
        for n, v in stage['cs']:
            cmd.append(f'/D{n}={v}' if v else f'/D{n}')
        cmd += ['/I', os.path.join(tree, 'Data', 'Shaders'), os.path.join(tree, *stage['source'].split('/'))]
        run(cmd, cwd=tree)
    spv = os.path.join(out, tag + '.spv')
    run([DXBC_COMPILER, dxbc, '--spv', spv])
    return spv


def dclf_spirv(stage, tree, out, options, defines, dxc):
    name = os.path.splitext(os.path.basename(stage['source']))[0]
    source = stage['source']
    if stage['pulled']:
        # The rewrite beside the original, so its relative includes resolve as the game's (the game compiles the rewritten bytes
        # under the original's name).
        pulled = source.replace('.hlsl', '.dclf-pulled.hlsl')
        cmd = [SHADERTOOL, 'rewrite', os.path.join(tree, *source.split('/')), os.path.join(tree, *pulled.split('/')),
               '--shaders', os.path.join(tree, 'Data', 'Shaders'), '--material-textures', ','.join(map(str, options['materialTextures'])),
               '--shadow-mask', str(options['shadowMaskSlot']), '--samplers', str(options['samplerSlots'])]
        if options['lazyRows'] and not EAGER:
            cmd.append('--lazy')
        run(cmd)
        source = pulled
    kind = ('pulled-' if stage['pulled'] else '') + ('depth-' if stage['depth'] else '') + ('ps' if stage['pixel'] else 'vs')
    tag = f"dclf-{name}-{kind}-{stage['descriptor']}"
    spv = os.path.join(out, tag + '.spv')
    cmd = [dxc, '-E', 'main', '-T', stage['target'], '-HV', '2018'] + ABI
    for n, v in stage['cs'] + stage['dclf'] + defines:
        cmd.append(f'-D{n}={v}' if v else f'-D{n}')
    cmd += ["-I", "Data/Shaders"] + stage["arguments"] + EXTRA_DXC + ["-Fo", spv, source]
    if os.environ.get('SHADER_COMPARE_PRINT'):
        print(subprocess.list2cmdline(cmd), '  (in', tree + ')')
    run(cmd, cwd=tree)
    return spv


# Counted from spirv-dis: instructions inside functions, loads by the storage class of their pointer, image work, control flow.
COUNTED = ['OpImageSampleImplicitLod', 'OpImageSampleExplicitLod', 'OpImageSampleDrefImplicitLod', 'OpImageSampleDrefExplicitLod',
           'OpImageFetch', 'OpImageGather', 'OpBranchConditional', 'OpSwitch', 'OpLoopMerge', 'OpPhi', 'OpFunctionCall', 'OpKill',
           'OpDemoteToHelperInvocation', 'OpGroupNonUniformBroadcastFirst', 'OpStore']


def metrics(spv):
    text = run([os.path.join(SDK, 'Bin', 'spirv-dis.exe'), '--raw-id', spv])
    pointer_class, var_class, result_class = {}, {}, {}
    m = collections.Counter()
    in_function = False
    for line in text.splitlines():
        line = line.split(';')[0].strip()
        if not line:
            continue
        lhs, _, rhs = line.partition(' = ') if ' = ' in line else ('', '', line)
        parts = rhs.split()
        op = parts[0]
        if op == 'OpTypePointer':
            pointer_class[lhs] = parts[1]
        elif op == 'OpTypeForwardPointer':
            pass
        elif op == 'OpVariable':
            result_class[lhs] = parts[2]
        elif op == 'OpFunction':
            in_function = True
        elif op == 'OpFunctionEnd':
            in_function = False
        if not in_function or op in ('OpFunction', 'OpLabel', 'OpFunctionEnd', 'OpFunctionParameter'):
            continue
        m['instructions'] += 1
        if op in ('OpAccessChain', 'OpInBoundsAccessChain', 'OpPtrAccessChain', 'OpConvertUToPtr', 'OpBitcast', 'OpCopyObject') and lhs:
            cls = pointer_class.get(parts[1])
            if cls:
                result_class[lhs] = cls
        if op == 'OpLoad':
            m[f'load {result_class.get(parts[2], "?")}'] += 1
        elif op in COUNTED:
            m[op.replace('Op', '')] += 1
        elif op.startswith('OpF') or op.startswith('OpExtInst') or op in ('OpDot', 'OpVectorTimesScalar', 'OpMatrixTimesVector', 'OpVectorTimesMatrix'):
            m['float/ext ops'] += 1
    m['words'] = os.path.getsize(spv) // 4
    return m


def optimized(spv):
    out = spv.replace('.spv', '.opt.spv')
    run([os.path.join(SDK, 'Bin', 'spirv-opt.exe'), '-O', spv, '-o', out])
    return out


def driver_stats(vs, ps):
    out = run([STATS, vs, ps])
    stats = {}
    for line in out.splitlines():
        e = json.loads(line)
        stats['VS' if e['stage'] == 1 else 'FS'] = e['stats']
    return stats


def pipeline_stats(args, stages, lighting, options, tree, dxc, defines):
    """Native and DCLF pipelines of each drawn Lighting program: registers and binary size per stage."""
    pairs = sorted(set(lighting))
    if args.pixel:
        wanted = {p.upper().zfill(8) for p in args.pixel}
        pairs = [(v, p) for v, p in pairs if p in wanted]
    built = {}

    def spirv(kind, d, native):
        key = (kind, d, native)
        if key not in built:
            pixel, depth, pulled = KINDS[kind]
            stage = stages.get(('Data/Shaders/Lighting.hlsl', pixel, depth, pulled, False, d))
            built[key] = None if not stage else (native_spirv(stage, tree, args.out, args.native) if native else dclf_spirv(stage, tree, args.out, options, defines, dxc))
        return built[key]

    keys = [('VS', 'Register Count'), ('FS', 'Register Count'), ('VS', 'Binary Size'), ('FS', 'Binary Size')]
    print(f"{'vertex':9s} {'pixel':9s} | {'VS regs':>11s} | {'FS regs':>11s} | {'VS bytes':>13s} | {'FS bytes':>15s}")
    print(f"{'':19s} | {'nat  dclf':>11s} | {'nat  dclf':>11s} | {'nat   dclf':>13s} | {'nat    dclf':>15s}")
    sums = collections.Counter()
    for v, p in pairs:
        nvs, nps = spirv('vs', v, True), spirv('ps', p, True)
        dvs, dps = spirv('pulled-vs', v, False), spirv('pulled-ps', p, False)
        if not (nvs and nps and dvs and dps):
            print(f'{v} {p}: a stage is missing from the dump')
            continue
        n, d = driver_stats(nvs, nps), driver_stats(dvs, dps)
        cells = []
        for stage, name in keys:
            a, b = n[stage][name], d[stage][name]
            sums[(stage, name, 'n')] += a
            sums[(stage, name, 'd')] += b
            cells.append(f'{a:5d} {b:5d}' if 'Reg' in name else f'{a:6d} {b:6d}')
        print(f'{v:9s} {p:9s} | ' + ' | '.join(cells))
    if pairs:
        print('mean                | ' + ' | '.join(f"{sums[(s, n, 'n')] / len(pairs):5.0f} {sums[(s, n, 'd')] / len(pairs):5.0f}" for s, n in keys))


def dxbc_chunks(path):
    data = open(path, 'rb').read()
    if data[:4] != b'DXBC':
        sys.exit(f'{path} is not DXBC')
    chunks = {}
    for i in range(struct.unpack_from('<I', data, 28)[0]):
        offset = struct.unpack_from('<I', data, 32 + 4 * i)[0]
        size = struct.unpack_from('<I', data, offset + 4)[0]
        chunks[data[offset:offset + 4].decode('latin1')] = data[offset + 8:offset + 8 + size]
    return chunks


def dxbc_code(chunks):
    return chunks.get('SHEX', chunks.get('SHDR', b''))


def signature(chunks):
    return chunks.get('ISGN', chunks.get('ISG1')), chunks.get('OSGN', chunks.get('OSG1'))


def find_by_signature(folder, reference):
    """The dump's DXBC files whose interface is the reference's, largest code first."""
    wanted = signature(dxbc_chunks(reference))
    found = []
    for name in os.listdir(folder):
        if name.endswith('.dxbc'):
            chunks = dxbc_chunks(os.path.join(folder, name))
            if signature(chunks) == wanted:
                found.append((len(dxbc_code(chunks)), name))
    return sorted(found, reverse=True)


def dxbc_opcodes(path, out):
    """FXC's disassembly of a DXBC file: its opcode mix, and the instruction count it states."""
    asm = os.path.join(out, os.path.basename(path) + '.asm')
    run([FXC, '/nologo', '/dumpbin', path, '/Fc', asm])
    text = open(asm, errors='replace').read()
    slots = re.search(r'Approximately (\d+) instruction slots', text)
    ops = collections.Counter()
    for line in text.splitlines():
        word = re.match(r'\s*(?:\d+:\s+)?([a-z][a-z0-9_]*)', line)
        if word and not line.lstrip().startswith(('//', 'dcl_', 'ps_', 'vs_', 'cs_')):
            ops[word.group(1)] += 1
    return int(slots.group(1)) if slots else 0, ops


def spirv_opcodes(path):
    text = run([os.path.join(SDK, 'Bin', 'spirv-dis.exe'), '--raw-id', path])
    ops, ext, modes, in_function = collections.Counter(), collections.Counter(), [], False
    for line in text.splitlines():
        line = line.split(';')[0].strip()
        if not line:
            continue
        parts = line.split(' = ')[-1].split()
        if parts[0] == 'OpFunction':
            in_function = True
        elif parts[0] == 'OpExecutionMode':
            modes.append(' '.join(parts[2:]))
        elif in_function:
            ops[parts[0]] += 1
            if parts[0] == 'OpExtInst':
                ext[parts[3]] += 1
    return text, ops, ext, modes


def split_fma(text):
    count = [0]

    def replace(m):
        count[0] += 1
        return f'{m[1]}%fma{count[0]} = OpFMul {m[3]} {m[4]} {m[5]}\n{m[1]}{m[2]} = OpFAdd {m[3]} %fma{count[0]} {m[6]}'
    return re.sub(r'^(\s*)(%\d+) = OpExtInst (%\d+) %\d+ Fma (%\d+) (%\d+) (%\d+)$', replace, text, flags=re.M)


# What a translation adds, each removed alone from the module's text.
DXBC_VARIANTS = {
    'no SignedZeroInfNanPreserve': lambda t: re.sub(r'^\s*Op(ExecutionMode %\d+|Capability) SignedZeroInfNanPreserve.*\n', '', t, flags=re.M),
    'no NoContraction': lambda t: re.sub(r'^\s*OpDecorate %\d+ NoContraction\n', '', t, flags=re.M),
    'FMin, FMax, FClamp': lambda t: t.replace(' NMin ', ' FMin ').replace(' NMax ', ' FMax ').replace(' NClamp ', ' FClamp '),
    'Fma split': split_fma,
}


def compare_dxbc(args):
    os.makedirs(args.out, exist_ok=True)
    if os.path.isdir(args.dxbc):
        if not args.reference:
            sys.exit('--dxbc <folder> needs --reference')
        for size, name in find_by_signature(args.dxbc, args.reference):
            print(f'{size:8d}  {name}')
        return
    slots, dxbc_ops = dxbc_opcodes(args.dxbc, args.out)
    text, ops, ext, modes = spirv_opcodes(args.spv)
    print(f'DXBC: {slots} instruction slots, {len(dxbc_code(dxbc_chunks(args.dxbc)))} bytes of code')
    print('  ' + ', '.join(f'{k} {v}' for k, v in dxbc_ops.most_common(16)))
    print(f'SPIR-V: {sum(ops.values())} instructions in functions; execution modes: {", ".join(modes)}')
    print('  ' + ', '.join(f'{k} {v}' for k, v in ops.most_common(16)))
    print('  extended: ' + ', '.join(f'{k} {v}' for k, v in ext.most_common(12)))
    if args.reference:
        print(f'reference DXBC: {dxbc_opcodes(args.reference, args.out)[0]} instruction slots')
    if not args.vertex:
        return
    base = driver_stats(args.vertex, args.spv)['FS']
    print(f"driver: {base['Binary Size']} bytes, {base['Register Count']} registers")
    for name, change in DXBC_VARIANTS.items():
        source = os.path.join(args.out, 'variant.spvasm')
        target = os.path.join(args.out, 'variant.spv')
        open(source, 'w').write(change(text))
        run([os.path.join(SDK, 'Bin', 'spirv-as.exe'), '--target-env', 'vulkan1.3', source, '-o', target])
        stats = driver_stats(args.vertex, target)['FS']
        print(f"  {name:30s} {stats['Binary Size']:8d} bytes ({stats['Binary Size'] / base['Binary Size'] - 1:+.1%}), {stats['Register Count']} registers")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dump')
    ap.add_argument('--pixel', action='append', default=[], help='pixel descriptors to compare (default: every Lighting program the dump drew)')
    ap.add_argument('--kind', default='pulled-ps', choices=sorted(KINDS))
    ap.add_argument('--define', action='append', default=[], help='an extra DCLF define, NAME[=value]')
    ap.add_argument('--dxc-arg', action='append', default=[], help='an extra DXC argument for the DCLF builds (e.g. -O3, -Od)')
    ap.add_argument('--eager', action='store_true', help='the pulled rewrite without lazy rows (CS_DCLF_LAZY_ROWS=0)')
    ap.add_argument('--tree', default='repo', choices=['repo', 'deployed'])
    ap.add_argument('--native', default='fxc', choices=['fxc', 'cache'])
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'shader-compare'))
    ap.add_argument('--opt', action='store_true', help='compare both after spirv-opt -O, near what a driver compiler sees once it has promoted locals')
    ap.add_argument('--detail', action='store_true', help='every metric per stage, not only the totals')
    ap.add_argument('--stats', action='store_true',
                    help='the driver\'s statistics (spv-pipeline-stats) of each Lighting program the dump drew, native (VS + PS) against '
                         'DCLF\'s CPU colour pipeline (pulled VS + pulled PS); --pixel narrows it to those pixel descriptors')
    ap.add_argument('--dxbc', help='a DXVK_SHADER_DUMP_PATH .dxbc file to compare with --spv, or the dump folder to search with --reference')
    ap.add_argument('--spv', help="the same shader's .spv from the dump")
    ap.add_argument('--vertex', help="a vertex stage .spv: adds the driver's statistics of the module and of its variants")
    ap.add_argument('--reference', help='a cached .pso/.vso: its slot count, or with --dxbc <folder>, what it finds there')
    args = ap.parse_args()
    if args.dxbc:
        return compare_dxbc(args)
    if not args.dump:
        ap.error('--dump or --dxbc is required')
    EXTRA_DXC.extend(args.dxc_arg)
    if args.eager:
        EAGER.append(True)
    for tool in (SHADERTOOL, DXBC_COMPILER):
        if not os.path.exists(tool):
            sys.exit(f'{tool} is missing: build it (tools/shader-compare/README.md)')
    os.makedirs(args.out, exist_ok=True)
    stages, lighting, options = load_dump(args.dump)
    tree = stage_tree(args.out, args.tree)
    dxc = dxc_exe(args.out)
    defines = [d.split('=', 1) if '=' in d else [d, '1'] for d in args.define]
    if args.stats:
        return pipeline_stats(args, stages, lighting, options, tree, dxc, defines)
    pixel, depth, pulled = KINDS[args.kind]
    wanted = args.pixel or sorted({p if pixel else v for v, p in lighting})
    totals = collections.defaultdict(collections.Counter)
    rows = []
    for d in wanted:
        d = d.upper().zfill(8)
        dclf = stages.get(('Data/Shaders/Lighting.hlsl', pixel, depth, pulled, False, d))
        native = stages.get(('Data/Shaders/Lighting.hlsl', pixel, False, False, False, d)) or dclf
        if not dclf:
            print(f'{d}: no {args.kind} stage in the dump')
            continue
        nspv = native_spirv(native, tree, args.out, args.native)
        dspv = dclf_spirv(dclf, tree, args.out, options, defines, dxc)
        if args.opt:
            nspv, dspv = (optimized(nspv) if nspv else None), optimized(dspv)
        nm, dm = (metrics(nspv) if nspv else collections.Counter()), metrics(dspv)
        totals['native'].update(nm)
        totals['dclf'].update(dm)
        rows.append((d, nm, dm))
    keys = sorted(set(totals['native']) | set(totals['dclf']), key=lambda k: (not k.startswith('instr'), not k.startswith('load'), k))
    print(f"{'descriptor':10s} {'native instr':>13s} {'dclf instr':>11s} {'ratio':>6s} {'native loads':>13s} {'dclf loads':>11s}")
    for d, nm, dm in rows:
        nl = sum(v for k, v in nm.items() if k.startswith('load'))
        dl = sum(v for k, v in dm.items() if k.startswith('load'))
        print(f"{d:10s} {nm['instructions']:13d} {dm['instructions']:11d} {dm['instructions'] / max(1, nm['instructions']):6.2f} {nl:13d} {dl:11d}")
        if args.detail:
            for k in keys:
                if nm[k] or dm[k]:
                    print(f'    {k:36s} {nm[k]:8d} {dm[k]:8d}')
    print(f'\ntotals over {len(rows)} stages ({args.kind}):')
    for k in keys:
        print(f"  {k:36s} native {totals['native'][k]:9d}  dclf {totals['dclf'][k]:9d}")
    print(f'\nSPIR-V and DXBC in {args.out}')


if __name__ == '__main__':
    main()
