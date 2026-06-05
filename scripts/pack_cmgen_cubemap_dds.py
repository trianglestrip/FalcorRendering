#!/usr/bin/env python3
"""Pack cmgen per-face DDS outputs into a single cubemap DDS (D3D10).

cmgen --format=dds deploy writes individual face/mip DDS files. Falcor's
ImageIO::loadTextureFromDDS expects one cubemap DDS with all faces and mips.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

DDS_MAGIC = 0x20534444
DDS_HEADER_SIZE = 124
DDS_DXT10_SIZE = 20
DDS_DXT10_OFFSET = 4 + DDS_HEADER_SIZE

# DDS_HEADER flags / caps
DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PITCH = 0x8
DDSD_PIXELFORMAT = 0x1000
DDSD_MIPMAPCOUNT = 0x20000
DDSD_LINEARSIZE = 0x80000

DDSCAPS_TEXTURE = 0x1000
DDSCAPS_COMPLEX = 0x8
DDSCAPS_MIPMAP = 0x400000

DDSCAPS2_CUBEMAP = 0x200
DDSCAPS2_CUBEMAP_ALLFACES = 0xFC00

DDS_RESOURCE_MISC_TEXTURECUBE = 0x4

FACE_ORDER = ("px", "nx", "py", "ny", "pz", "nz")


def read_dds_payload(path: Path) -> tuple[bytes, int, int, int]:
    data = path.read_bytes()
    if len(data) < DDS_DXT10_OFFSET + DDS_DXT10_SIZE:
        raise ValueError(f"{path}: DDS file too small")
    magic, = struct.unpack_from("<I", data, 0)
    if magic != DDS_MAGIC:
        raise ValueError(f"{path}: invalid DDS magic")

    height, = struct.unpack_from("<I", data, 12)
    width, = struct.unpack_from("<I", data, 16)
    fourcc = data[84:88]
    if fourcc != b"DX10":
        raise ValueError(f"{path}: expected DX10 DDS (cmgen --format=dds)")

    dxgi_format, = struct.unpack_from("<I", data, DDS_DXT10_OFFSET)
    resource_dim, = struct.unpack_from("<I", data, DDS_DXT10_OFFSET + 4)
    if resource_dim != 3:
        raise ValueError(f"{path}: expected 2D DDS")

    header_size = DDS_DXT10_OFFSET + DDS_DXT10_SIZE
    payload = data[header_size:]
    return payload, width, height, dxgi_format


def mip_payload_size(width: int, height: int, dxgi_format: int) -> int:
    # cmgen DDS_LINEAR uses RGBA16F for environment maps.
    if dxgi_format == 10:  # DXGI_FORMAT_R16G16B16A16_FLOAT
        return width * height * 8
    raise ValueError(f"Unsupported DXGI format {dxgi_format}")


def build_cubemap_dds(
    face_mip_files: list[list[Path]],
    output_path: Path,
) -> None:
    if len(face_mip_files) != 6:
        raise ValueError("Cubemap requires 6 faces")

    mip_count = len(face_mip_files[0])
    if any(len(faces) != mip_count for faces in face_mip_files):
        raise ValueError("All faces must have the same mip count")

    payloads: list[bytes] = []
    base_w = base_h = dxgi_format = 0
    for face_idx, face_paths in enumerate(face_mip_files):
        for mip_idx, path in enumerate(face_paths):
            payload, width, height, fmt = read_dds_payload(path)
            if face_idx == 0 and mip_idx == 0:
                base_w, base_h, dxgi_format = width, height, fmt
            expected = mip_payload_size(width, height, fmt)
            if len(payload) != expected:
                raise ValueError(
                    f"{path}: payload size {len(payload)} != expected {expected} ({width}x{height})"
                )
            payloads.append(payload)

    # nvtt/D3D layout: for each face, all mips.
    body = b"".join(payloads)

    flags = (
        DDSD_CAPS
        | DDSD_HEIGHT
        | DDSD_WIDTH
        | DDSD_PIXELFORMAT
        | DDSD_MIPMAPCOUNT
        | DDSD_LINEARSIZE
    )
    caps1 = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX
    if mip_count > 1:
        caps1 |= DDSCAPS_MIPMAP
    caps2 = DDSCAPS2_CUBEMAP | DDSCAPS2_CUBEMAP_ALLFACES

    header = bytearray(DDS_DXT10_OFFSET + DDS_DXT10_SIZE)
    struct.pack_into("<I", header, 0, DDS_MAGIC)
    struct.pack_into("<I", header, 4, DDS_HEADER_SIZE)
    struct.pack_into("<I", header, 8, flags)
    struct.pack_into("<I", header, 12, base_h)
    struct.pack_into("<I", header, 16, base_w)
    struct.pack_into("<I", header, 20, len(body))  # pitch/linear size (unused for DX10)
    struct.pack_into("<I", header, 24, 1)  # depth
    struct.pack_into("<I", header, 28, mip_count)
    # pixel format
    struct.pack_into("<I", header, 76, 32)  # DDS_PIXELFORMAT size
    struct.pack_into("<I", header, 80, DDSD_PIXELFORMAT)
    header[84:88] = b"DX10"
    struct.pack_into("<I", header, 108, caps1)
    struct.pack_into("<I", header, 112, caps2)

    struct.pack_into("<I", header, DDS_DXT10_OFFSET, dxgi_format)
    struct.pack_into("<I", header, DDS_DXT10_OFFSET + 4, 3)  # TEXTURE2D
    struct.pack_into("<I", header, DDS_DXT10_OFFSET + 8, DDS_RESOURCE_MISC_TEXTURECUBE)
    struct.pack_into("<I", header, DDS_DXT10_OFFSET + 12, 6)  # arraySize (faces)
    struct.pack_into("<I", header, DDS_DXT10_OFFSET + 16, 0)  # miscFlags2

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(header) + body)
    print(f"Wrote {output_path} ({base_w}x{base_h}, {mip_count} mips, 6 faces)")


def collect_ibl_faces(src_dir: Path, mip_levels: int) -> list[list[Path]]:
    faces: list[list[Path]] = []
    for face in FACE_ORDER:
        mips = [src_dir / f"m{level}_{face}.dds" for level in range(mip_levels)]
        missing = [str(p) for p in mips if not p.exists()]
        if missing:
            raise FileNotFoundError(f"Missing IBL DDS files: {missing}")
        faces.append(mips)
    return faces


def collect_skybox_faces(src_dir: Path) -> list[list[Path]]:
    faces: list[list[Path]] = []
    for face in FACE_ORDER:
        path = src_dir / f"{face}.dds"
        if not path.exists():
            raise FileNotFoundError(f"Missing skybox face: {path}")
        faces.append([path])
    return faces


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", type=Path, required=True, help="cmgen face output directory")
    parser.add_argument("--ibl-out", type=Path, required=True, help="packed specular/IBL cubemap DDS")
    parser.add_argument("--skybox-out", type=Path, required=True, help="packed skybox cubemap DDS")
    parser.add_argument("--mip-levels", type=int, default=5, help="number of prefiltered mips")
    args = parser.parse_args()

    src_dir = args.src
    if not src_dir.is_dir():
        print(f"Source directory not found: {src_dir}", file=sys.stderr)
        return 1

    ibl_faces = collect_ibl_faces(src_dir, args.mip_levels)
    skybox_faces = collect_skybox_faces(src_dir)

    build_cubemap_dds(ibl_faces, args.ibl_out)
    build_cubemap_dds(skybox_faces, args.skybox_out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
