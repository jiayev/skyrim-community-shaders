#!/usr/bin/env python3

import argparse
import re
from pathlib import Path


PRIMITIVE_TYPES = {
    "bool": "Boolean",
    "float": "Float",
    "double": "Float",
    "int": "Integer",
    "int8_t": "Integer",
    "int16_t": "Integer",
    "int32_t": "Integer",
    "int64_t": "Integer",
    "uint": "Integer",
    "uint8_t": "Integer",
    "uint16_t": "Integer",
    "uint32_t": "Integer",
    "uint64_t": "Integer",
    "std::int8_t": "Integer",
    "std::int16_t": "Integer",
    "std::int32_t": "Integer",
    "std::int64_t": "Integer",
    "std::uint8_t": "Integer",
    "std::uint16_t": "Integer",
    "std::uint32_t": "Integer",
    "std::uint64_t": "Integer",
    "std::string": "String",
    "string": "String",
}

STRUCT_DECL_RE = r"\bstruct\s+(?:alignas\s*\([^)]*\)\s+)?(\w+)[^{;]*\{"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="ignore")


def cpp_escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def prettify(identifier: str) -> str:
    if not identifier:
        return identifier
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", identifier)
    text = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", text)
    text = text.replace("_", " ")
    return text[:1].upper() + text[1:]


def split_args(arg_text: str) -> list[str]:
    args = []
    current = []
    depth = 0
    in_string = False
    escaped = False
    for ch in arg_text:
        if in_string:
            current.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
            current.append(ch)
        elif ch in "([{<":
            depth += 1
            current.append(ch)
        elif ch in ")]}>":
            depth = max(0, depth - 1)
            current.append(ch)
        elif ch == "," and depth == 0:
            args.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    if current:
        args.append("".join(current).strip())
    return args


def find_matching_paren(text: str, open_index: int) -> int:
    depth = 0
    in_string = False
    escaped = False
    for i in range(open_index, len(text)):
        ch = text[i]
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return i
    return -1


def find_matching_brace(text: str, open_index: int) -> int:
    depth = 0
    in_string = False
    escaped = False
    for i in range(open_index, len(text)):
        ch = text[i]
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
        elif ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return i
    return -1


def collect_nlohmann_macros(paths: list[Path]) -> dict[str, list[str]]:
    macros: dict[str, list[str]] = {}
    token = "NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT"
    for path in paths:
        text = read_text(path)
        pos = 0
        while True:
            start = text.find(token, pos)
            if start < 0:
                break
            open_index = text.find("(", start + len(token))
            if open_index < 0:
                break
            close_index = find_matching_paren(text, open_index)
            if close_index < 0:
                break
            args = split_args(text[open_index + 1:close_index])
            if len(args) >= 2:
                type_name = args[0].strip()
                fields = [a.strip().rstrip(";") for a in args[1:] if a.strip()]
                macros[type_name] = fields
            pos = close_index + 1
    return macros


def collect_struct_bodies(paths: list[Path]) -> dict[str, str]:
    bodies: dict[str, str] = {}
    for path in paths:
        text = read_text(path)
        for match in re.finditer(STRUCT_DECL_RE, text):
            name = match.group(1)
            body_start = match.end()
            depth = 1
            i = body_start
            in_string = False
            escaped = False
            while i < len(text):
                ch = text[i]
                if in_string:
                    if escaped:
                        escaped = False
                    elif ch == "\\":
                        escaped = True
                    elif ch == '"':
                        in_string = False
                elif ch == '"':
                    in_string = True
                elif ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        bodies.setdefault(name, text[body_start:i])
                        break
                i += 1
    return bodies


def clean_type(type_name: str) -> str:
    type_name = re.sub(r"\b(const|volatile|mutable|static|inline|constexpr)\b", "", type_name)
    type_name = type_name.replace("&", "").replace("*", "").strip()
    type_name = re.sub(r"\s+", " ", type_name)
    return type_name


