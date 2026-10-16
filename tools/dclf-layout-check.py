#!/usr/bin/env python3
"""Drawcall Limit Fix's layout and complexity check (docs/development/dclf-layout.md, "The rules").

    python tools/dclf-layout-check.py            check; exit 1 on a rule broken or a baseline exceeded
    python tools/dclf-layout-check.py --update   rewrite the baselines from the tree (after a step that shrank them)

Rules (always errors):
  1. Only the shell (src/Features/DrawcallLimitFix.{h,cpp}) tests CS_HAS_DCLF; nothing under the DCLF tree tests the render
     graph's or ORGModuleServices' macros (the tree is not built without them: cmake/RenderGraph.cmake).
  2. No file outside the DCLF tree includes a header inside it: the rest of CS calls DCLF through the shell.
  3. Api/ includes only Api/. Core/ includes only Api/ and Core/. Features/<X>/ includes Api/, Core/, its own folder and the
     features DEPENDS names for it. Integrations/<X>/ includes Api/ and Core/.
  4. Every feature DEPENDS names is listed in Features/FeatureList.cpp before the feature that depends on it (its registrations
     run first: toggles, classes, report lines).
  5. The top level is closed: every file under the DCLF tree is in one of TOP_LEVEL's folders (none at its root), and Core/ holds
     only its documented subfolders (CORE_SUBFOLDERS, dclf-layout.md's tree): no new layer folder grows back.

Diagnostics/ may be included from anywhere: production code reaches it only through its observer contract (Hooks.h, Observation,
Policy), which the parity rework will redefine.

Baseline (an error when exceeded, shrunk with --update):
  - the functions over the complexity thresholds (lizard, when installed: pip install lizard): their number may only fall, and
    a function not in the baseline may not cross them. EXEMPT names the functions that are not counted, each with its reason
    (a data table, a flat verdict chain a split would only fragment).
"""
import csv
import io
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src"
TREE = SRC / "Features" / "DrawcallLimitFix"
SHELL = {SRC / "Features" / "DrawcallLimitFix.h", SRC / "Features" / "DrawcallLimitFix.cpp"}
BASELINE = ROOT / "tools" / "dclf-layout-baseline.json"

# Rule 5: the DCLF tree's top-level folders, and Core's subfolders (docs/development/dclf-layout.md, "The target").
TOP_LEVEL = {"Api", "Core", "Features", "Integrations", "Diagnostics"}
CORE_SUBFOLDERS = {"Runtime", "Engine", "Settings", "Scene", "Handoff", "Shading", "Gpu", "Render", "Extensions"}
# Features/<X> -> the other features it may include (and that are listed before it in Features/FeatureList.cpp).
DEPENDS = {
    "Shadows/Sun": {"Shadows/Local"},
    "Shadows/Local": set(),
    "LocalLights": {"Portals"},
    "FirstPerson": {"Actors", "Skins"},
    "Effects": {"Skins"},
    "Fades": set(),
}
FEATURE_LIST = TREE / "Features" / "FeatureList.cpp"
FEATURE_ENTRY = re.compile(r"^\s*((?:\w+::)+)Feature\(\),")
MAX_NLOC = 120
MAX_CCN = 40
# Functions over the thresholds that are not counted (by lizard's qualified name), each with its reason.
EXEMPT = {
    "DCLF::Features::HiddenNodes::HiddenStoreSites": "data table",
}

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
DEPENDENCY_MACRO = re.compile(r"\b(CS_HAS_RENDER_GRAPH|CS_HAS_ORG_MODULE_SERVICES|CS_HAS_DCLF|ORG_MODULE_SERVICES_HAS_DXC)\b")


def sources(base):
    for dirpath, _, files in os.walk(base):
        for name in files:
            if name.endswith((".h", ".hpp", ".cpp", ".inl")):
                yield Path(dirpath) / name


def resolve(path, target):
    """The DCLF file an include names, or None when it is outside the tree."""
    for candidate in (path.parent / target, SRC / target, SRC / "Features" / target):
        try:
            resolved = candidate.resolve()
        except OSError:
            continue
        if resolved.is_file() and TREE in resolved.parents:
            return resolved
    return None


def module(path):
    """Api, Core, Features/<X> (Features/Shadows/<X> for the shadow features), Integrations/<X> or Diagnostics."""
    parts = path.relative_to(TREE).parts
    if len(parts) == 1:
        return "."
    if parts[0] in ("Features", "Integrations") and len(parts) > 2:
        if parts[0] == "Features" and parts[1] == "Shadows" and len(parts) > 3:
            return f"Features/Shadows/{parts[2]}"
        return f"{parts[0]}/{parts[1]}"
    return parts[0]


def allowed(source, target):
    if source == target or target == "Diagnostics":
        return True
    if source == "Api":
        return target == "Api"
    if source == "Core":
        return target in ("Api", "Core")
    if source.startswith("Features/"):
        return target in ("Api", "Core") or target.removeprefix("Features/") in DEPENDS.get(source.removeprefix("Features/"), ())
    if source.startswith("Integrations/"):
        return target in ("Api", "Core")
    return True  # Diagnostics (and a stray folder, which rule 5 reports)


