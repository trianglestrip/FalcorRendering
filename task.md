# Nanite Task Plan

This task list follows `docs/NANITE_IN_FALCOR_PLAN.md` and tracks the branch-level implementation work for `feature/nanite`.

## Phase 0: Branch, Plan, And Tool MVP

- [x] Create `feature/nanite` branch.
- [x] Add Nanite implementation plan in `docs/NANITE_IN_FALCOR_PLAN.md`.
- [x] Add `Source/Tools/Nanite/` as the folder for Nanite command-line tools and shared tool code.
- [x] Add `NaniteToolCore` static library for shared `.fnanite` asset structures, binary read/write, validation, OBJ import, and cluster building.
- [x] Add `NaniteBuilder` C++20 executable.
- [x] Add `NaniteLoader` C++20 executable.
- [x] Wire Nanite tools into `Source/Tools/CMakeLists.txt`.
- [x] Configure `windows-vs2022` CMake preset after adding the targets.
- [x] Build `NaniteBuilder` and `NaniteLoader` in Release.
- [x] Verify end-to-end generation and loading with `data/framework/meshes/cube.obj`.

## Phase 1: Builder Quality

- [ ] Replace the initial sequential cluster packing with spatial ordering, starting with Morton sorting by triangle centroid.
- [ ] Add adjacency-aware cluster partitioning for better boundary quality.
- [ ] Add degenerate triangle filtering and report removed triangle counts in debug JSON.
- [ ] Add per-cluster surface area and normal cone statistics.
- [ ] Add optional vertex deduplication inside source mesh sections before clustering.
- [ ] Add stable source triangle remap data for debug and regression.
- [ ] Add glTF input path by reusing Falcor import or a small dedicated importer.
- [ ] Add PBRT input path or support generated mesh sections from the PBRT importer.

## Phase 2: Asset Format V2

- [ ] Add chunk table with explicit offsets and sizes.
- [ ] Add ClusterGroup table.
- [ ] Add hierarchy node table.
- [ ] Add page table while keeping the MVP loader able to load all data eagerly.
- [ ] Add file format version migration checks.
- [ ] Add compressed position storage using cluster-local bounds.
- [ ] Add octahedral normal packing.
- [ ] Add optional uncompressed debug mode.

## Phase 3: Runtime Asset Loading

- [ ] Move reusable asset format code from `Source/Tools/Nanite/Common` into an engine-side module under `Source/Falcor/Scene/Nanite`.
- [ ] Keep the tool-side code as a thin wrapper around the engine-side asset API.
- [ ] Add `NaniteAsset` runtime object.
- [ ] Add GPU buffer creation for cluster, vertex, index, material, and hierarchy data.
- [ ] Add runtime validation for buffer ranges and table version.
- [ ] Add memory usage reporting.

## Phase 4: Nanite Viewer

- [ ] Add `Source/Samples/NaniteViewer`.
- [ ] Load `.fnanite` from command line.
- [ ] Display asset bounds and per-cluster debug information in UI.
- [ ] Add camera controls and simple scene setup.
- [ ] Add debug draw for cluster AABBs.
- [ ] Add screenshots and regression scene paths.

## Phase 5: Raster Display MVP

- [ ] Add `Source/RenderPasses/NaniteRaster`.
- [ ] Add `NaniteShared.slangh`.
- [ ] Add visibility buffer texture output.
- [ ] Add software raster compute path for all resident clusters.
- [ ] Add depth output.
- [ ] Add cluster ID debug output.
- [ ] Add material resolve for position, normal, texcoord, and base color.
- [ ] Integrate `NaniteRaster` into `NaniteViewer`.

## Phase 6: GPU Culling And LOD

- [ ] Add hierarchy traversal compute pass.
- [ ] Add screen-space error LOD selection.
- [ ] Add frustum culling.
- [ ] Add normal cone backface culling.
- [ ] Add visible cluster append buffer.
- [ ] Add indirect dispatch or indirect draw arguments.
- [ ] Add debug modes for LOD level, screen error, and cull reason.

## Phase 7: HZB Occlusion

- [ ] Add HZB builder pass.
- [ ] Add HZB sampling helpers.
- [ ] Add cluster occlusion culling.
- [ ] Add conservative bounds projection.
- [ ] Add occlusion debug heatmap.
- [ ] Add counters for tested, visible, and occluded clusters.

## Phase 8: Scene And RenderGraph Integration

- [ ] Add Nanite mesh descriptors to `Scene::SceneData`.
- [ ] Add Nanite instance data.
- [ ] Add SceneBuilder path for prebuilt `.fnanite` assets.
- [ ] Add mixed rendering with regular GBuffer geometry.
- [ ] Merge Nanite depth with regular scene depth.
- [ ] Output GBuffer-compatible material resolve data.
- [ ] Add RenderGraph scripting support.

## Phase 9: Streaming

- [ ] Add page residency table.
- [ ] Add missing page request buffer.
- [ ] Add CPU upload path for requested pages.
- [ ] Add parent fallback when child pages are missing.
- [ ] Add page memory budget.
- [ ] Add stats for resident bytes and page misses.

## Phase 10: Tests And Documentation

- [ ] Add unit tests for `.fnanite` read/write.
- [ ] Add unit tests for cluster range validation.
- [ ] Add unit tests for bounds containment.
- [ ] Add image tests for `NaniteViewer`.
- [ ] Add performance CSV dump.
- [ ] Add usage documentation for `NaniteBuilder` and `NaniteLoader`.
- [ ] Add sample asset generation script.

