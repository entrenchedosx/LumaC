#!/usr/bin/env python3
"""Generate deterministic glTF fixtures for Phase 14 asset tests.

Everything here is authored from scratch (no third-party geometry or
images), so all outputs are freely committable with no license
encumbrance. Run from the repository root:

    py -3 assets/generate_fixtures.py

Outputs (all deterministic: fixed key order, no timestamps):
  assets/BoxTextured.glb                 example asset (textured box)
  assets/tests/fixtures/fixture.glb      sharing/hierarchy fixture
  assets/tests/fixtures/fixture.gltf/.bin/.png   external-ref fixture
  assets/tests/fixtures/malformed/*      negative-test inputs

Conventions baked in: Y-up right-handed, column-major matrices, CCW
front faces, top-left UVs (matching Luma throughout, so the importer
converts nothing).
"""

import json
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIX_DIR = os.path.join(ROOT, "assets", "tests", "fixtures")
MAL_DIR = os.path.join(FIX_DIR, "malformed")


def write_png_rgba(path, width, height, px):
    """Minimal RGBA PNG writer (stdlib only). px = bytes row-major."""

    def chunk(ctype, data):
        c = ctype + data
        return struct.pack(">I", len(data)) + c + struct.pack(
            ">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = b"".join(
        b"\x00" + px[y * width * 4:(y + 1) * width * 4]
        for y in range(height))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6,
                                      0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) +
           chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def write_glb(path, json_obj, bin_bytes):
    """Write a .glb: JSON chunk (0x20-padded) + BIN chunk (0x00-padded)."""
    j = json.dumps(json_obj, separators=(",", ":"),
                   sort_keys=True).encode("utf-8")
    j += b" " * (-len(j) % 4)
    b = bytes(bin_bytes) + b"\x00" * (-len(bin_bytes) % 4)
    total = 12 + 8 + len(j) + 8 + len(b)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(j), 0x4E4F534A))
        f.write(j)
        f.write(struct.pack("<II", len(b), 0x004E4942))
        f.write(b)


