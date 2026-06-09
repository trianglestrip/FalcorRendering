# Nanite 剩余任务 — 并行工作流拆分

> 本文档从 `task.md` 提取 **Phase 1–10 全部未完成项**，按依赖层级重组为可分配给不同 Agent 的独立工作流。  
> Phase 0 已完成；**Phase 1 剩余项为最高优先级**，应优先于 Phase 2+ 启动。  
> 设计依据：`docs/NANITE_IN_FALCOR_PLAN.md`。

---

## 使用说明

1. 每个工作流（WS）可独立领取；同层内 WS 默认互不阻塞。
2. 合并 PR 时注意 **软冲突** 标注（多 WS 可能改同一文件）。
3. 每个 WS 完成后：Release 构建相关目标 + 该 WS 列出的验收命令。
4. 完成后在本文档对应 checkbox 打勾，并同步回 `task.md`（或由集成 Agent 统一同步）。

---

## 依赖层级总览

| 层级 | 可并行 WS | 说明 |
|------|-----------|------|
| **Layer 0** | WS-P1-ADJ, WS-P1-DEDUP, WS-P1-GLTF, WS-P1-PBRT, WS-P10-DOC | 立即开工；Phase 1 四项 + 早期文档 |
| **Layer 1** | WS-P2-FORMAT, WS-P10-UNIT | 格式 V2 骨架；V1 单元测试可并行 |
| **Layer 2** | WS-P2-COMPRESS | 顶点压缩与 debug 模式 |
| **Layer 3** | WS-P3-RUNTIME | 引擎侧运行时加载 |
| **Layer 4** | WS-P4-VIEWER | 独立 Viewer 示例 |
| **Layer 5** | WS-P5-RASTER | Software raster MVP |
| **Layer 6** | WS-P6-CULLING | GPU 筛选与 LOD |
| **Layer 7** | WS-P7-HZB | HZB 遮挡 |
| **Layer 8** | WS-P8-SCENE | Scene / RenderGraph 集成 |
| **Layer 9** | WS-P9-STREAM | Page 流式加载 |
| **Layer 10** | WS-P10-E2E | 图像回归与性能 dump |

---

## Layer 0 — 可立即并行（Phase 1 优先）

### WS-P1-ADJ：邻接关系 Cluster 划分

**来源 Phase 1**

- [x] 增加基于邻接关系的 Cluster 划分，提升边界质量。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | 在现有 Morton 排序 + 贪心填充基础上，引入 mesh 邻接图（共享边）驱动的分区，减少 Cluster 边界切割和顶点膨胀 |
| **依赖** | 无（Phase 0 已完成） |
| **建议路径** | `Source/Tools/Nanite/Common/NaniteBuild.cpp`、`NaniteBuild.h`；参考 `docs/NANITE_IN_FALCOR_PLAN.md` §3.3 |
| **验收标准** | ① `cmake --preset windows-vs2022` + Release 构建 `NaniteBuilder`/`NaniteLoader` 通过；② `NaniteBuilder -i data/framework/meshes/cube.obj -o /tmp/cube.fnanite` 成功；③ debug JSON 中 cluster 数合理、边界顶点数相对 Morton-only 有改善（与同 mesh 旧版对比或新增回归指标） |
| **复杂度** | **M** |
| **软冲突** | `NaniteBuild.cpp` — 与 WS-P1-DEDUP 协调合并顺序 |

---

### WS-P1-DEDUP：Section 内可选顶点去重

**来源 Phase 1**

- [x] 增加 source mesh section 内的可选顶点去重。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | 构建前在同一 material section 内合并位置（及可选属性）相同的顶点；CLI 增加 `--dedup-verts`（或等价开关） |
| **依赖** | 无 |
| **建议路径** | `Source/Tools/Nanite/Common/NaniteBuild.cpp`、`NaniteBuilder/NaniteBuilder.cpp`（CLI）；`NaniteObj.cpp` 预处理入口 |
| **验收标准** | ① 构建通过；② 对含重复顶点的 OBJ，开启去重后顶点数下降、cluster 输出仍合法；③ `NaniteLoader` 加载校验通过 |
| **复杂度** | **S** |
| **软冲突** | `NaniteBuild.cpp`、`NaniteBuilder.cpp` |

---

### WS-P1-GLTF：glTF 输入路径

**来源 Phase 1**

