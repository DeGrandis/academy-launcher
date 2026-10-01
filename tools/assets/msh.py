"""Reads, writes and converts Clone Wars .msh meshes (Pandemic MSH2, as used by the Battlezone II/Zero engine).

A .msh is a tree of chunks: 4-byte tag, u32 size, body (no padding). Containers hold child chunks;
MATL, SKL2 and BLN2 start with a u32 count before their children.

  HEDR
    MSH2
      SINF (NAME FRAM BBOX)   CAMR   LGTP
      MATL count, MATD* (NAME, DATA: diffuse/specular/ambient rgba + power, ATRB, TX0D..TX3D texture names)
      MODL* (MTYP, MNDX, NAME, PRNT parent name, FLGS, TRAN scale3/quat4(xyzw)/translation3,
             GEOM (BBOX, SEGM* (MATI, POSL, NRML, UV0L, CLRL, NDXL, STRP, WGHT, SHDW), ENVL))
    SKL2  BLN2  ANM2 (CYCL KFR3)
  CL1L (end marker)

STRP holds triangle strips as u16 indices; the first two indices of every strip have bit 0x8000 set.

Usage:
  python tools/assets/msh.py tree   <file.msh>
  python tools/assets/msh.py check  <dir>                          (parse + rewrite every .msh, must be identical)
  python tools/assets/msh.py export <file.msh> <out.gltf> [--textures <dir with .xbt>]
  python tools/assets/msh.py import <template.msh> <in.gltf> <out.msh>

import keeps everything in the template and rebuilds the geometry of each model whose node name appears in the
glTF (triangulated primitives with POSITION, NORMAL, TEXCOORD_0; one segment per primitive). Nodes not in the
template are added as new static models. Bounding boxes are recomputed.
"""

import base64
import json
import math
import struct
import sys
from pathlib import Path

COUNTED = {b"MATL", b"SKL2", b"BLN2"}
CONTAINERS = {b"HEDR", b"MSH2", b"MODL", b"GEOM", b"SEGM", b"MATD", b"SINF", b"ANM2", b"CAMR", b"MATL", b"SKL2", b"BLN2"}


class Chunk:
    def __init__(self, tag, data=b"", children=None, count=None):
        self.tag = tag
        self.data = data
        self.children = children
        self.count = count

    def find(self, tag):
        return next((child for child in self.children or [] if child.tag == tag), None)

    def find_all(self, tag):
        return [child for child in self.children or [] if child.tag == tag]

    def serialize(self):
        if self.children is None:
            body = self.data
        else:
            body = (struct.pack("<I", self.count) if self.count is not None else b"") + b"".join(c.serialize() for c in self.children)
        return self.tag + struct.pack("<I", len(body)) + body


def parse_chunks(data, offset, end):
    chunks = []
    while offset + 8 <= end:
        tag = data[offset:offset + 4]
        size = struct.unpack_from("<I", data, offset + 4)[0]
        body_start = offset + 8
        body_end = body_start + size
        if body_end > end:
            raise ValueError(f"chunk {tag!r} at {offset} overruns its parent")
        if tag in CONTAINERS and _looks_like_container(data, body_start + (4 if tag in COUNTED else 0), body_end):
            count = struct.unpack_from("<I", data, body_start)[0] if tag in COUNTED else None
            children = parse_chunks(data, body_start + (4 if tag in COUNTED else 0), body_end)
            chunks.append(Chunk(tag, children=children, count=count))
        else:
            chunks.append(Chunk(tag, data[body_start:body_end]))
        offset = body_end
    if offset != end:
        chunks.append(Chunk(b"\0RAW", data[offset:end]))  # trailing bytes, kept for exact round trips
    return chunks


def _looks_like_container(data, start, end):
    if start == end:
        return True
    if start + 8 > end:
        return False
    tag = data[start:start + 4]
    size = struct.unpack_from("<I", data, start + 4)[0]
    return all(48 <= c < 91 or c == 95 for c in tag) and start + 8 + size <= end


class RawAware(Chunk):
    pass


def serialize_tree(chunks):
    out = bytearray()
    for chunk in chunks:
        out += chunk.data if chunk.tag == b"\0RAW" else chunk.serialize()
    return bytes(out)