def checker(w, h, c0, c1, cell=8):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            c = c1 if ((x // cell) + (y // cell)) % 2 else c0
            px += bytes(c)
    return bytes(px)


# ------------------------------------------------------------------
# assets/BoxTextured.glb — example asset: indexed box, normals + UVs,
# embedded checker texture, one translated node.
# ------------------------------------------------------------------

def build_box():
    s = 0.5
    # Corner orders copied from the proven renderer primitive
    # (renderer/src/mesh_primitives.c): faces +X -X +Y -Y +Z -Z, each
    # CCW-outward for BACK/CCW raster state. UVs run (0,0),(1,0),
    # (1,1),(0,1) down each face's vertex order.
    faces = [
        ((1, 0, 0), [(s, -s, s), (s, -s, -s), (s, s, -s), (s, s, s)]),
        ((-1, 0, 0), [(-s, -s, -s), (-s, -s, s), (-s, s, s), (-s, s, -s)]),
        ((0, 1, 0), [(-s, s, s), (s, s, s), (s, s, -s), (-s, s, -s)]),
        ((0, -1, 0), [(-s, -s, -s), (s, -s, -s), (s, -s, s), (-s, -s, s)]),
        ((0, 0, 1), [(-s, -s, s), (s, -s, s), (s, s, s), (-s, s, s)]),
        ((0, 0, -1), [(s, -s, -s), (-s, -s, -s), (-s, s, -s), (s, s, -s)]),
    ]
    positions, normals, uvs, indices = [], [], [], []
    for n, corners in faces:
        base = len(positions) // 3
        for i, p in enumerate(corners):
            positions += list(p)
            normals += list(n)
            uvs += [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)][i]
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return positions, normals, uvs, indices


def make_box_glb():
    positions, normals, uvs, indices = build_box()
    pos_b = struct.pack("<%df" % len(positions), *positions)
    nrm_b = struct.pack("<%df" % len(normals), *normals)
    uv_b = struct.pack("<%df" % len(uvs), *uvs)
    idx_b = struct.pack("<%dH" % len(indices), *indices)
    png = checker_png_bytes(64, 64, (235, 30, 90, 255), (235, 235, 235,
                                                          255), 8)
    blob = pos_b + nrm_b + uv_b + idx_b + png
    o_pos, o_nrm = 0, len(pos_b)
    o_uv, o_idx = o_nrm + len(nrm_b), o_nrm + len(nrm_b) + len(uv_b)
    o_png = o_idx + len(idx_b)
    gltf = {
        "asset": {"version": "2.0", "generator": "luma-generate-fixtures"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "translation": [0.0, 0.0, 0.0]}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
            "indices": 3, "material": 0}]}],
        "materials": [{
            "name": "CheckerRed",
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "baseColorTexture": {"index": 0},
                "metallicFactor": 0.0, "roughnessFactor": 0.9},
            "doubleSided": False}],
        "textures": [{"source": 0, "sampler": 0}],
        "images": [{"bufferView": 4, "mimeType": "image/png"}],
        "samplers": [{"magFilter": 9729, "minFilter": 9986,
                      "wrapS": 10497, "wrapT": 10497}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 24,
             "type": "VEC3"},
            {"bufferView": 1, "componentType": 5126, "count": 24,
             "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 24,
             "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 36,
             "type": "SCALAR"}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": o_pos, "byteLength": len(pos_b)},
            {"buffer": 0, "byteOffset": o_nrm, "byteLength": len(nrm_b)},
            {"buffer": 0, "byteOffset": o_uv, "byteLength": len(uv_b)},
            {"buffer": 0, "byteOffset": o_idx, "byteLength": len(idx_b)},
            {"buffer": 0, "byteOffset": o_png, "byteLength": len(png)}],
        "buffers": [{"byteLength": len(blob)}],
    }
    return gltf, blob