- [x] 增加 glTF 输入路径，优先复用 Falcor importer 或使用小型专用导入器。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | `NaniteBuilder --input *.gltf` 可导入三角网格并走现有 `buildNaniteAsset` 管线；输出与 OBJ 路径一致的 `.fnanite` |
| **依赖** | 无 |
| **建议路径** | 新建 `Source/Tools/Nanite/Common/NaniteGltf.cpp/.h`（或 `NaniteImport.cpp`）；`NaniteBuilder.cpp` 扩展输入分发；可参考 `Source/Falcor/Scene/SceneBuilder.h`、`Importer.h` 的数据约定 |
| **验收标准** | ① 构建通过；② 至少一个 glTF 测试资产（如 `data/` 下现有模型）成功生成 `.fnanite` 且 `NaniteLoader` 校验通过；③ 多 material section 正确拆分 |
| **复杂度** | **L**（若需链接 Falcor Scene 库则 **M–L**） |

---

### WS-P1-PBRT：PBRT 输入路径

**来源 Phase 1**

- [x] 增加 PBRT 输入路径，或支持从 PBRT importer 生成的 mesh section 构建。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | 支持 PBRT 场景/mesh 作为 builder 输入，或接受与 PBRT importer 相同 layout 的中间 mesh section |
| **依赖** | 无 |
| **建议路径** | 新建 `Source/Tools/Nanite/Common/NanitePbrt.cpp/.h`；`NaniteBuilder.cpp`；参考 `Source/Falcor/Scene/Material/PBRT/`、`Source/Samples/PBRTOfflineRenderer/` |
| **验收标准** | ① 构建通过；② 至少一个 PBRT 测试场景生成 `.fnanite`；③ material / section 边界与 OBJ 路径行为一致 |
| **复杂度** | **M** |

---

### WS-P10-DOC：Builder / Loader 早期文档与脚本骨架

**来源 Phase 10（可提前）

- [x] 增加 `NaniteBuilder` 和 `NaniteLoader` 使用文档。
- [x] 增加示例资产生成脚本。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | `docs/` 下 Nanite 工具 README（命令行、参数、输出说明）；`scripts/` 或 `data/nanite/` 下 PowerShell/Python 脚本骨架，调用 `NaniteBuilder` 生成示例资产 |
| **依赖** | 无（脚本可先用 `cube.obj`，后续 Layer 1+ 扩展 glTF/PBRT） |
| **建议路径** | `docs/NANITE_TOOLS.md`（新建）、`data/nanite/`（新建）、`scripts/build_nanite_assets.ps1`（新建） |
| **验收标准** | ① 文档中的命令可在本机复现 Phase 0 的 cube 端到端流程；② 脚本 exit 0 并产出 `.fnanite` |
| **复杂度** | **S** |

---

## Layer 1 — 资产格式 V2 骨架 + V1 单元测试

> **前置**：建议 Layer 0 的 WS-P1-* 至少合并一项后再开 WS-P2-FORMAT，避免 builder 输出剧烈变动导致格式工作返工。WS-P10-UNIT 可与 WS-P2-FORMAT **并行**（针对当前 V1）。

### WS-P2-FORMAT：`.fnanite` V2 表结构与兼容

**来源 Phase 2**

- [x] 增加 chunk table，显式记录各块 offset 和 size。
- [x] 增加 ClusterGroup table。
- [x] 增加 hierarchy node table。
- [x] 增加 page table，同时保留 MVP loader 的全量 eager load 能力。
- [x] 增加文件格式版本迁移和兼容性检查。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | 版本号升至 V2；实现 chunk / ClusterGroup / hierarchy / page 表读写；`NaniteLoader` 仍可 eager load 全量数据；V1 文件可读或给出明确错误 |
| **依赖** | Layer 0 Phase 1  builder 稳定度（软依赖） |
| **建议路径** | `Source/Tools/Nanite/Common/NaniteAsset.h/.cpp`、`NaniteLoader/NaniteLoader.cpp`、`NaniteBuilder/NaniteBuilder.cpp`；规格见 `docs/NANITE_IN_FALCOR_PLAN.md` §3.6–3.7 |
| **验收标准** | ① 构建通过；② cube.obj → V2 `.fnanite` → `NaniteLoader` 校验通过；③ 故意损坏 chunk 时 loader 报错；④ V1 资产行为符合兼容策略 |
| **复杂度** | **L** |

---

### WS-P10-UNIT：格式与 Cluster 校验单元测试

**来源 Phase 10**

