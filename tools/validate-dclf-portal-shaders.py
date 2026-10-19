"""Compile portal entry points and both interpreters with the runtime Vulkan descriptor-heap ABI."""

import argparse
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dxc", required=True, type=Path)
    parser.add_argument("--spirv-val", type=Path, help="Also validate each module for Vulkan 1.3")
    parser.add_argument("--shader-dir", type=Path, default=Path("build/ALL/aio/Shaders"),
                        help="Prepared shader directory; run prepare_shaders first")
    parser.add_argument("--output-dir", type=Path, default=Path("build/portal-shader-validation"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    abi = (root / "extern/BasicRHI/rhi_shader_abi.h").read_text()

    def constant(name):
        match = re.search(rf"\b{name}\s*=\s*(\d+)\s*;", abi)
        if not match:
            raise RuntimeError(f"Missing runtime shader ABI constant: {name}")
        return match.group(1)

    flags = ["-spirv", "-fvk-use-dx-layout", "-fspv-target-env=vulkan1.3", "-HV", "2021",
             "-T", "cs_6_6", "-Zi", "-D", "BASICRHI_SHADER_API_VULKAN=1"]
    for option, name in [("resource", "RESOURCE"), ("sampler", "SAMPLER"), ("counter", "COUNTER")]:
        flags += [f"-fvk-bind-{option}-heap", constant(f"VULKAN_{name}_DESCRIPTOR_HEAP_BINDING"),
                  constant("VULKAN_DESCRIPTOR_HEAP_SET")]
    shader_root = args.shader_dir.resolve()
    if not (shader_root / "DrawcallLimitFix/PortalBatchCS.hlsl").is_file():
        raise RuntimeError("Prepared portal shaders are missing; run prepare_shaders first")
    flags += ["-I", str(shader_root), "-I", str(root / "package/Shaders")]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    entries = {
        "PortalBatchCS.hlsl": ["Clear", "Count", "TaskCounts", "Scan", "AddOffsets", "Scatter", "MakeTasks", "Evaluate"],
        "Features/Fades/FadeStateCS.hlsl": ["PreparePortalQueries", "main"],
        "BuildDrawsCS.hlsl": ["ClearPortalQueries", "PreparePortalQueries", "main"],
        "ParabolicQueriesCS.hlsl": ["main"],
    }
    count = 0
    for source, names in entries.items():
        for entry in names:
            variants = [(False, False)]
            if source == "ParabolicQueriesCS.hlsl":
                variants = [(False, False), (True, False)]
            elif entry == "Evaluate":
                variants = [(uniform, atlas) for uniform in [False, True] for atlas in [False, True]]
            for uniform, atlas in variants:
                variant = ("uniform" if uniform else "independent") + ("-atlas" if atlas else "")
                output = args.output_dir / f"{Path(source).stem}-{entry}-{variant}.spv"
                command = [str(args.dxc), *flags, "-E", entry, "-Fo", str(output),
                           str(shader_root / "DrawcallLimitFix" / source)]
                if uniform:
                    command += ["-D", "DCLF_PORTAL_UNIFORM_SCAN=1"]
                if atlas:
                    command += ["-D", "DCLF_PORTAL_CONTEXT_ATLAS=1"]
                subprocess.run(command, check=True)
                if args.spirv_val:
                    subprocess.run([str(args.spirv_val), "--target-env", "vulkan1.3", str(output)], check=True)
                count += 1
    print(f"Validated {count} portal shader entry/permutation combinations")


if __name__ == "__main__":
    main()