def parse_struct_fields(body: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for raw_stmt in body.split(";"):
        stmt = raw_stmt.strip()
        stmt = re.sub(r"\b(public|private|protected)\s*:\s*", "", stmt).strip()
        if not stmt or "(" in stmt or ")" in stmt:
            continue
        if stmt.startswith(("using ", "enum ", "static ", "static_assert", "STATIC_ASSERT", "return ")):
            continue
        stmt = re.sub(r"//.*", "", stmt).strip()
        stmt = re.sub(r"=\s*.*$", "", stmt).strip()
        stmt = re.sub(r"\[[^\]]*\]", "", stmt).strip()
        match = re.match(r"(.+?)\s+([A-Za-z_]\w*)$", stmt)
        if not match:
            continue
        field_type = clean_type(match.group(1))
        field_name = match.group(2)
        fields[field_name] = field_type
    return fields


def strip_nested_type_bodies(body: str) -> str:
    result = list(body)
    for match in re.finditer(r"\b(?:struct|class|enum)\s+\w+[^{;]*\{", body):
        end = find_matching_brace(body, match.end() - 1)
        if end < 0:
            continue
        for i in range(match.start(), end + 1):
            result[i] = " "
    return "".join(result)


def collect_feature_struct_fields(paths: list[Path], features: dict[str, dict[str, str]]) -> dict[str, dict[str, dict[str, str]]]:
    feature_fields: dict[str, dict[str, dict[str, str]]] = {}
    for path in paths:
        text = read_text(path)
        for feature_class in features:
            feature_match = re.search(rf"\b(?:struct|class)\s+{re.escape(feature_class)}[^\{{;]*\{{", text)
            if not feature_match:
                continue
            feature_end = find_matching_brace(text, feature_match.end() - 1)
            if feature_end < 0:
                continue

            feature_body = text[feature_match.end():feature_end]
            for struct_match in re.finditer(STRUCT_DECL_RE, feature_body):
                body_start = struct_match.end()
                body_end = find_matching_brace(feature_body, body_start - 1)
                if body_end < 0:
                    continue
                fields = parse_struct_fields(feature_body[body_start:body_end])
                feature_fields.setdefault(feature_class, {})[struct_match.group(1)] = fields
    return feature_fields


def collect_feature_member_fields(paths: list[Path], features: dict[str, dict[str, str]]) -> dict[str, dict[str, str]]:
    feature_members: dict[str, dict[str, str]] = {}
    for path in paths:
        text = read_text(path)
        for feature_class in features:
            feature_match = re.search(rf"\b(?:struct|class)\s+{re.escape(feature_class)}[^\{{;]*\{{", text)
            if not feature_match:
                continue
            feature_end = find_matching_brace(text, feature_match.end() - 1)
            if feature_end < 0:
                continue

            raw_feature_body = text[feature_match.end():feature_end]
            members = parse_struct_fields(strip_nested_type_bodies(raw_feature_body))
            for struct_match in re.finditer(STRUCT_DECL_RE, raw_feature_body):
                body_end = find_matching_brace(raw_feature_body, struct_match.end() - 1)
                if body_end < 0:
                    continue
                tail = raw_feature_body[body_end + 1:raw_feature_body.find(";", body_end)]
                for member_match in re.finditer(r"\b([A-Za-z_]\w*)\b", tail):
                    members.setdefault(member_match.group(1), struct_match.group(1))
            feature_members[feature_class] = members
    return feature_members


def collect_save_roots(paths: list[Path]) -> dict[str, str]:
    roots: dict[str, str] = {}
    for path in paths:
        text = read_text(path)
        for match in re.finditer(r"\bvoid\s+(\w+)::SaveSettings\s*\([^)]*\)\s*\{", text):
            feature_class = match.group(1)
            body_end = find_matching_brace(text, match.end() - 1)
            if body_end < 0:
                continue
            body = text[match.end():body_end]
            assignment = re.search(r"\b(?:\w+|this->\w+)\s*=\s*(?:this->)?([A-Za-z_]\w*)\s*;", body)
            if assignment:
                roots[feature_class] = assignment.group(1)
    return roots


def collect_features(paths: list[Path]) -> dict[str, dict[str, str]]:
    features: dict[str, dict[str, str]] = {}
    for path in paths:
        text = read_text(path)
        class_match = re.search(r"\b(?:struct|class)\s+(\w+)[^{:;]*(?::[^{]+Feature)", text)
        if not class_match:
            continue
        class_name = class_match.group(1)
        short_match = re.search(r"GetShortName\(\)\s*(?:const\s*)?(?:override\s*)?\{[^{}]*return\s+\"([^\"]+)\"", text)
        if not short_match:
            continue
        name_match = re.search(r"GetName\(\)\s*(?:const\s*)?(?:override\s*)?\{[^{}]*return\s+\"([^\"]+)\"", text)
        features[class_name] = {
            "short": short_match.group(1),
            "name": name_match.group(1) if name_match else short_match.group(1),
            "source": str(path),
        }
    return features


def collect_ui_labels(paths: list[Path]) -> dict[tuple[str, str], tuple[str, str]]:
    labels: dict[tuple[str, str], tuple[str, str]] = {}
    feature_by_file: dict[Path, str] = {}
    for path in paths:
        text = read_text(path)
        match = re.search(r"\b(\w+)::DrawSettings\(", text)
        if match:
            feature_by_file[path] = match.group(1)

    current_category = ""
    for path in paths:
        feature = feature_by_file.get(path)
        if not feature:
            continue
        current_category = ""
        for line in read_text(path).splitlines():
            cat_match = re.search(r"(?:SeparatorText|TreeNodeEx|DrawSectionHeader)\s*\(\s*T\([^,]+,\s*\"([^\"]+)\"", line)
            if cat_match:
                current_category = cat_match.group(1)
            label_match = re.search(r"(?:ImGui|Util)::\w+\s*\(\s*(?:T\([^,]+,\s*\"([^\"]+)\"|\"([^\"]+)\")", line)
            setting_match = re.search(r"&(?:settings|debugSettings)\.([A-Za-z_]\w*)", line)
            if label_match and setting_match:
                label = label_match.group(1) or label_match.group(2) or ""
                label = label.split("##", 1)[0]
                labels[(feature, setting_match.group(1))] = (label, current_category)
    return labels


def type_to_value_type(type_name: str) -> str | None:
    cleaned = clean_type(type_name)
    if cleaned.startswith("std::atomic<"):
        inner = cleaned[len("std::atomic<"):-1].strip()
        cleaned = inner
    if cleaned in PRIMITIVE_TYPES:
        return PRIMITIVE_TYPES[cleaned]
    if cleaned.endswith("::value_type"):
        return None
    if cleaned in {"RE::NiColor", "RE::NiPoint2", "RE::NiPoint3", "DirectX::XMFLOAT2", "DirectX::XMFLOAT3", "DirectX::XMFLOAT4"}:
        return None
    return None


def nested_type_candidates(owner: str, field_type: str) -> list[str]:
    cleaned = clean_type(field_type)
    return [cleaned, f"{owner}::{cleaned}"]


def build_entries(source_dir: Path) -> list[dict[str, str]]:
    src_paths = list((source_dir / "src").rglob("*.cpp")) + list((source_dir / "src").rglob("*.h"))
    src_paths += [source_dir / "src" / "TruePBR.cpp", source_dir / "src" / "TruePBR.h"]
    src_paths = sorted({p for p in src_paths if p.exists()})

    macros = collect_nlohmann_macros(src_paths)
    struct_bodies = collect_struct_bodies(src_paths)
    struct_fields = {name: parse_struct_fields(body) for name, body in struct_bodies.items()}
    features = collect_features([p for p in src_paths if p.suffix == ".h"])
    feature_fields = collect_feature_struct_fields([p for p in src_paths if p.suffix == ".h"], features)
    feature_members = collect_feature_member_fields([p for p in src_paths if p.suffix == ".h"], features)
    save_roots = collect_save_roots([p for p in src_paths if p.suffix == ".cpp"])
    labels = collect_ui_labels([p for p in src_paths if p.suffix == ".cpp"])

    entries: list[dict[str, str]] = []
    seen: set[tuple[str, tuple[str, ...], str]] = set()

    def add_entry(feature_class: str, path: list[str], key: str, value_type: str, access: str):
        feature = features.get(feature_class)
        if not feature:
            return
        feature_short = feature["short"]
        identity = (feature_short, tuple(path), key)
        if identity in seen:
            return
        seen.add(identity)
        source_path = Path(feature["source"])
        try:
            include_path = source_path.relative_to(source_dir / "src").as_posix()
        except ValueError:
            include_path = source_path.as_posix()

        label, ui_category = labels.get((feature_class, key), (prettify(key), ""))
        flags = ["SceneSettingsCatalog::SettingFlag::Persisted"]
        if value_type == "Float":
            flags.append("SceneSettingsCatalog::SettingFlag::Transitionable")
        lowered = " ".join(path + [key]).lower()
        if "debug" in lowered:
            flags.append("SceneSettingsCatalog::SettingFlag::Hidden")

        display_path = [p for p in path]
        if ui_category and ui_category not in display_path:
            display_path.insert(0, ui_category)

        entries.append({
            "featureClass": feature_class,
            "feature": feature_short,
            "featureName": feature["name"],
            "include": include_path,
            "path": "/".join(path),
            "key": key,
            "displayName": label,
            "displayPath": "/".join(display_path),
            "type": value_type,
            "flags": " | ".join(flags),
            "access": access,
        })

    def emit_type(feature_class: str, full_type: str, path: list[str], access: str):
        fields = macros.get(full_type)
        if not fields:
            return
        simple_type = full_type.split("::")[-1]
        declared_fields = feature_fields.get(feature_class, {}).get(simple_type, struct_fields.get(simple_type, {}))
        for field in fields:
            field_type = declared_fields.get(field, "")
            value_type = type_to_value_type(field_type)
            field_access = f"{access}.{field}"
            if value_type:
                add_entry(feature_class, path, field, value_type, field_access)
                continue
            emitted_nested = False
            for candidate in nested_type_candidates(feature_class, field_type):
                if candidate in macros:
                    emit_type(feature_class, candidate, path + [field], field_access)
                    emitted_nested = True
                    break
            if not emitted_nested and field_type:
                continue

    for feature_class in sorted(features):
        members = feature_members.get(feature_class, {})
        root_member = save_roots.get(feature_class)
        if not root_member:
            for member_name, member_type in members.items():
                if clean_type(member_type).split("::")[-1] == "Settings":
                    root_member = member_name
                    break
        if not root_member:
            continue

        root_type = clean_type(members.get(root_member, ""))
        if not root_type:
            continue
        root_full_type = f"{feature_class}::{root_type.split('::')[-1]}"
        emit_type(feature_class, root_full_type, [], root_member)

    entries.sort(key=lambda e: (e["feature"], e["displayPath"], e["displayName"], e["path"], e["key"]))
    return entries


def write_catalog(entries: list[dict[str, str]], out_dir: Path):
    out_dir.mkdir(parents=True, exist_ok=True)
    header = out_dir / "SceneSettingsCatalog.generated.h"
    source = out_dir / "SceneSettingsCatalog.generated.cpp"

    header.write_text("""#pragma once

#include <span>
#include <string_view>
#include <cstdint>

struct Feature;

namespace SceneSettingsCatalog
{
\tenum class ValueType : std::uint8_t
\t{
\t\tBoolean,
\t\tInteger,
\t\tFloat,
\t\tString,
\t};

\tenum class SettingFlag : std::uint32_t
\t{
\t\tNone = 0,
\t\tPersisted = 1u << 0,
\t\tTransitionable = 1u << 1,
\t\tHidden = 1u << 2,
\t};

\tconstexpr SettingFlag operator|(SettingFlag lhs, SettingFlag rhs)
\t{
\t\treturn static_cast<SettingFlag>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
\t}

\tconstexpr bool HasFlag(SettingFlag flags, SettingFlag flag)
\t{
\t\treturn (static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(flag)) != 0;
\t}

\tstruct SettingMetadata
\t{
\t\tstd::string_view featureShortName;
\t\tstd::string_view featureDisplayName;
\t\tstd::string_view settingPath;
\t\tstd::string_view settingKey;
\t\tstd::string_view displayName;
\t\tstd::string_view displayPath;
\t\tValueType valueType;
\t\tSettingFlag flags;
\t};

\tstd::span<const SettingMetadata> GetSettings();
\tconst SettingMetadata* FindSetting(std::string_view featureShortName, std::string_view settingPath, std::string_view settingKey);
\tconst SettingMetadata* FindSettingForControl(Feature* feature, const void* valueAddress);
}
""", encoding="utf-8")

    rows = []
    for e in entries:
        rows.append(
            f'\t\t{{ "{cpp_escape(e["feature"])}", "{cpp_escape(e["featureName"])}", '
            f'"{cpp_escape(e["path"])}", "{cpp_escape(e["key"])}", "{cpp_escape(e["displayName"])}", "{cpp_escape(e["displayPath"])}", '
            f'SceneSettingsCatalog::ValueType::{e["type"]}, {e["flags"]} }},'
        )
    joined_rows = "\n".join(rows)
    includes = "\n".join(f'#include "{cpp_escape(include_path)}"' for include_path in sorted({e["include"] for e in entries}))
    pointer_checks = []
    for index, e in enumerate(entries):
        pointer_checks.append(
            f'\t\tif (valueAddress == static_cast<const void*>(&typedFeature->{e["access"]}))\n'
            f'\t\t\treturn &kSceneSettings[{index}];'
        )

    feature_blocks = []
    for feature_short in sorted({e["feature"] for e in entries}):
        feature_entries = [e for e in entries if e["feature"] == feature_short]
        feature_class = feature_entries[0]["featureClass"]
        checks = "\n".join(pointer_checks[i] for i, e in enumerate(entries) if e["feature"] == feature_short)
        feature_blocks.append(f'''\t\tif (featureShortName == "{cpp_escape(feature_short)}") {{
\t\t\tauto* typedFeature = static_cast<{feature_class}*>(feature);
{checks}
\t\t\treturn nullptr;
\t\t}}''')
    joined_feature_blocks = "\n".join(feature_blocks)
    source.write_text(f"""#include "SceneSettingsCatalog.generated.h"

#include "Feature.h"
{includes}

#include <array>

namespace
{{
\tstatic constexpr std::array<SceneSettingsCatalog::SettingMetadata, {len(entries)}> kSceneSettings = {{{{
{joined_rows}
\t}}}};
}}

namespace SceneSettingsCatalog
{{
\tstd::span<const SettingMetadata> GetSettings()
\t{{
\t\treturn kSceneSettings;
\t}}

\tconst SettingMetadata* FindSetting(std::string_view featureShortName, std::string_view settingPath, std::string_view settingKey)
\t{{
\t\tfor (const auto& setting : kSceneSettings) {{
\t\t\tif (setting.featureShortName == featureShortName &&
\t\t\t\tsetting.settingPath == settingPath &&
\t\t\t\tsetting.settingKey == settingKey) {{
\t\t\t\treturn &setting;
\t\t\t}}
\t\t}}
\t\treturn nullptr;
\t}}

\tconst SettingMetadata* FindSettingForControl(Feature* feature, const void* valueAddress)
\t{{
\t\tif (!feature || !valueAddress)
\t\t\treturn nullptr;

\t\tconst auto featureShortName = feature->GetShortName();
{joined_feature_blocks}
\t\treturn nullptr;
\t}}
}}
""", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--out-dir", required=True)
    args = parser.parse_args()

    source_dir = Path(args.source_dir)
    out_dir = Path(args.out_dir)
    entries = build_entries(source_dir)
    write_catalog(entries, out_dir)
    print(f"Generated {len(entries)} scene setting catalog entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