- [x] 增加 `.fnanite` 读写单元测试。
- [x] 增加 Cluster range 校验测试。
- [x] 增加 bounds containment 测试。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | 在 FalcorTest 或独立 Nanite 测试中覆盖读写往返、cluster 索引范围、cluster AABB 包含所有顶点 |
| **依赖** | 无硬依赖（先测 V1；WS-P2-FORMAT 合并后扩展 V2 case） |
| **建议路径** | `Source/Tools/FalcorTest/Tests/` 下新建 `Nanite/`；或 `Source/Tools/Nanite/` 内 gtest 目标；测试数据 `data/framework/meshes/cube.obj` |
| **验收标准** | ① Release 构建测试目标；② 全部测试 green；③ CI 可 `-run_test Nanite*`（若项目支持） |
| **复杂度** | **M** |

---

## Layer 2 — 压缩与 Debug 模式

### WS-P2-COMPRESS：顶点压缩与未压缩 Debug 模式

**来源 Phase 2**

- [x] 增加基于 Cluster local bounds 的 position 压缩。
- [x] 增加 octahedral normal packing。
- [x] 增加未压缩 debug 模式。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | Cluster 局部量化 position；octahedral normal/tangent 打包；header flags 控制 compressed / debug uncompressed；loader 解压或直通 |
| **依赖** | **WS-P2-FORMAT** |
| **建议路径** | `Source/Tools/Nanite/Common/NaniteAsset.cpp`、新建 `NaniteCompress.cpp/.h` |
| **验收标准** | ① 构建通过；② 同一 mesh 压缩/非压缩模式 loader 校验通过；③ 解压后顶点与 debug 模式误差在阈值内 |
| **复杂度** | **M** |

---

## Layer 3 — 运行时资产加载

### WS-P3-RUNTIME：引擎侧 Nanite 资产与 GPU Buffer

**来源 Phase 3**

- [x] 将可复用资产格式代码从 `Source/Tools/Nanite/Common` 迁移到 `Source/Falcor/Scene/Nanite`。
- [x] 工具侧保留为 engine-side asset API 的薄包装。
- [x] 新增运行时对象 `NaniteAsset`。
- [x] 为 cluster、vertex、index、material、hierarchy 数据创建 GPU Buffer。
- [x] 增加运行时表范围和格式版本校验。
- [x] 增加 Nanite 资产内存占用统计。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | 新目录 `Source/Falcor/Scene/Nanite/` 含共享格式 + `NaniteAsset` 类；工具链链接共享库或 thin wrapper；GPU upload 与内存统计 API |
| **依赖** | **WS-P2-FORMAT**（**WS-P2-COMPRESS** 可选，可先支持 uncompressed） |
| **建议路径** | `Source/Falcor/Scene/Nanite/`（新建）、`Source/Falcor/CMakeLists.txt`、`Source/Tools/Nanite/CMakeLists.txt` |
| **验收标准** | ① Falcor + NaniteBuilder + NaniteLoader 构建通过；② 小型 host 测试或 NaniteLoader 调用 engine API 加载 V2 资产并打印 GPU buffer 大小；③ 非法表范围触发校验失败 |
| **复杂度** | **L** |

---

## Layer 4 — Nanite Viewer

### WS-P4-VIEWER：独立查看器示例

**来源 Phase 4**

- [x] 新增 `Source/Samples/NaniteViewer`。
- [x] 从命令行加载 `.fnanite`。
- [x] 在 UI 中显示资产 bounds 和 per-cluster 调试信息。
- [x] 增加相机控制和简单场景初始化。
- [x] 增加 Cluster AABB debug draw。
- [x] 增加截图和回归场景路径。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | 基于 `SampleAppTemplate` 的新 Sample；CLI `--fnanite <path>`；ImGui 调试面板；线框/ AABB overlay；固定相机截图路径 |
| **依赖** | **WS-P3-RUNTIME** |
| **建议路径** | `Source/Samples/NaniteViewer/`（新建）、`Source/Samples/CMakeLists.txt`；参考 `Source/Samples/Visualization2D/`、`SampleAppTemplate/` |
| **验收标准** | ① Release 构建 `NaniteViewer`；② 加载 cube.fnanite 显示 bounds 与 cluster 列表；③ 截图写入约定路径（供 Layer 10 图像测试） |
| **复杂度** | **M** |

---

## Layer 5 — Raster 显示 MVP

### WS-P5-RASTER：NaniteRaster Pass 与 Viewer 集成

**来源 Phase 5**