def feature_order():
    """Features/FeatureList.cpp's features in list order, by folder (Shadows::Sun -> Shadows/Sun)."""
    order = []
    for line in FEATURE_LIST.read_text(encoding="utf-8", errors="replace").splitlines():
        m = FEATURE_ENTRY.match(line)
        if m:
            order.append(m.group(1).rstrip(":").replace("::", "/"))
    return order


def depends_order_errors():
    """Rule 4: each DEPENDS entry listed, and listed before its dependent."""
    errors = []
    order = feature_order()
    for dependent, needed in sorted(DEPENDS.items()):
        for feature in sorted(needed | {dependent}):
            if feature not in order:
                errors.append(f"Features/FeatureList.cpp: {feature} (DEPENDS) is not listed")
        for feature in sorted(needed):
            if feature in order and dependent in order and order.index(feature) > order.index(dependent):
                errors.append(f"Features/FeatureList.cpp: {dependent} is listed before {feature}, which it depends on (DEPENDS)")
    return errors


def top_level_errors():
    """Rule 5: every file under the tree in a TOP_LEVEL folder, and Core/'s files in a CORE_SUBFOLDERS folder."""
    errors = []
    for dirpath, _, files in os.walk(TREE):
        for name in files:
            parts = (Path(dirpath) / name).relative_to(TREE).parts
            rel = (Path(dirpath) / name).relative_to(ROOT).as_posix()
            if len(parts) == 1:
                errors.append(f"{rel}: a file at the DCLF tree's root (top level: {', '.join(sorted(TOP_LEVEL))})")
            elif parts[0] not in TOP_LEVEL:
                errors.append(f"{rel}: {parts[0]}/ is not a top-level folder ({', '.join(sorted(TOP_LEVEL))})")
            elif parts[0] == "Core" and (len(parts) == 2 or parts[1] not in CORE_SUBFOLDERS):
                errors.append(f"{rel}: not in one of Core's subfolders ({', '.join(sorted(CORE_SUBFOLDERS))}; dclf-layout.md)")
    return errors


def complexity():
    try:
        out = subprocess.run([sys.executable, "-m", "lizard", "--csv", str(TREE), str(SRC / "Features" / "DrawcallLimitFix.cpp")],
                             capture_output=True, text=True, encoding="utf-8", errors="replace", check=False).stdout
    except OSError:
        return None
    if not out:
        return None
    over = {}
    for cols in csv.reader(io.StringIO(out)):
        if len(cols) < 10 or not cols[0].isdigit():
            continue
        nloc, ccn = int(cols[0]), int(cols[1])
        if (nloc > MAX_NLOC or ccn > MAX_CCN) and cols[7] not in EXEMPT:
            over[cols[7]] = [nloc, ccn]  # by qualified name, so a moved file keeps its baseline
    return over


def main():
    update = "--update" in sys.argv
    errors = top_level_errors()

    for path in sources(SRC):
        inside = TREE in path.parents
        text = path.read_text(encoding="utf-8", errors="replace")
        rel = path.relative_to(ROOT).as_posix()
        if inside and DEPENDENCY_MACRO.search(text):
            errors.append(f"{rel}: tests a dependency macro (only the shell may: CS_HAS_DCLF)")
        for number, line in enumerate(text.splitlines(), 1):
            m = INCLUDE.match(line)
            if not m:
                continue
            target = resolve(path, m.group(1))
            if target is None:
                continue
            if not inside:
                if path not in SHELL:
                    errors.append(f"{rel}:{number}: includes {target.relative_to(SRC).as_posix()} (call DCLF through the shell)")
                continue
            source_module, target_module = module(path), module(target)
            if not allowed(source_module, target_module):
                errors.append(f"{rel}:{number}: {source_module} includes {target_module} ({target.relative_to(TREE).as_posix()})")

    errors += depends_order_errors()

    over = complexity()
    baseline = json.loads(BASELINE.read_text(encoding="utf-8")) if BASELINE.exists() else {}
    if update:
        kept = over if over is not None else {k: v for k, v in baseline.get("overThresholds", {}).items() if k not in EXEMPT}
        BASELINE.write_text(json.dumps({"overThresholds": dict(sorted(kept.items()))}, indent=1) + "\n", encoding="utf-8")
        print(f"baseline written: {len(kept)} functions over the thresholds" + ("" if over is not None else " (lizard not installed: kept)"))
    else:
        if over is None:
            print("complexity: skipped (lizard not installed)")
        else:
            known = {k: v for k, v in baseline.get("overThresholds", {}).items() if k not in EXEMPT}
            for name, (nloc, ccn) in sorted(over.items()):
                if name not in known:
                    errors.append(f"{name}: {nloc} lines, CCN {ccn} (thresholds {MAX_NLOC} / {MAX_CCN}; split it)")
            if len(over) > len(known):
                errors.append(f"functions over the thresholds: {len(over)} > baseline {len(known)}")
            print(f"complexity: {len(over)} functions over {MAX_NLOC} lines or CCN {MAX_CCN} (baseline {len(known)}, "
                  f"{len(EXEMPT)} exempt)")

    for error in errors:
        print("ERROR", error)
    return 1 if errors and not update else 0


if __name__ == "__main__":
    sys.exit(main())
