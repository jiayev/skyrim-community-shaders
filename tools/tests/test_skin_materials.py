import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import skin_materials as skin


def draft():
    return {"name": "Fixture", "parameters": {key: row[0] for key, row in skin.PARAMETERS.items()},
            "textures": {"rfaos": None, "wetness": None}}


def fixture():
    header = b"Gamebryo File Format, Version 20.2.0.7\n"
    transform = struct.pack("<13f", 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1)
    shape = (skin.u32(0) + skin.u32(0) + skin.u32(0xFFFFFFFF) + skin.u32(0xE)
             + transform + skin.u32(0xFFFFFFFF) + struct.pack("<4f", 0, 0, 0, 1)
             + skin.u32(0xFFFFFFFF) + skin.u32(1) + b"opaque vertex data")
    shader = skin.u32(5) + b"opaque lighting property"
    unknown = b"opaque block with references\x00\x01\xff"
    blocks = [shape, shader, unknown]
    types = [b"BSTriShape", b"BSLightingShaderProperty", b"UnknownBlock"]
    return (header + skin.u32(0x14020007) + b"\x01" + skin.u32(12) + skin.u32(3) + skin.u32(100)
            + b"\x01\x00" * 3 + struct.pack("<H", 3) + b"".join(skin.sized(t) for t in types)
            + struct.pack("<3H", 0, 1, 2) + b"".join(skin.u32(len(b)) for b in blocks)
            + skin.u32(1) + skin.u32(4) + skin.sized(b"Body") + skin.u32(0)
            + b"".join(blocks) + skin.u32(1) + skin.u32(0))


class MaterialProtocolTests(unittest.TestCase):
    def test_complete_material_does_not_inherit(self):
        value = draft()
        del value["textures"]["wetness"]
        with self.assertRaises(ValueError):
            skin.material(value)
        value = draft()
        del value["parameters"]["F0"]
        with self.assertRaises(ValueError):
            skin.material(value)

    def test_patch_is_sparse_and_typed(self):
        self.assertEqual(skin.parameters({"UseSSS": False}), {"UseSSS": False})
        for value in ({"UseSSS": 1}, {"F0": True}, {"F0": None}, {"F0": float("nan")}, {"sssWidth": 0}, {"unknown": 1}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                skin.parameters(value)

    def test_metadata_boundaries(self):
        for name, (default, low, high) in skin.PARAMETERS.items():
            if type(default) is not bool:
                skin.parameters({name: low})
                skin.parameters({name: high})
                with self.assertRaises(ValueError):
                    skin.parameters({name: high + 1})

    def test_paths(self):
        self.assertEqual(skin.texture_path("Textures\\Skin\\Body.dds"), "textures/skin/body.dds")
        for path in ("C:/textures/a.dds", "textures/../a.dds", "textures//a.dds", "textures/a:stream.dds", "http://x/a.dds", "textures/x /a.dds", "textures/./a.dds"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                skin.texture_path(path)

    def test_duplicate_json_keys(self):
        with self.assertRaises(ValueError):
            json.loads('{"material":{"name":"a","name":"b"}}', object_pairs_hook=skin.unique_object)

    def test_plugin_duplicates_and_prefixes(self):
        key = {"plugin": "MySkin.esp", "localFormId": "00000800"}
        row = {"txst": key, "material": draft()}
        package = {"schemaVersion": 1, "ownerPlugin": "MySkin.esp", "recordMaterials": [row, copy.deepcopy(row)], "raceAdjustments": [], "npcAdjustments": []}
        with self.assertRaises(ValueError):
            skin.validate_document(package)
        with self.assertRaises(ValueError):
            skin.form_key({"plugin": "MySkin.esl", "localFormId": "FE123800"})

    def test_legacy_import_never_converts_selectors(self):
        old = {"rules": [{"match": {"normalGlob": "*"}, "parameters": {"F0": .04}}], "RaceProfiles": {"NordRace": draft()["parameters"]}}
        result = skin.legacy_drafts(old)
        self.assertEqual(len(result), 2)
        self.assertNotIn("normalGlob", json.dumps(result))
        self.assertTrue(result[1]["complete"])


class NifPublisherTests(unittest.TestCase):
    def test_noop_preserves_every_byte(self):
        data = fixture()
        self.assertEqual(skin.Nif(data).encode(), data)

    def test_publish_preserves_native_blocks_and_shape_body(self):
        original = skin.Nif(fixture())
        nif = skin.Nif(fixture())
        identity = nif.assign(0, draft())
        output = skin.Nif(nif.encode())
        self.assertEqual(output.blocks[1:3], original.blocks[1:3])
        self.assertEqual(output.blocks[0][12:], original.blocks[0][8:])
        self.assertEqual(output.footer, original.footer)
        payload = json.loads(output.shape(0)[3][0][1])
        self.assertEqual(payload["surfaceId"], identity)
        self.assertEqual(payload["material"], draft())

    def test_variant_identity_and_independent_copy(self):
        first = skin.Nif(fixture())
        identity = first.assign(0, draft())
        self.assertEqual(first.assign(0, draft()), identity)
        variant = skin.Nif(fixture())
        self.assertEqual(variant.assign(0, draft(), identity), identity)
        self.assertNotEqual(first.assign(0, draft(), independent=True), identity)

    def test_duplicate_metadata_is_rejected(self):
        nif = skin.Nif(fixture())
        nif.assign(0, draft())
        block = nif.blocks[0]
        nif.blocks[0] = block[:4] + skin.u32(2) + block[8:12] * 2 + block[12:]
        with self.assertRaises(ValueError):
            nif.shape(0)

    def test_truncated_or_other_version_is_rejected(self):
        data = fixture()
        for bad in (data[:-1], data.replace(skin.u32(100), skin.u32(130), 1)):
            with self.assertRaises(ValueError):
                skin.Nif(bad)

    def test_staging_never_overwrites_input_or_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "input.nif"
            destination = Path(directory) / "output.nif"
            source.write_bytes(fixture())
            with self.assertRaises(ValueError):
                skin.publish(source, 0, draft(), source)
            skin.publish(source, 0, draft(), destination)
            before = destination.read_bytes()
            with self.assertRaises(ValueError):
                skin.publish(source, 0, draft(), destination)
            self.assertEqual(destination.read_bytes(), before)
            self.assertEqual(source.read_bytes(), fixture())


if __name__ == "__main__":
    unittest.main()