- [x] 新增 `Source/RenderPasses/NaniteRaster`。
- [x] 新增 `NaniteShared.slangh`。
- [x] 增加 visibility buffer 输出纹理。
- [x] 增加全驻留 Cluster 的 software raster compute 路径。
- [x] 增加 depth 输出。
- [x] 增加 Cluster ID 调试输出。
- [x] 增加 material resolve，输出 position、normal、texcoord 和 base color。
- [x] 将 `NaniteRaster` 集成到 `NaniteViewer`。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | 新 RenderPass + compute shader 软件光栅；visibility / depth / clusterID / gbuffer-like resolve；Viewer render loop 调用 |
| **依赖** | **WS-P4-VIEWER**、**WS-P3-RUNTIME** |
| **建议路径** | `Source/RenderPasses/NaniteRaster/`（新建）、`NaniteShared.slangh`；参考 `Source/RenderPasses/GBuffer/`、`RenderPasses/SceneDebugger/` |
| **验收标准** | ① 构建 pass 插件；② NaniteViewer 中可见 depth + cluster ID 视图；③ material resolve 输出非黑截图；④ 无 GPU validation error |
| **复杂度** | **L** |

---

## Layer 6 — GPU Culling 与 LOD

### WS-P6-CULLING：层级遍历、LOD 与视锥/背面剔除

**来源 Phase 6**

- [x] 增加 hierarchy traversal compute pass。
- [x] 增加 screen-space error LOD 选择。
- [x] 增加 frustum culling。
- [x] 增加 normal cone backface culling。
- [x] 增加 visible cluster append buffer。
- [x] 增加 indirect dispatch 或 indirect draw 参数生成。
- [x] 增加 LOD level、screen error 和 cull reason 调试模式。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | Compute pass 遍历 hierarchy，按屏幕误差选 LOD；frustum + cone culling；append visible clusters；驱动后续 raster indirect args；debug 视图 |
| **依赖** | **WS-P5-RASTER**、**WS-P2-FORMAT**（hierarchy 表） |
| **建议路径** | `Source/RenderPasses/NaniteRaster/` 下新增 `.cs.slang`；扩展 `NaniteShared.slangh` |
| **验收标准** | ① 构建通过；② 拉远相机时 cluster 数下降；③ debug 模式可显示 cull reason；④ 剔除后画面与全量 raster 在 LOD 阈值内一致 |
| **复杂度** | **L** |

---

## Layer 7 — HZB 遮挡

### WS-P7-HZB：HZB 构建与 Cluster 遮挡剔除

**来源 Phase 7**

- [x] 增加 HZB builder pass。
- [x] 增加 HZB sampling helper。
- [x] 增加 Cluster occlusion culling。
- [x] 增加 conservative bounds projection。
- [x] 增加遮挡调试 heatmap。
- [x] 增加 tested、visible、occluded Cluster 计数器。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | 由 depth 构建 HZB；cluster bounds 投影采样；occlusion cull 接入 WS-P6 流水线；heatmap + 计数器 |
| **依赖** | **WS-P6-CULLING** |
| **建议路径** | `Source/RenderPasses/NaniteRaster/`；可参考其他 pass 的 depth pyramid 模式（仓库内暂无现成 HZB 模块） |
| **验收标准** | ① 构建通过；② 遮挡场景（如墙后物体）occluded 计数 > 0；③ heatmap 可视化；④ 无 false negative 导致的明显空洞（保守投影） |
| **复杂度** | **L** |

---

## Layer 8 — Scene 与 RenderGraph 集成

### WS-P8-SCENE：SceneBuilder 与 GBuffer 混合渲染

**来源 Phase 8**

- [x] 在 `Scene::SceneData` 中增加 Nanite mesh descriptor。
- [x] 增加 Nanite instance data。
- [x] 为预构建 `.fnanite` 资产增加 SceneBuilder 加载路径。
- [x] 支持 Nanite 与普通 GBuffer geometry 混合渲染。
- [x] 合并 Nanite depth 与普通 scene depth。
- [x] 输出兼容 GBuffer 的 material resolve 数据。
- [x] 增加 RenderGraph scripting 支持。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | Scene 数据结构扩展；`.fnanite` 经 SceneBuilder 加载；RenderGraph 脚本可插入 NaniteRaster；与 GBuffer pass 共存 |
| **依赖** | **WS-P5-RASTER**（**WS-P7-HZB** 可选） |
| **建议路径** | `Source/Falcor/Scene/Scene.h`、`SceneBuilder.cpp/.h`、`Source/Falcor/Scene/Nanite/`；RenderGraph Python 示例 `tests/image_tests/` |
| **验收标准** | ① 构建 Falcor + 至少一个混合场景 Sample/脚本；② pyscene 加载 fnanite + 普通 mesh；③ GBuffer 通道可见 Nanite 几何；④ depth 合并正确 |
| **复杂度** | **L** |

