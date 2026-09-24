#!/usr/bin/env python3
"""Advanced Skin authoring: explicit NIF surfaces, material drafts and plugin packages."""

from __future__ import annotations

import argparse
import copy
import json
import math
import os
from pathlib import Path
import re
import struct
import tempfile
import uuid


ROOT = Path(__file__).resolve().parents[1]
MAX_JSON = 4 * 1024 * 1024
MAX_PAYLOAD = 64 * 1024
SHAPES = {"BSTriShape", "BSDynamicTriShape", "BSSubIndexTriShape"}


def parameter_table():
    table = {}
    source = (ROOT / "src/Features/Skin/SkinParameters.def").read_text(encoding="utf-8")
    for kind, args in re.findall(r"SKIN_(FLOAT|BOOL)\(([^)]+)\)", source):
        parts = [part.strip() for part in args.split(",")]
        if kind == "BOOL":
            table[parts[0]] = (parts[1] == "true", None, None)
        else:
            table[parts[0]] = tuple(float(p.rstrip("f")) for p in parts[1:])
    if len(table) != 20:
        raise ValueError("Expected the v1 parameter table (20 fields)")
    return table


PARAMETERS = parameter_table()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON key: {key}")
        result[key] = value
    return result


def read_json(path):
    with Path(path).open("rb") as stream:
        data = stream.read(MAX_JSON + 1)
    if len(data) > MAX_JSON:
        raise ValueError("JSON exceeds 4 MiB")
    value = json.loads(data.decode("utf-8-sig"), object_pairs_hook=unique_object)
    pending = [(value, 0)]
    while pending:
        node, depth = pending.pop()
        if depth > 16:
            raise ValueError("JSON nesting exceeds 16 levels")
        if isinstance(node, dict):
            pending.extend((child, depth + 1) for child in node.values())
        elif isinstance(node, list):
            pending.extend((child, depth + 1) for child in node)
    return value


def read_nif(path):
    with Path(path).open("rb") as stream:
        data = stream.read(256 * 1024 * 1024 + 1)
    return Nif(data)


def fields(value, allowed, required=None):
    if not isinstance(value, dict) or set(value) - set(allowed):
        raise ValueError(f"Expected object with fields {allowed}")
    if set(allowed if required is None else required) - set(value):
        raise ValueError(f"Missing required fields in {allowed}")


def parameters(value, complete=False):
    fields(value, PARAMETERS, PARAMETERS if complete else ())
    for key, number in value.items():
        default, low, high = PARAMETERS[key]
        if type(default) is bool:
            if type(number) is not bool:
                raise ValueError(f"{key} must be a boolean")
        elif type(number) not in (int, float) or not math.isfinite(number) or not low <= number <= high:
            raise ValueError(f"{key} must be finite and between {low} and {high}")
    return copy.deepcopy(value)


def texture_path(value):
    if value is None:
        return None
    if not isinstance(value, str):
        raise ValueError("Texture must be null or a Data-relative DDS path")
    value = value.replace("\\", "/").lower()
    if (len(value.encode("utf-8")) > 260 or not value.startswith("textures/") or not value.endswith(".dds")
            or any(c in value for c in ':*?"<>|') or any(ord(c) < 32 for c in value)
            or any(p in ("", ".", "..") or p.endswith((" ", ".")) for p in value.split("/"))):
        raise ValueError("Texture must be a Data-relative textures/...dds path without traversal")
    return value


def material(value):
    fields(value, ("name", "parameters", "textures"))
    name = value["name"]
    if not isinstance(name, str) or not name or len(name.encode("utf-8")) > 128 or any(ord(c) < 32 for c in name):
        raise ValueError("Material name must contain 1-128 UTF-8 bytes")
    fields(value["textures"], ("rfaos", "wetness"))
    return {"name": name, "parameters": parameters(value["parameters"], True),
            "textures": {k: texture_path(v) for k, v in value["textures"].items()}}