def read_msh(path):
    data = Path(path).read_bytes()
    return parse_chunks(data, 0, len(data))


def cstring(data):
    return data.split(b"\0", 1)[0].decode("latin-1")


def name_chunk(tag, text):
    raw = text.encode("latin-1") + b"\0"
    raw += bytes((4 - len(raw) % 4) % 4)
    return Chunk(tag, raw)


# ---------------------------------------------------------------------------------------------- geometry helpers

def read_vectors(data, components):
    count = struct.unpack_from("<I", data, 0)[0]
    values = struct.unpack_from(f"<{count * components}f", data, 4)
    return [tuple(values[i * components:(i + 1) * components]) for i in range(count)]


def strips_to_triangles(data):
    count = struct.unpack_from("<I", data, 0)[0]
    indices = struct.unpack_from(f"<{count}H", data, 4)
    triangles = []
    strip = []
    for position, raw in enumerate(indices):
        starts = raw & 0x8000 and position + 1 < count and indices[position + 1] & 0x8000
        if starts and (not strip or len(strip) >= 2):
            strip = []
            parity = 0
        strip.append(raw & 0x7FFF)
        if len(strip) >= 3:
            a, b, c = strip[-3], strip[-2], strip[-1]
            if len(strip) % 2 == 0:
                a, b = b, a
            if a != b and b != c and a != c:
                triangles.append((a, b, c))
    return triangles


def triangles_to_strips(triangles):
    """Every triangle becomes its own 3-index strip (valid for the engine; not optimally short)."""
    indices = []
    for a, b, c in triangles:
        indices += [a | 0x8000, b | 0x8000, c]
    return struct.pack(f"<I{len(indices)}H", len(indices), *indices)


def triangles_to_polygons(triangles):
    body = bytearray(struct.pack("<I", len(triangles)))
    for a, b, c in triangles:
        body += struct.pack("<4H", 3, a, b, c)
    return bytes(body)


def bbox_chunk(points):
    if not points:
        points = [(0.0, 0.0, 0.0)]
    low = [min(p[i] for p in points) for i in range(3)]
    high = [max(p[i] for p in points) for i in range(3)]
    center = [(low[i] + high[i]) / 2 for i in range(3)]
    extent = [(high[i] - low[i]) / 2 for i in range(3)]
    radius = math.sqrt(sum(e * e for e in extent))
    return Chunk(b"BBOX", struct.pack("<4f3f3f f", 0, 0, 0, 1, *center, *extent, radius))


# ---------------------------------------------------------------------------------------------- glTF export

