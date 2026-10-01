"""Renders a glTF from tools/assets/msh.py to a PNG (textured, flat-shaded, z-buffered) for quick visual checks.

Usage: python tools/assets/render_gltf.py <in.gltf> <out.png> [yaw-degrees] [pitch-degrees]
"""

import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from msh import load_gltf  # noqa: E402


def quaternion_matrix(x, y, z, w):
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])


def node_matrix(node):
    matrix = np.eye(4)
    matrix[:3, :3] = quaternion_matrix(*node.get("rotation", [0, 0, 0, 1])) * np.array(node.get("scale", [1, 1, 1]))
    matrix[:3, 3] = node.get("translation", [0, 0, 0])
    return matrix


def main(argv):
    gltf, accessor = load_gltf(argv[1])
    yaw = math.radians(float(argv[3]) if len(argv) > 3 else 35)
    pitch = math.radians(float(argv[4]) if len(argv) > 4 else 25)
    base = Path(argv[1]).parent
    images = [np.asarray(Image.open(base / image["uri"]).convert("RGB")) for image in gltf.get("images", [])]
    triangles = []

    def visit(index, parent):
        node = gltf["nodes"][index]
        world = parent @ node_matrix(node)
        if "mesh" in node:
            for primitive in gltf["meshes"][node["mesh"]]["primitives"]:
                positions = np.array(accessor(primitive["attributes"]["POSITION"]), float)
                positions = (world @ np.c_[positions, np.ones(len(positions))].T).T[:, :3]
                uvs = np.array(accessor(primitive["attributes"]["TEXCOORD_0"])) if "TEXCOORD_0" in primitive["attributes"] else None
                texture = None
                material = gltf["materials"][primitive["material"]] if "material" in primitive else {}
                info = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
                if info is not None:
                    texture = images[gltf["textures"][info["index"]]["source"]]
                flat = accessor(primitive["indices"])
                for i in range(0, len(flat) - 2, 3):
                    tri = flat[i:i + 3]
                    triangles.append((positions[tri], uvs[tri] if uvs is not None else None, texture))
        for child in node.get("children", []):
            visit(child, world)

    for root in gltf["scenes"][0]["nodes"]:
        visit(root, np.eye(4))
    rotation = np.array([[math.cos(yaw), 0, math.sin(yaw)], [0, 1, 0], [-math.sin(yaw), 0, math.cos(yaw)]])
    rotation = np.array([[1, 0, 0], [0, math.cos(pitch), -math.sin(pitch)], [0, math.sin(pitch), math.cos(pitch)]]) @ rotation
    points = np.concatenate([t[0] for t in triangles]) @ rotation.T
    low, high = points.min(axis=0), points.max(axis=0)
    size = 512
    scale = 0.9 * size / max(high[0] - low[0], high[1] - low[1])
    color = np.full((size, size, 3), 40, np.uint8)
    depth = np.full((size, size), np.inf)
    light = np.array([0.4, 0.8, -0.45]) / np.linalg.norm([0.4, 0.8, -0.45])
    for positions, uvs, texture in triangles:
        view = positions @ rotation.T
        screen = np.c_[(view[:, 0] - (low[0] + high[0]) / 2) * scale + size / 2, size / 2 - (view[:, 1] - (low[1] + high[1]) / 2) * scale]
        normal = np.cross(view[1] - view[0], view[2] - view[0])
        if np.linalg.norm(normal) == 0:
            continue
        shade = 0.35 + 0.65 * abs(np.dot(normal / np.linalg.norm(normal), light))
        x0, y0 = np.floor(screen.min(axis=0)).astype(int).clip(0, size - 1)
        x1, y1 = np.ceil(screen.max(axis=0)).astype(int).clip(0, size - 1)
        if x1 < x0 or y1 < y0:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        (ax, ay), (bx, by), (cx, cy) = screen
        area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay)
        if abs(area) < 1e-9:
            continue
        w0 = ((bx - xs) * (cy - ys) - (cx - xs) * (by - ys)) / area
        w1 = ((cx - xs) * (ay - ys) - (ax - xs) * (cy - ys)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        z = w0 * view[0, 2] + w1 * view[1, 2] + w2 * view[2, 2]
        region = depth[y0:y1 + 1, x0:x1 + 1]
        visible = inside & (z < region)
        region[visible] = z[visible]
        if texture is not None and uvs is not None:
            u = (w0 * uvs[0, 0] + w1 * uvs[1, 0] + w2 * uvs[2, 0]) % 1.0
            v = (w0 * uvs[0, 1] + w1 * uvs[1, 1] + w2 * uvs[2, 1]) % 1.0
            texels = texture[(v * (texture.shape[0] - 1)).astype(int), (u * (texture.shape[1] - 1)).astype(int)]
        else:
            texels = np.full(xs.shape + (3,), 200)
        target = color[y0:y1 + 1, x0:x1 + 1]
        target[visible] = (texels[visible] * shade).astype(np.uint8)
    Image.fromarray(color).save(argv[2])


if __name__ == "__main__":
    main(sys.argv)