def surface_id(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", value):
        raise ValueError("surfaceId must be a UUID")
    if uuid.UUID(value).int == 0:
        raise ValueError("surfaceId cannot be the nil UUID")
    return value.lower()


def form_key(value):
    fields(value, ("plugin", "localFormId"))
    plugin, local = value["plugin"], value["localFormId"]
    if (not isinstance(plugin, str) or not re.fullmatch(r'[^/\\:|*?"<>\x00-\x1f]+\.(esm|esp|esl)', plugin, re.I)
            or len(plugin.encode("utf-8")) > 260):
        raise ValueError("Invalid plugin filename")
    if not isinstance(local, str) or not re.fullmatch(r"[0-9a-fA-F]{1,8}", local) or not 0 < int(local, 16) <= 0xFFFFFF:
        raise ValueError("localFormId must be hexadecimal without a load-order prefix")
    return {"plugin": plugin.lower(), "localFormId": f"{int(local, 16):08X}"}


def validate_document(data):
    if not isinstance(data, dict) or type(data.get("schemaVersion")) is not int or data["schemaVersion"] != 1:
        raise ValueError("Unsupported schemaVersion")
    if "ownerPlugin" in data:
        fields(data, ("schemaVersion", "ownerPlugin", "recordMaterials", "raceAdjustments", "npcAdjustments"))
        form_key({"plugin": data["ownerPlugin"], "localFormId": "1"})
        for array, target in (("recordMaterials", "txst"), ("raceAdjustments", "race"), ("npcAdjustments", "npc")):
            rows = data[array]
            if not isinstance(rows, list) or len(rows) > 4096:
                raise ValueError(f"Invalid {array}")
            seen = set()
            for row in rows:
                payload = "material" if target == "txst" else "parameters"
                fields(row, (target, payload))
                row[target] = form_key(row[target])
                key = tuple(row[target].values())
                if key in seen:
                    raise ValueError(f"Duplicate {target}: {key}")
                seen.add(key)
                row[payload] = material(row[payload]) if target == "txst" else parameters(row[payload])
    else:
        fields(data, ("schemaVersion", "surfaceId", "material"), ("schemaVersion", "material"))
        data["material"] = material(data["material"])
        if "surfaceId" in data:
            data["surfaceId"] = surface_id(data["surfaceId"])
    return data


def atomic_write(path, data, overwrite=False):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and not overwrite:
        raise ValueError(f"Output already exists: {path}")
    fd, temporary = tempfile.mkstemp(prefix=".skin-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if overwrite:
            os.replace(temporary, path)
        else:
            # The publisher must not overwrite an asset created after the existence check.
            os.link(temporary, path)
            os.unlink(temporary)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def write_json(path, data, overwrite=False):
    encoded = (json.dumps(data, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
    if len(encoded) > MAX_JSON:
        raise ValueError("Output JSON exceeds 4 MiB")
    atomic_write(path, encoded, overwrite)


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, count):
        if count < 0 or self.pos + count > len(self.data):
            raise ValueError("Truncated NIF")
        result = self.data[self.pos:self.pos + count]
        self.pos += count
        return result

    def number(self, fmt="I"):
        return struct.unpack("<" + fmt, self.take(struct.calcsize("<" + fmt)))[0]

    def string(self, fmt="I"):
        length = self.number(fmt)
        if length > MAX_JSON:
            raise ValueError("Oversized NIF string")
        return self.take(length)


def u32(value):
    return struct.pack("<I", value)


def sized(value):
    return u32(len(value)) + value


class Nif:
    """SSE stream 100; preserve unedited blocks verbatim and never renumber existing blocks."""

    def __init__(self, data):
        if len(data) > 256 * 1024 * 1024:
            raise ValueError("NIF exceeds 256 MiB")
        r = Reader(data)
        header = b"Gamebryo File Format, Version 20.2.0.7\n"
        if r.take(len(header)) != header or r.number() != 0x14020007 or r.number("B") != 1 or r.number() != 12:
            raise ValueError("Only little-endian Skyrim SE NIF 20.2.0.7/user 12 is supported")
        count = r.number()
        if count > 100000:
            raise ValueError("Too many NIF blocks")
        if r.number() != 100:
            raise ValueError("Only Skyrim SE stream 100 is supported")
        for _ in range(3):
            r.string("B")
        self.prefix = data[:r.pos]
        types = r.number("H")
        self.types = [r.string() for _ in range(types)]
        self.indices = [r.number("H") for _ in range(count)]
        if any(i >= types for i in self.indices):
            raise ValueError("Invalid block type index")
        sizes = [r.number() for _ in range(count)]
        strings = r.number()
        r.number()
        if strings > 200000:
            raise ValueError("Too many NIF strings")
        self.strings = [r.string() for _ in range(strings)]
        groups = r.number()
        if groups:
            raise ValueError("Grouped NIF streams are not supported")
        self.blocks = [r.take(size) for size in sizes]
        self.footer = data[r.pos:]
        footer = Reader(self.footer)
        roots = footer.number()
        if roots > count or len(self.footer) != 4 + roots * 4:
            raise ValueError("Invalid NIF footer")
        if any(footer.number() >= count for _ in range(roots)):
            raise ValueError("Invalid NIF root")

    def typename(self, block):
        return self.types[self.indices[block]].decode("ascii")

    def string_at(self, index):
        if index == 0xFFFFFFFF:
            return ""
        if index >= len(self.strings):
            raise ValueError("Invalid NIF string reference")
        return self.strings[index].decode("utf-8")

    def shape(self, block):
        if not 0 <= block < len(self.blocks) or self.typename(block) not in SHAPES:
            raise ValueError("Select a supported geometry block")
        r = Reader(self.blocks[block])
        name = self.string_at(r.number())
        count = r.number()
        if count > 4096:
            raise ValueError("Too many extra data links")
        extras = [r.number() for _ in range(count)]
        if any(i >= len(self.blocks) for i in extras):
            raise ValueError("Invalid extra data reference")
        end = r.pos
        r.take(4 + 4 + 12 + 36 + 4 + 4 + 16 + 4)
        shader = r.number()
        if shader >= len(self.blocks) or self.typename(shader) != "BSLightingShaderProperty":
            raise ValueError("Surface has no BSLightingShaderProperty")
        shader_type = Reader(self.blocks[shader]).number()
        if shader_type not in (4, 5):
            raise ValueError("Surface must use Face Tint (4) or Skin Tint (5)")
        payloads = []
        for index in extras:
            # Unknown extra-data subclasses stay opaque.
            if self.typename(index) == "NiStringExtraData":
                extra = Reader(self.blocks[index])
                if self.string_at(extra.number()) == "CSSkinMaterial":
                    if len(self.blocks[index]) != 8:
                        raise ValueError("Invalid NiStringExtraData block size")
                    text = self.string_at(extra.number())
                    if len(text.encode("utf-8")) > MAX_PAYLOAD:
                        raise ValueError("CSSkinMaterial exceeds 64 KiB")
                    payloads.append((index, text))
            elif self.typename(index).startswith("Ni") and self.typename(index).endswith("ExtraData") and len(self.blocks[index]) >= 4:
                if self.string_at(Reader(self.blocks[index]).number()) == "CSSkinMaterial":
                    raise ValueError("CSSkinMaterial has the wrong extra data type")
        if len(payloads) > 1:
            raise ValueError("Duplicate CSSkinMaterial blocks")
        return name, extras, end, payloads

    def surfaces(self):
        result = []
        for block in range(len(self.blocks)):
            if self.typename(block) in SHAPES:
                try:
                    name, _, _, payloads = self.shape(block)
                    result.append((block, name, payloads[0][1] if payloads else ""))
                except ValueError:
                    continue
        return result

    def add_string(self, text):
        value = text.encode("utf-8")
        if value not in self.strings:
            self.strings.append(value)
        return self.strings.index(value)

    def assign(self, block, draft, identity=None, independent=False):
        name, extras, end, payloads = self.shape(block)
        if identity:
            identity = surface_id(identity)
        elif payloads and not independent:
            previous = validate_document(json.loads(payloads[0][1], object_pairs_hook=unique_object))
            identity = surface_id(previous["surfaceId"])
        else:
            identity = str(uuid.uuid4())
        payload = {"schemaVersion": 1, "surfaceId": identity, "material": material(draft)}
        encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
        if len(encoded.encode("utf-8")) > MAX_PAYLOAD:
            raise ValueError("Material exceeds 64 KiB")
        kind = b"NiStringExtraData"
        if kind not in self.types:
            self.types.append(kind)
        new_index = len(self.blocks)
        self.indices.append(self.types.index(kind))
        self.blocks.append(u32(self.add_string("CSSkinMaterial")) + u32(self.add_string(encoded)))
        old = {item[0] for item in payloads}
        extras = [i for i in extras if i not in old] + [new_index]
        original = self.blocks[block]
        self.blocks[block] = original[:4] + u32(len(extras)) + b"".join(u32(i) for i in extras) + original[end:]
        return identity

    def encode(self):
        prefix = bytearray(self.prefix)
        header_length = len(b"Gamebryo File Format, Version 20.2.0.7\n")
        struct.pack_into("<I", prefix, header_length + 4 + 1 + 4, len(self.blocks))
        return (bytes(prefix) + struct.pack("<H", len(self.types)) + b"".join(sized(t) for t in self.types)
                + b"".join(struct.pack("<H", i) for i in self.indices)
                + b"".join(u32(len(b)) for b in self.blocks) + u32(len(self.strings))
                + u32(max(map(len, self.strings), default=0)) + b"".join(sized(s) for s in self.strings)
                + u32(0) + b"".join(self.blocks) + self.footer)


def publish(input_path, block, draft, output, identity=None, independent=False):
    if Path(input_path).resolve() == Path(output).resolve():
        raise ValueError("Publish to a staging copy, not the source NIF")
    if identity and independent:
        raise ValueError("Choose an explicit shared UUID or a new independent identity, not both")
    nif = read_nif(input_path)
    identity = nif.assign(block, draft, identity, independent)
    encoded = nif.encode()
    check = Nif(encoded)
    checked = json.loads(check.shape(block)[3][0][1], object_pairs_hook=unique_object)
    validate_document(checked)
    if checked["surfaceId"] != identity or checked["material"] != material(draft):
        raise ValueError("NIF round-trip verification failed")
    atomic_write(output, encoded)
    return identity


def legacy_drafts(value):
    """Extract payloads only. Legacy selectors never become binding identities."""
    result = []

    def visit(node, trail):
        if isinstance(node, dict):
            selected = {key: val for key, val in node.items() if key in PARAMETERS}
            if selected:
                parameters(selected)
                result.append({"label": trail, "parameters": selected, "complete": set(selected) == set(PARAMETERS)})
            for key, child in node.items():
                visit(child, trail + "/" + key)
        elif isinstance(node, list):
            for i, child in enumerate(node):
                visit(child, trail + f"/{i}")

    visit(value, "legacy")
    return result


def gui():
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    window = tk.Tk()
    window.title("Advanced Skin — Material Authoring")
    window.geometry("900x850")
    state = {"input": None, "nif": None, "surface": None}
    values = {key: (tk.BooleanVar(value=default) if type(default) is bool else tk.StringVar(value=str(default)))
              for key, (default, _, _) in PARAMETERS.items()}
    name, rfaos, wetness = tk.StringVar(value="Skin material"), tk.StringVar(), tk.StringVar()
    identity, notice = tk.StringVar(), tk.StringVar(value="Load a NIF and explicitly select its skin surface.")
    independent = tk.BooleanVar(value=False)

    def guarded(action):
        try:
            action()
        except (ValueError, KeyError, OSError, struct.error, UnicodeError) as exc:
            messagebox.showerror("Advanced Skin", str(exc))

    def get_material():
        params = {k: v.get() if type(PARAMETERS[k][0]) is bool else float(v.get()) for k, v in values.items()}
        return material({"name": name.get(), "parameters": params,
                         "textures": {"rfaos": rfaos.get() or None, "wetness": wetness.get() or None}})

    def set_material(data):
        data = material(data)
        name.set(data["name"])
        for key, val in data["parameters"].items():
            values[key].set(val)
        rfaos.set(data["textures"]["rfaos"] or "")
        wetness.set(data["textures"]["wetness"] or "")

    def load_nif():
        path = filedialog.askopenfilename(filetypes=[("Skyrim SE NIF", "*.nif")])
        if not path:
            return
        nif = read_nif(path)
        rows.delete(*rows.get_children())
        for block, label, payload in nif.surfaces():
            rows.insert("", "end", iid=str(block), values=(block, label, bool(payload)))
        state.update(input=path, nif=nif, surface=None)
        identity.set("")
        notice.set(path)

    def select_surface(event=None):
        selection = rows.selection()
        if not selection:
            return
        block = int(selection[0])
        state["surface"] = block
        identity.set("")
        payloads = state["nif"].shape(block)[3]
        if payloads:
            data = validate_document(json.loads(payloads[0][1], object_pairs_hook=unique_object))
            identity.set(data["surfaceId"])
            set_material(data["material"])
        else:
            identity.set(str(uuid.uuid4()))

    def load_draft():
        path = filedialog.askopenfilename(filetypes=[("Material draft", "*.json")])
        if path:
            set_material(validate_document(read_json(path))["material"])

    def save_draft():
        data = {"schemaVersion": 1, "material": get_material()}
        path = filedialog.asksaveasfilename(defaultextension=".skinmaterial.json")
        if path:
            write_json(path, data, overwrite=True)
            notice.set("Saved material draft: " + path)

    def publish_nif():
        if state["surface"] is None:
            raise ValueError("Select a NIF surface first")
        draft = get_material()
        chosen_id = str(uuid.uuid4()) if independent.get() else surface_id(identity.get())
        path = filedialog.asksaveasfilename(defaultextension=".nif", title="Publish to a NEW staging NIF")
        if path:
            published = publish(state["input"], state["surface"], draft, path, chosen_id)
            identity.set(published)
            notice.set("Published and re-read: " + path)

    def import_legacy():
        path = filedialog.askopenfilename(filetypes=[("Legacy JSON", "*.json")])
        if not path:
            return
        drafts = legacy_drafts(read_json(path))
        dialog = tk.Toplevel(window)
        dialog.title("Import parameters — select fields explicitly")
        choices = tk.Listbox(dialog, width=90, height=12)
        choices.pack(fill="both", expand=True)
        for row in drafts:
            choices.insert("end", row["label"] + (" [all 20 fields]" if row["complete"] else ""))
        checks = {}
        frame = ttk.Frame(dialog)
        frame.pack(fill="both")

        def pick(event=None):
            for widget in frame.winfo_children():
                widget.destroy()
            checks.clear()
            if choices.curselection():
                for i, (key, value) in enumerate(drafts[choices.curselection()[0]]["parameters"].items()):
                    checks[key] = tk.BooleanVar(value=False)
                    ttk.Checkbutton(frame, text=f"{key}: {value}", variable=checks[key]).grid(row=i // 2, column=i % 2, sticky="w")

        def apply_import():
            if choices.curselection():
                selected = drafts[choices.curselection()[0]]["parameters"]
                for key, enabled in checks.items():
                    if enabled.get():
                        values[key].set(selected[key])
                notice.set("Imported selected parameters. Choose a NIF surface or record explicitly before publishing.")
                dialog.destroy()

        choices.bind("<<ListboxSelect>>", pick)
        ttk.Button(dialog, text="Import checked fields", command=apply_import).pack()

    def edit_package():
        dialog = tk.Toplevel(window)
        dialog.title("Plugin material package — exact records")
        owner = tk.StringVar(value="MySkin.esp")
        plugin, local = tk.StringVar(value="Skyrim.esm"), tk.StringVar()
        kind = tk.StringVar(value="npcAdjustments")
        package = {"schemaVersion": 1, "ownerPlugin": owner.get(), "recordMaterials": [], "raceAdjustments": [], "npcAdjustments": []}
        checked = {key: tk.BooleanVar(value=False) for key in PARAMETERS}
        for i, (label, variable) in enumerate((("Owner plugin (must be loaded)", owner),
                                             ("Target's original plugin", plugin), ("Target local hexadecimal FormID", local))):
            ttk.Label(dialog, text=label).grid(row=i, column=0, sticky="w", padx=8, pady=3)
            ttk.Entry(dialog, textvariable=variable, width=48).grid(row=i, column=1, sticky="ew", padx=8)
        ttk.Combobox(dialog, textvariable=kind, values=("npcAdjustments", "raceAdjustments", "recordMaterials"), state="readonly").grid(row=3, column=0, columnspan=2, sticky="ew", padx=8)
        ttk.Label(dialog, text="NPC/race: check the parameters to take from the main editor.\nTXST: assign the complete material. Verify its actual use in-game.", wraplength=620).grid(row=4, column=0, columnspan=2, padx=8)
        checks = ttk.Frame(dialog)
        checks.grid(row=5, column=0, columnspan=2)
        for i, (key, value) in enumerate(checked.items()):
            ttk.Checkbutton(checks, text=key, variable=value).grid(row=i // 2, column=i % 2, sticky="w")
        listing = tk.Listbox(dialog, width=95, height=8)
        listing.grid(row=6, column=0, columnspan=2, sticky="ew", padx=8)
        targets = {"recordMaterials": "txst", "raceAdjustments": "race", "npcAdjustments": "npc"}
        list_keys = []

        def refresh():
            listing.delete(0, "end")
            list_keys.clear()
            for array, target in targets.items():
                for index, entry in enumerate(package[array]):
                    listing.insert("end", f"{target}: {entry[target]['plugin']} | {entry[target]['localFormId']}")
                    list_keys.append((array, index))

        def add():
            key = form_key({"plugin": plugin.get(), "localFormId": local.get()})
            draft = get_material()
            array, target = kind.get(), targets[kind.get()]
            entry = {target: key}
            if target == "txst":
                entry["material"] = draft
            else:
                entry["parameters"] = {k: draft["parameters"][k] for k, enabled in checked.items() if enabled.get()}
                if not entry["parameters"]:
                    raise ValueError("Check at least one parameter to adjust")
            for index, old in enumerate(package[array]):
                if old[target] == key:
                    if not messagebox.askyesno("Replace entry", "Replace the existing entry for this exact record?", parent=dialog):
                        return
                    package[array][index] = entry
                    break
            else:
                package[array].append(entry)
            refresh()

        def remove():
            if listing.curselection():
                array, index = list_keys[listing.curselection()[0]]
                del package[array][index]
                refresh()

        def load():
            path = filedialog.askopenfilename(filetypes=[("Plugin material package", "*.skin.json")], parent=dialog)
            if path:
                data = validate_document(read_json(path))
                if "ownerPlugin" not in data:
                    raise ValueError("Expected a plugin material package")
                package.clear()
                package.update(data)
                owner.set(package["ownerPlugin"])
                refresh()

        def save():
            package["ownerPlugin"] = owner.get()
            validate_document(package)
            path = filedialog.asksaveasfilename(initialfile=owner.get() + ".skin.json", parent=dialog)
            if path:
                if Path(path).name.lower() != (owner.get() + ".skin.json").lower():
                    raise ValueError("Filename must be ownerPlugin + .skin.json")
                write_json(path, package, overwrite=True)
                notice.set("Published exact-record package: " + path)

        buttons = ttk.Frame(dialog)
        buttons.grid(row=7, column=0, columnspan=2, pady=10)
        for label, action in (("Add / replace target", add), ("Remove selected", remove), ("Load package", load), ("Save package", save)):
            ttk.Button(buttons, text=label, command=lambda fn=action: guarded(fn)).pack(side="left", padx=3)

    toolbar = ttk.Frame(window)
    toolbar.pack(fill="x", padx=10, pady=10)
    for label, callback in (("Open NIF", load_nif), ("Load material draft", load_draft),
                            ("Save material draft", save_draft), ("Import legacy parameters", import_legacy), ("Plugin package", edit_package)):
        ttk.Button(toolbar, text=label, command=lambda action=callback: guarded(action)).pack(side="left", padx=3)
    rows = ttk.Treeview(window, columns=("block", "surface", "authored"), show="headings", height=5, selectmode="browse")
    for column in rows["columns"]:
        rows.heading(column, text=column.title())
    rows.pack(fill="x", padx=10)
    rows.bind("<<TreeviewSelect>>", lambda event: guarded(select_surface))
    form = ttk.Frame(window)
    form.pack(fill="x", padx=10, pady=8)
    for i, (label, value) in enumerate((("Material name", name), ("RFAOS (textures/...dds or empty)", rfaos),
                                      ("Wetness (textures/...dds or empty)", wetness), ("Surface UUID (reuse for _0/_1 variants)", identity))):
        ttk.Label(form, text=label).grid(row=i, column=0, sticky="w")
        ttk.Entry(form, textvariable=value, width=72).grid(row=i, column=1, sticky="ew")
    ttk.Checkbutton(form, text="Independent asset: generate a new UUID when publishing", variable=independent).grid(row=4, column=1, sticky="w")
    grid = ttk.LabelFrame(window, text="Material parameters")
    grid.pack(fill="x", padx=10, pady=8)
    for i, (key, value) in enumerate(values.items()):
        row, col = i // 2, (i % 2) * 2
        default, low, high = PARAMETERS[key]
        ttk.Label(grid, text=key).grid(row=row, column=col, sticky="w", padx=6, pady=2)
        widget = ttk.Checkbutton(grid, variable=value) if type(default) is bool else ttk.Spinbox(grid, textvariable=value, from_=low, to=high, increment=0.01, width=14)
        widget.grid(row=row, column=col + 1, padx=6)
    ttk.Button(window, text="Publish NIF copy", command=lambda: guarded(publish_nif)).pack(pady=8)
    ttk.Label(window, textvariable=notice, wraplength=850).pack(fill="x", padx=10)
    window.mainloop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command")
    commands.add_parser("gui")
    inspect = commands.add_parser("inspect")
    inspect.add_argument("nif", type=Path)
    validate = commands.add_parser("validate")
    validate.add_argument("json", type=Path)
    publish_cmd = commands.add_parser("publish")
    publish_cmd.add_argument("nif", type=Path)
    publish_cmd.add_argument("block", type=int)
    publish_cmd.add_argument("draft", type=Path)
    publish_cmd.add_argument("output", type=Path)
    publish_cmd.add_argument("--surface-id")
    publish_cmd.add_argument("--independent", action="store_true")
    args = parser.parse_args()
    try:
        if args.command in (None, "gui"):
            gui()
        elif args.command == "inspect":
            for block, name, payload in read_nif(args.nif).surfaces():
                print(block, name, "authored" if payload else "unassigned", sep="\t")
        elif args.command == "validate":
            validate_document(read_json(args.json))
            print("Valid v1 material document (record existence and resources require the game/xEdit).")
        elif args.command == "publish":
            draft = validate_document(read_json(args.draft))["material"]
            print(publish(args.nif, args.block, draft, args.output, args.surface_id, args.independent))
    except (ValueError, KeyError, OSError, struct.error, UnicodeError) as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    main()
