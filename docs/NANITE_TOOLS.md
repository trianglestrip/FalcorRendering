### [Index](./index.md) | Nanite Tools

--------

# Nanite Builder and Loader

Command-line tools for offline `.fnanite` asset generation and validation. Implementation lives under `Source/Tools/Nanite/`; the overall Nanite plan is described in [NANITE_IN_FALCOR_PLAN.md](./NANITE_IN_FALCOR_PLAN.md).

## Prerequisites

Build the tools with the `windows-vs2022` CMake preset (Release recommended):

```powershell
cmake --preset windows-vs2022
cmake --build build/windows-vs2022 --config Release --target NaniteBuilder NaniteLoader
```

Executables are written to:

```text
build/windows-vs2022/bin/Release/NaniteBuilder.exe
build/windows-vs2022/bin/Release/NaniteLoader.exe
```

## Quick start (cube end-to-end)

From the repository root:

```powershell
build\windows-vs2022\bin\Release\NaniteBuilder.exe -i data\framework\meshes\cube.obj -o data\nanite\cube.fnanite --debug-json
build\windows-vs2022\bin\Release\NaniteLoader.exe -i data\nanite\cube.fnanite --list-clusters
```

Or run the sample asset script:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_nanite_assets.ps1
```

Expected builder output includes mesh/material/cluster counts and a `Wrote:` line for the `.fnanite` file. The loader prints `Validation: OK` when the asset passes internal checks.

## NaniteBuilder

Converts a triangle mesh into a Nanite-style clustered `.fnanite` asset.

### Usage

```text
NaniteBuilder --input <model.obj> [--output <asset.fnanite>] [options]
```

Positional input is supported: if the first non-option argument is not prefixed with `-`, it is treated as the input path.

### Options

| Option | Default | Description |
|---|---|---|
| `-i`, `--input <path>` | *(required)* | Input OBJ file. |
| `-o`, `--output <path>` | `<input>.fnanite` | Output `.fnanite` path. |
| `--cluster-tris <count>` | `128` | Target triangles per cluster. |
| `--max-cluster-verts <count>` | `256` | Maximum local vertices per cluster. |
| `--workers <count>` | hardware thread count | Taskflow worker threads for mesh-parallel cluster build. |
| `--debug-json [path]` | `<output>.fnanite.json` | Write a JSON build summary (cluster bounds, triangle remap, stats). |
| `-h`, `--help` | | Print usage and exit. |

### Current limitations (MVP)

- **Input:** OBJ only. glTF and PBRT import are planned (see [NANITE_IN_FALCOR_PLAN.md](./NANITE_IN_FALCOR_PLAN.md) §3.1).
- **LOD / pages:** Leaf clusters only; no hierarchy, cluster groups, or page table yet.
- **Compression:** Vertices are stored uncompressed (32 bytes per vertex: position, normal, texcoord).

### Build pipeline

```text
OBJ load  ->  mesh sections / materials  ->  Morton-ordered cluster packing
         ->  validate  ->  write .fnanite  ->  optional debug JSON
```

Cluster construction uses taskflow for mesh-level parallelism. Results are merged in source order so repeated builds produce stable output.

### Debug JSON

When `--debug-json` is set, the builder writes a sidecar file with:

- Global counts (meshes, materials, clusters, vertices, triangles)
- Source and degenerate triangle counts
- Asset bounds
- Per-cluster: mesh/material indices, triangle and vertex counts, bounds, normal cone, surface area, and source triangle remap

Use this file to inspect cluster boundaries and to compare builder changes during regression work.

### Example

```powershell
build\windows-vs2022\bin\Release\NaniteBuilder.exe `
  --input data\framework\meshes\cube.obj `
  --output data\nanite\cube.fnanite `
  --cluster-tris 128 `
  --max-cluster-verts 256 `
  --workers 4 `
  --debug-json data\nanite\cube.fnanite.json
```

## NaniteLoader

Loads a `.fnanite` file, runs validation, and prints a summary. Useful for CI checks and manual inspection.

### Usage

```text
NaniteLoader --input <asset.fnanite> [options]
```

### Options

| Option | Description |
|---|---|
| `-i`, `--input <path>` | Input `.fnanite` file *(required)*. |
| `--list-meshes` | Print the mesh table after validation. |
| `--list-materials` | Print the material name table. |
| `--list-clusters` | Print the cluster table (offsets, counts, bounds metadata). |
| `-h`, `--help` | Print usage and exit. |

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Loaded and validated successfully. |
| `1` | Load or parse error (missing file, bad magic/version, corrupt tables). |
| `2` | Loaded but failed `validateAsset` checks. |

### Example

```powershell
build\windows-vs2022\bin\Release\NaniteLoader.exe -i data\nanite\cube.fnanite --list-meshes --list-clusters
```

## `.fnanite` file format (V1)

The current on-disk format is **version 1** (`kNaniteVersion = 1`, magic `FNAN`). Layout:

```text
Header (counts, bounds, block offsets)
Mesh table
Material table (string-table name offsets)
Cluster table
Vertex buffer (32-byte Vertex records)
Index buffer (uint32)
String table (mesh and material names)
```

Each cluster record stores mesh/material indices, vertex and index ranges, triangle count, axis-aligned bounds, bounding sphere, normal cone, geometric error, and surface area. The loader verifies that all ranges stay within the vertex and index buffers and that cluster bounds contain their vertices.

Version 2 (chunk table, cluster groups, hierarchy, pages) is specified in [NANITE_IN_FALCOR_PLAN.md](./NANITE_IN_FALCOR_PLAN.md) §3.6–3.7 and is not implemented yet.

## Sample assets

Generated example assets are written under `data/nanite/`. Regenerate them with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_nanite_assets.ps1
```

The script builds `cube.fnanite` from `data/framework/meshes/cube.obj`, writes debug JSON, and runs `NaniteLoader` to confirm the asset. Additional meshes (glTF, PBRT, larger scenes) can be appended to the script as import paths land in Phase 1.

## Related documentation

- [NANITE_IN_FALCOR_PLAN.md](./NANITE_IN_FALCOR_PLAN.md) — full Nanite roadmap, runtime integration, and planned format V2
- [development/cmake.md](./development/cmake.md) — CMake presets and build layout
- [task.md](../task.md) — implementation checklist