def export_gltf(msh_path, out_path, texture_dir=None):
    tree = read_msh(msh_path)
    hedr = tree[0]
    msh2 = hedr.find(b"MSH2")
    out_path = Path(out_path)
    buffer = bytearray()
    views, accessors, meshes, nodes, materials, images, textures = [], [], [], [], [], [], []
    texture_index = {}

    def add_view(blob, target=None):
        while len(buffer) % 4:
            buffer.append(0)
        view = {"buffer": 0, "byteOffset": len(buffer), "byteLength": len(blob)}
        if target:
            view["target"] = target
        buffer.extend(blob)
        views.append(view)
        return len(views) - 1

    def add_accessor(values, kind, components):
        flat = [v for value in values for v in value]
        view = add_view(struct.pack(f"<{len(flat)}f", *flat), 34962)
        accessor = {"bufferView": view, "componentType": 5126, "count": len(values), "type": kind}
        if kind == "VEC3":
            accessor["min"] = [min(v[i] for v in values) for i in range(3)]
            accessor["max"] = [max(v[i] for v in values) for i in range(3)]
        accessors.append(accessor)
        return len(accessors) - 1

    matl = msh2.find(b"MATL")
    for matd in matl.find_all(b"MATD") if matl else []:
        name = cstring(matd.find(b"NAME").data)
        diffuse = struct.unpack_from("<4f", matd.find(b"DATA").data, 0)
        material = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": list(diffuse), "metallicFactor": 0.0, "roughnessFactor": 1.0},
                    "extras": {"msh_data": matd.find(b"DATA").data.hex(), "msh_atrb": matd.find(b"ATRB").data.hex()}}
        tx0d = matd.find(b"TX0D")
        if tx0d is not None and cstring(tx0d.data):
            texture = cstring(tx0d.data)
            material["extras"]["texture"] = texture
            if texture_dir is not None:
                stem = Path(texture).stem.lower()
                if stem not in texture_index:
                    xbt = next((p for p in Path(texture_dir).glob("*.xbt") if p.stem.lower() == stem), None)
                    if xbt is not None:
                        sys.path.insert(0, str(Path(__file__).resolve().parent))
                        import xbt as xbt_module
                        image_path = out_path.parent / f"{stem}.png"
                        xbt_module.decode(xbt, image_path)
                        images.append({"uri": image_path.name})
                        textures.append({"source": len(images) - 1})
                        texture_index[stem] = len(textures) - 1
                if stem in texture_index:
                    material["pbrMetallicRoughness"]["baseColorTexture"] = {"index": texture_index[stem]}
        materials.append(material)

    models = msh2.find_all(b"MODL")
    node_of = {}
    for model in models:
        name = cstring(model.find(b"NAME").data)
        tran = struct.unpack("<10f", model.find(b"TRAN").data[:40])
        node = {"name": name, "scale": list(tran[0:3]), "rotation": list(tran[3:7]), "translation": list(tran[7:10]),
                "extras": {"msh_mtyp": struct.unpack("<I", model.find(b"MTYP").data)[0]}}
        geom = model.find(b"GEOM")
        primitives = []
        for segment in geom.find_all(b"SEGM") if geom else []:
            posl, strp = segment.find(b"POSL"), segment.find(b"STRP")
            if posl is None or strp is None:
                continue
            positions = read_vectors(posl.data, 3)
            triangles = strips_to_triangles(strp.data)
            if not positions or not triangles:
                continue
            attributes = {"POSITION": add_accessor(positions, "VEC3", 3)}
            nrml = segment.find(b"NRML")
            if nrml is not None:
                attributes["NORMAL"] = add_accessor(read_vectors(nrml.data, 3), "VEC3", 3)
            uv = segment.find(b"UV0L")
            if uv is not None:
                attributes["TEXCOORD_0"] = add_accessor([(u, 1.0 - v) for u, v in read_vectors(uv.data, 2)], "VEC2", 2)
            flat = [i for triangle in triangles for i in triangle]
            index_view = add_view(struct.pack(f"<{len(flat)}H", *flat), 34963)
            accessors.append({"bufferView": index_view, "componentType": 5123, "count": len(flat), "type": "SCALAR"})
            primitive = {"attributes": attributes, "indices": len(accessors) - 1}
            mati = segment.find(b"MATI")
            if mati is not None and struct.unpack("<I", mati.data)[0] < len(materials):
                primitive["material"] = struct.unpack("<I", mati.data)[0]
            primitives.append(primitive)
        if primitives:
            meshes.append({"name": name, "primitives": primitives})
            node["mesh"] = len(meshes) - 1
        node_of[name] = len(nodes)
        nodes.append(node)
    roots = []
    for index, model in enumerate(models):
        parent = model.find(b"PRNT")
        parent_name = cstring(parent.data) if parent is not None else ""
        if parent_name in node_of and node_of[parent_name] != index:
            nodes[node_of[parent_name]].setdefault("children", []).append(index)
        else:
            roots.append(index)

    gltf = {
        "asset": {"version": "2.0", "generator": "clone_wars_decompile tools/assets/msh.py"},
        "scene": 0, "scenes": [{"nodes": roots}], "nodes": nodes, "meshes": meshes, "materials": materials,
        "accessors": accessors, "bufferViews": views,
        "buffers": [{"byteLength": len(buffer), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
    }
    if images:
        gltf["images"] = images
        gltf["textures"] = textures
    out_path.write_text(json.dumps(gltf, indent=1))


# ---------------------------------------------------------------------------------------------- glTF import

def load_gltf(path):
    gltf = json.loads(Path(path).read_text())
    blobs = []
    for buffer in gltf["buffers"]:
        uri = buffer["uri"]
        if uri.startswith("data:"):
            blobs.append(base64.b64decode(uri.split(",", 1)[1]))
        else:
            blobs.append((Path(path).parent / uri).read_bytes())

    def accessor(index):
        acc = gltf["accessors"][index]
        view = gltf["bufferViews"][acc["bufferView"]]
        blob = blobs[view["buffer"]]
        components = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
        code = {5126: "f", 5123: "H", 5125: "I", 5121: "B"}[acc["componentType"]]
        size = struct.calcsize(code)
        stride = view.get("byteStride", size * components)
        start = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
        values = []
        for i in range(acc["count"]):
            values.append(struct.unpack_from(f"<{components}{code}", blob, start + i * stride))
        return values if components > 1 else [v[0] for v in values]
    return gltf, accessor


def build_segments(gltf, accessor, mesh_index):
    segments = []
    all_points = []
    for primitive in gltf["meshes"][mesh_index]["primitives"]:
        if primitive.get("mode", 4) != 4:
            raise ValueError("only triangle-list primitives can be imported")
        positions = accessor(primitive["attributes"]["POSITION"])
        if len(positions) > 0x7FFF:
            raise ValueError("a segment can have at most 32767 vertices; split the mesh")
        normals = accessor(primitive["attributes"]["NORMAL"]) if "NORMAL" in primitive["attributes"] else [(0.0, 1.0, 0.0)] * len(positions)
        uvs = accessor(primitive["attributes"]["TEXCOORD_0"]) if "TEXCOORD_0" in primitive["attributes"] else [(0.0, 0.0)] * len(positions)
        flat = accessor(primitive["indices"]) if "indices" in primitive else list(range(len(positions)))
        triangles = [tuple(flat[i:i + 3]) for i in range(0, len(flat) - 2, 3)]
        children = [
            Chunk(b"MATI", struct.pack("<I", primitive.get("material", 0))),
            Chunk(b"POSL", struct.pack(f"<I{len(positions) * 3}f", len(positions), *[c for p in positions for c in p])),
            Chunk(b"NRML", struct.pack(f"<I{len(normals) * 3}f", len(normals), *[c for n in normals for c in n])),
            Chunk(b"UV0L", struct.pack(f"<I{len(uvs) * 2}f", len(uvs), *[c for u, v in uvs for c in (u, 1.0 - v)])),
            Chunk(b"NDXL", triangles_to_polygons(triangles)),
            Chunk(b"STRP", triangles_to_strips(triangles)),
        ]
        segments.append(Chunk(b"SEGM", children=children))
        all_points += positions
    return segments, all_points


def same_geometry(model, gltf, accessor, mesh_index):
    geom = model.find(b"GEOM")
    segments = [seg for seg in (geom.find_all(b"SEGM") if geom else []) if seg.find(b"POSL") and seg.find(b"STRP")]
    primitives = gltf["meshes"][mesh_index]["primitives"]
    segments = [seg for seg in segments if read_vectors(seg.find(b"POSL").data, 3) and strips_to_triangles(seg.find(b"STRP").data)]
    if len(segments) != len(primitives):
        return False
    for segment, primitive in zip(segments, primitives):
        positions = accessor(primitive["attributes"]["POSITION"])
        original = read_vectors(segment.find(b"POSL").data, 3)
        if len(positions) != len(original) or any(abs(a - b) > 1e-5 for p, q in zip(positions, original) for a, b in zip(p, q)):
            return False
        flat = accessor(primitive["indices"])
        if [tuple(flat[i:i + 3]) for i in range(0, len(flat) - 2, 3)] != strips_to_triangles(segment.find(b"STRP").data):
            return False
        if "TEXCOORD_0" in primitive["attributes"] and segment.find(b"UV0L") is not None:
            uvs = accessor(primitive["attributes"]["TEXCOORD_0"])
            original_uvs = [(u, 1.0 - v) for u, v in read_vectors(segment.find(b"UV0L").data, 2)]
            if any(abs(a - b) > 1e-5 for p, q in zip(uvs, original_uvs) for a, b in zip(p, q)):
                return False
    return True


def import_gltf(template_path, gltf_path, out_path):
    tree = read_msh(template_path)
    msh2 = tree[0].find(b"MSH2")
    gltf, accessor = load_gltf(gltf_path)
    nodes_by_name = {node["name"]: (index, node) for index, node in enumerate(gltf["nodes"]) if "name" in node}
    parent_of = {}
    for index, node in enumerate(gltf["nodes"]):
        for child in node.get("children", []):
            parent_of[child] = node.get("name", "")
    existing = {cstring(model.find(b"NAME").data): model for model in msh2.find_all(b"MODL")}
    replaced = added = 0
    for name, (index, node) in nodes_by_name.items():
        model = existing.get(name)
        if model is None:
            if "mesh" not in node:
                continue
            model = Chunk(b"MODL", children=[
                Chunk(b"MTYP", struct.pack("<I", 0)),
                Chunk(b"MNDX", struct.pack("<I", len(msh2.find_all(b"MODL")) + 1)),
                name_chunk(b"NAME", name),
            ])
            if parent_of.get(index):
                model.children.append(name_chunk(b"PRNT", parent_of[index]))
            model.children.append(Chunk(b"FLGS", struct.pack("<I", 0)))
            model.children.append(Chunk(b"TRAN", b""))
            insert_at = max(i for i, c in enumerate(msh2.children) if c.tag == b"MODL") + 1
            msh2.children.insert(insert_at, model)
            added += 1
        scale = node.get("scale", [1, 1, 1])
        rotation = node.get("rotation", [0, 0, 0, 1])
        translation = node.get("translation", [0, 0, 0])
        tran = model.find(b"TRAN")
        new_tran = struct.pack("<10f", *scale, *rotation, *translation)
        if len(tran.data) < 40 or any(abs(a - b) > 1e-6 for a, b in zip(struct.unpack("<10f", tran.data[:40]), struct.unpack("<10f", new_tran))):
            tran.data = new_tran + tran.data[40:]
        if "mesh" not in node:
            continue
        if model in existing.values() and same_geometry(model, gltf, accessor, node["mesh"]):
            continue  # unchanged: keep the original segments (strips, shadow volumes, extra UV sets)
        segments, points = build_segments(gltf, accessor, node["mesh"])
        geom = model.find(b"GEOM")
        if geom is None:
            geom = Chunk(b"GEOM", children=[])
            model.children.append(geom)
        kept = [c for c in geom.children if c.tag not in (b"BBOX", b"SEGM")]
        geom.children = [bbox_chunk(points)] + segments + kept
        model.find(b"MTYP").data = struct.pack("<I", 0) if model.find(b"MTYP").data == struct.pack("<I", 1) else model.find(b"MTYP").data
        replaced += 1
    Path(out_path).write_bytes(serialize_tree(tree))
    print(f"wrote {out_path}: {replaced} models rebuilt ({added} new)")


# ---------------------------------------------------------------------------------------------- CLI

def print_tree(chunks, depth=0):
    for chunk in chunks:
        if chunk.tag == b"\0RAW":
            print("  " * depth + f"(raw {len(chunk.data)} bytes)")
            continue
        info = ""
        if chunk.tag in (b"NAME", b"PRNT", b"TX0D", b"TX1D", b"TX2D", b"TX3D"):
            info = cstring(chunk.data)
        elif chunk.tag in (b"POSL", b"NRML", b"UV0L", b"STRP", b"NDXL", b"WGHT", b"CLRL") and len(chunk.data) >= 4:
            info = f"count {struct.unpack_from('<I', chunk.data, 0)[0]}"
        elif chunk.tag in (b"MTYP", b"MNDX", b"MATI", b"FLGS") and len(chunk.data) == 4:
            info = str(struct.unpack("<I", chunk.data)[0])
        print("  " * depth + f"{chunk.tag.decode('latin-1')} {len(chunk.data) if chunk.children is None else ''} {info}".rstrip())
        if chunk.children is not None:
            print_tree(chunk.children, depth + 1)


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    command = argv[1]
    if command == "tree":
        print_tree(read_msh(argv[2]))
    elif command == "check":
        files = sorted(Path(argv[2]).glob("*.msh"))
        failures = 0
        for path in files:
            data = path.read_bytes()
            try:
                if serialize_tree(parse_chunks(data, 0, len(data))) != data:
                    failures += 1
                    print(f"{path.name}: rewrite differs")
            except Exception as error:
                failures += 1
                print(f"{path.name}: {error}")
        print(f"{len(files) - failures}/{len(files)} round-trip identically")
    elif command == "export":
        textures = argv[argv.index("--textures") + 1] if "--textures" in argv else None
        export_gltf(argv[2], argv[3], textures)
    elif command == "import":
        import_gltf(argv[2], argv[3], argv[4])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