def checker_png_bytes(w, h, c0, c1, cell):
    import io

    def chunk(ctype, data):
        c = ctype + data
        return struct.pack(">I", len(data)) + c + struct.pack(
            ">I", zlib.crc32(c) & 0xFFFFFFFF)

    raw = b"".join(
        b"\x00" + b"".join(
            bytes(c1 if ((x // cell) + (y // cell)) % 2 else c0)
            for x in range(w))
        for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0,
                                       0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# ------------------------------------------------------------------
# Fixture: shared meshes, hierarchy, generated normals/tangents,
# interleaved stride, matrix node, dual-role texture.
# ------------------------------------------------------------------

def make_fixture():
    # Quad mesh 0 (own bufferViews): prim0 textured, prim1 flat.
    # positions/normals/uvs tightly packed; indices u16.
    q_pos = [-1.0, -1.0, 0.0, 1.0, -1.0, 0.0, 1.0, 1.0, 0.0,
             -1.0, 1.0, 0.0]
    q_nrm = [0.0, 0.0, 1.0] * 4
    q_uv = [0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0]
    q_idx = [0, 1, 2, 0, 2, 3]
    # Triangle mesh 1: POSITION only (normal/tangent generation).
    t_pos = [-0.5, -0.5, 0.5, 0.5, -0.5, 0.5, 0.0, 0.5, 0.5]
    # Interleaved mesh 2: POSITION+NORMAL stride 24 in ONE view
    # (accessor byteOffsets 0 and 12), indices u8.
    m2 = []
    for p, n in zip([q_pos[i:i + 3] for i in range(0, 12, 3)],
                    [q_nrm[i:i + 3] for i in range(0, 12, 3)]):
        m2 += p + n
    m2_idx = [0, 1, 2, 0, 2, 3]
    png = checker_png_bytes(4, 4, (200, 40, 40, 255), (40, 200, 40, 255),
                            2)

    blob = bytearray()
    blob += struct.pack("<%df" % len(q_pos), *q_pos)
    o_qpos = 0
    blob += struct.pack("<%df" % len(q_nrm), *q_nrm)
    o_qnrm = 12 * 4
    blob += struct.pack("<%df" % len(q_uv), *q_uv)
    o_quv = o_qnrm + 12 * 4
    blob += struct.pack("<%dH" % len(q_idx), *q_idx)
    o_qidx = o_quv + 8 * 4
    blob += struct.pack("<%df" % len(t_pos), *t_pos)
    o_tpos = o_qidx + 6 * 2
    blob += struct.pack("<%df" % len(m2), *m2)
    o_m2 = o_tpos + 9 * 4
    blob += bytes(bytearray(m2_idx))
    o_m2idx = o_m2 + 24 * 4
    blob += png
    o_png = o_m2idx + 6
    blob = bytes(blob)

    def acc(view, ctype, count, typ, offset=0):
        a = {"bufferView": view, "componentType": ctype,
             "count": count, "type": typ}
        if offset:
            a["byteOffset"] = offset
        return a

    gltf = {
        "asset": {"version": "2.0", "generator": "luma-generate-fixtures"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1, 3, 4]}],
        "nodes": [
            {"name": "QuadA", "mesh": 0, "translation": [-1.5, 0.0, 0.0]},
            {"name": "QuadB", "mesh": 0, "rotation": [0.0, 0.0, 0.0, 1.0],
             "children": [2]},
            {"name": "TriC", "mesh": 1, "scale": [2.0, 2.0, 2.0]},
            {"name": "InterleavedD", "mesh": 2},
            {"name": "MatrixE", "mesh": 1,
             "matrix": [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 1.0,
                        0.0, 0.0, 1.5, 0.0, 0.0, 1.0]},
        ],
        "meshes": [
            {"primitives": [
                {"attributes": {"POSITION": 0, "NORMAL": 1,
                                "TEXCOORD_0": 2},
                 "indices": 3, "material": 0, "mode": 4},
                {"attributes": {"POSITION": 0, "NORMAL": 1,
                                "TEXCOORD_0": 2},
                 "indices": 3, "material": 1, "mode": 4}]},
            {"primitives": [
                {"attributes": {"POSITION": 4}, "indices": 5,
                 "material": 1, "mode": 4}]},
            {"primitives": [
                {"attributes": {"POSITION": 6, "NORMAL": 7},
                 "indices": 8, "material": 1, "mode": 4}]},
        ],
        "materials": [
            {"name": "TexGreen",
             "pbrMetallicRoughness": {
                 "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                 "baseColorTexture": {"index": 0},
                 "metallicFactor": 0.2, "roughnessFactor": 0.8},
             "normalTexture": {"index": 1, "scale": 0.75},
             "emissiveFactor": [0.0, 0.0, 0.0],
             "alphaMode": "OPAQUE", "doubleSided": False},
            {"name": "FlatBlue",
             "pbrMetallicRoughness": {
                 "baseColorFactor": [0.25, 0.45, 0.9, 1.0],
                 "metallicFactor": 0.0, "roughnessFactor": 0.5},
             "alphaMode": "OPAQUE", "doubleSided": False},
        ],
        "textures": [
            {"source": 0, "sampler": 0},
            {"source": 0, "sampler": 1},
        ],
        "images": [{"bufferView": 8, "mimeType": "image/png"}],
        "samplers": [
            {"magFilter": 9729, "minFilter": 9986, "wrapS": 33648,
             "wrapT": 33648},
            {"magFilter": 9728, "minFilter": 9728, "wrapS": 33071,
             "wrapT": 33071},
        ],
        "accessors": [
            acc(0, 5126, 4, "VEC3"), acc(1, 5126, 4, "VEC3"),
            acc(2, 5126, 4, "VEC2"), acc(3, 5123, 6, "SCALAR"),
            acc(4, 5126, 3, "VEC3"), acc(5, 5123, 3, "SCALAR"),
            acc(6, 5126, 4, "VEC3"), acc(6, 5126, 4, "VEC3", 12),
            acc(7, 5121, 6, "SCALAR")],
        "bufferViews": [
            {"buffer": 0, "byteOffset": o_qpos, "byteLength": 48},
            {"buffer": 0, "byteOffset": o_qnrm, "byteLength": 48},
            {"buffer": 0, "byteOffset": o_quv, "byteLength": 32},
            {"buffer": 0, "byteOffset": o_qidx, "byteLength": 12},
            {"buffer": 0, "byteOffset": o_tpos, "byteLength": 36},
            {"buffer": 0, "byteOffset": o_qidx, "byteLength": 12},
            {"buffer": 0, "byteOffset": o_m2, "byteLength": 96,
             "byteStride": 24},
            {"buffer": 0, "byteOffset": o_m2idx, "byteLength": 6},
            {"buffer": 0, "byteOffset": o_png, "byteLength": len(png)}],
        "buffers": [{"byteLength": len(blob)}],
    }
    return gltf, blob


def write_gltf_external(path, json_obj, bin_bytes):
    """Write a .gltf + sibling .bin/.png trio (external references)."""
    import copy
    doc = copy.deepcopy(json_obj)
    base = os.path.splitext(os.path.basename(path))[0]
    bin_name = base + ".bin"
    png_name = base + ".png"
    doc["buffers"] = [{"byteLength": len(bin_bytes), "uri": bin_name}]
    # The single embedded image becomes a sibling-file reference.
    for img in doc.get("images", []):
        if "bufferView" in img:
            del img["bufferView"]
            img["uri"] = png_name
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, separators=(",", ":"), sort_keys=True)
        f.write("\n")
    with open(os.path.join(os.path.dirname(path), bin_name), "wb") as f:
        f.write(bin_bytes)


def make_malformed(base_gltf, base_blob):
    """(name, kind, payload) cases; kind is 'glb', 'gltf', or 'raw'.

    Expected importer results:
      invalid_json.gltf  -> LA_ERROR_IMPORT (bad JSON)
      truncated.glb       -> LA_ERROR_IMPORT (cut container)
      points_mode.glb     -> LA_ERROR_UNSUPPORTED (POINTS prim)
      no_position.glb     -> LA_ERROR_IMPORT (missing POSITION)
      index_oob.glb       -> LA_ERROR_IMPORT (index >= vertex count)
      accessor_oob.glb    -> LA_ERROR_IMPORT (accessor escapes buffer)
      missing_buffer.gltf -> LA_ERROR_NOT_FOUND (absent .bin)
      missing_image.gltf  -> LA_ERROR_NOT_FOUND (absent .png)
      corrupt_image.glb   -> LA_ERROR_IMPORT (undecodable bytes)
    """
    import copy
    import io
    cases = []

    cases.append(("invalid_json.gltf", "raw", b"{ not valid json !!!"))
    cases.append(("truncated.glb", "raw", bytes(base_blob[:20])))

    def glb_of(doc, blob):
        out = io.BytesIO()
        j = json.dumps(doc, separators=(",", ":"),
                       sort_keys=True).encode("utf-8")
        j += b" " * (-len(j) % 4)
        b = bytes(blob) + b"\x00" * (-len(blob) % 4)
        out.write(struct.pack("<III", 0x46546C67, 2,
                              12 + 8 + len(j) + 8 + len(b)))
        out.write(struct.pack("<II", len(j), 0x4E4F534A))
        out.write(j)
        out.write(struct.pack("<II", len(b), 0x004E4942))
        out.write(b)
        return out.getvalue()

    d = copy.deepcopy(base_gltf)
    d["meshes"][0]["primitives"][0]["mode"] = 0  # POINTS
    cases.append(("points_mode.glb", "glb", glb_of(d, base_blob)))

    d = copy.deepcopy(base_gltf)
    # Break the FIRST primitive: headless negative tests use a
    # placeholder renderer, so the failure must precede any upload.
    del d["meshes"][0]["primitives"][0]["attributes"]["POSITION"]
    cases.append(("no_position.glb", "glb", glb_of(d, base_blob)))

    d = copy.deepcopy(base_gltf)
    blob = bytearray(base_blob)
    # First u16 index of view 3 lives at its byteOffset; poison it.
    idx_off = d["bufferViews"][3]["byteOffset"]
    blob[idx_off] = 99
    blob[idx_off + 1] = 0
    cases.append(("index_oob.glb", "glb", glb_of(d, bytes(blob))))

    d = copy.deepcopy(base_gltf)
    d["accessors"][0]["count"] = 1000000  # escapes its buffer
    cases.append(("accessor_oob.glb", "glb", glb_of(d, base_blob)))

    d = copy.deepcopy(base_gltf)
    d["buffers"] = [{"byteLength": len(base_blob), "uri": "nope.bin"}]
    cases.append(("missing_buffer.gltf", "gltf", d))

    d = copy.deepcopy(base_gltf)
    d["buffers"] = [{"byteLength": len(base_blob), "uri": "ok.bin"}]
    d["images"] = [{"uri": "nope.png"}]
    cases.append(("missing_image.gltf", "gltf", (d, "ok.bin")))

    d = copy.deepcopy(base_gltf)
    blob = bytearray(base_blob) + b"definitely-not-an-image!!"
    junk_view = {"buffer": 0, "byteOffset": len(base_blob), "byteLength": 25}
    d["bufferViews"].append(junk_view)
    d["images"] = [{"bufferView": len(d["bufferViews"]) - 1,
                    "mimeType": "image/png"}]
    d["buffers"] = [{"byteLength": len(blob)}]
    cases.append(("corrupt_image.glb", "glb", glb_of(d, bytes(blob))))

    return cases


def main():
    import copy
    os.makedirs(os.path.join(ROOT, "assets"), exist_ok=True)
    os.makedirs(FIX_DIR, exist_ok=True)
    os.makedirs(MAL_DIR, exist_ok=True)

    gltf, blob = make_box_glb()
    write_glb(os.path.join(ROOT, "assets", "BoxTextured.glb"), gltf, blob)
    print("wrote assets/BoxTextured.glb", len(blob), "bin bytes")

    gltf, blob = make_fixture()
    write_glb(os.path.join(FIX_DIR, "fixture.glb"), gltf, blob)
    print("wrote fixture.glb", len(blob), "bin bytes")

    # External-reference trio: same scene, .bin + .png siblings.
    write_gltf_external(os.path.join(FIX_DIR, "fixture.gltf"), gltf, blob)
    png_off = gltf["bufferViews"][8]["byteOffset"]
    png_len = gltf["bufferViews"][8]["byteLength"]
    with open(os.path.join(FIX_DIR, "fixture.bin"), "wb") as f:
        f.write(blob[:png_off])
    with open(os.path.join(FIX_DIR, "fixture.png"), "wb") as f:
        f.write(blob[png_off:png_off + png_len])
    print("wrote fixture.gltf/.bin/.png")

    # Negative inputs (expected results documented in make_malformed).
    for name, kind, payload in make_malformed(gltf, blob):
        path = os.path.join(MAL_DIR, name)
        if kind == "raw":
            with open(path, "wb") as f:
                f.write(payload)
        elif kind == "glb":
            with open(path, "wb") as f:
                f.write(payload)
        elif kind == "gltf":
            doc, bin_name = payload if isinstance(payload, tuple) \
                else (payload, None)
            if bin_name is not None:
                doc = copy.deepcopy(doc)
                with open(os.path.join(MAL_DIR, bin_name), "wb") as f:
                    f.write(blob)
            with open(path, "w", encoding="utf-8") as f:
                json.dump(doc, f, separators=(",", ":"),
                          sort_keys=True)
                f.write("\n")
        print("wrote malformed/" + name)


if __name__ == "__main__":
    main()