---

## Layer 9 — Streaming

### WS-P9-STREAM：Page 驻留与按需上传

**来源 Phase 9**

- [x] 增加 page residency table。
- [x] 增加 missing page request buffer。
- [x] 增加 CPU 侧 page 上传路径。
- [x] 当 child page 未驻留时使用 parent fallback。
- [x] 增加 page 显存预算。
- [x] 增加 resident bytes 和 page miss 统计。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | GPU residency 表；miss 请求 + CPU 异步上传；parent cluster fallback；VRAM budget 淘汰；统计 HUD |
| **依赖** | **WS-P3-RUNTIME**、**WS-P2-FORMAT**（page table）、**WS-P6-CULLING**（运行时遍历） |
| **建议路径** | `Source/Falcor/Scene/Nanite/`、`Source/RenderPasses/NaniteRaster/` |
| **验收标准** | ① 构建通过；② 大资产仅部分 page 驻留时可渲染（fallback 可见）；③ 统计面板显示 resident bytes / miss count；④ 预算下调时 page  eviction 正常 |
| **复杂度** | **L** |

---

## Layer 10 — 端到端测试与性能

### WS-P10-E2E：图像回归与性能 CSV

**来源 Phase 10**

- [x] 增加 `NaniteViewer` 图像测试。
- [x] 增加性能 CSV dump。

| 项 | 内容 |
|----|------|
| **范围 / 交付物** | `tests/image_tests/` 下 NaniteViewer 截图对比；Viewer 或 pass 输出 cluster/cull/raster 耗时 CSV |
| **依赖** | **WS-P4-VIEWER**、**WS-P5-RASTER**（图像）；**WS-P6-CULLING** / **WS-P7-HZB**（性能项可选） |
| **建议路径** | `tests/image_tests/renderpasses/` 或 `tests/image_tests/samples/`（新建 Nanite 目录）；`Source/Samples/NaniteViewer/` |
| **验收标准** | ① `tests/run_image_tests.bat` 包含 Nanite case 且 pass；② CSV 列含 frame time / visible clusters；③  golden image 差异在阈值内 |
| **复杂度** | **M** |

> **说明**：WS-P10-DOC 与 WS-P10-UNIT 已在 Layer 0 / Layer 1 拆分；本 WS 覆盖 Phase 10 剩余 E2E 项。

---

## 并行分配建议（多 Agent）

```
Agent A ──► WS-P1-ADJ  ──► WS-P2-FORMAT ──► WS-P2-COMPRESS ──► WS-P3-RUNTIME
Agent B ──► WS-P1-GLTF ──► WS-P10-UNIT   ──► WS-P10-E2E (部分)
Agent C ──► WS-P1-PBRT ──► WS-P10-DOC
Agent D ──► WS-P1-DEDUP
Agent E ──► （Layer 3 完成后）WS-P4-VIEWER ──► WS-P5-RASTER ──► WS-P6-CULLING
Agent F ──► （Layer 6 完成后）WS-P7-HZB ──► WS-P8-SCENE ──► WS-P9-STREAM
```

**最大并行度**：Layer 0 可同时跑 **5** 个 WS（注意 `NaniteBuild.cpp` 合并）。  
**关键路径**：P1 → P2-FORMAT → P3-RUNTIME → P4-VIEWER → P5-RASTER → P6 → P7/P8 → P9。

---

## 原 task.md 未完成项索引

| Phase | 未完成数 | 对应 WS |
|-------|----------|---------|
| 1 | 4 | WS-P1-ADJ, WS-P1-DEDUP, WS-P1-GLTF, WS-P1-PBRT |
| 2 | 8 | WS-P2-FORMAT, WS-P2-COMPRESS |
| 3 | 6 | WS-P3-RUNTIME |
| 4 | 6 | WS-P4-VIEWER |
| 5 | 8 | WS-P5-RASTER |
| 6 | 7 | WS-P6-CULLING |
| 7 | 6 | WS-P7-HZB |
| 8 | 7 | WS-P8-SCENE |
| 9 | 6 | WS-P9-STREAM |
| 10 | 7 | WS-P10-DOC, WS-P10-UNIT, WS-P10-E2E |

**合计：65 项**，均已映射到上述 16 个工作流。
