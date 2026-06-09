# Nanite 在 FalcorRendering 中的生成工具与显示流程

> 目标: 在 FalcorRendering 中实现一套 Nanite 风格的虚拟化微多边形几何系统，包含离线资产生成工具、运行时资源加载、GPU 可见性筛选、LOD 选择、光栅化显示、调试可视化和验证流程。
> 基础: 依托 Falcor 现有的 `SceneBuilder`、`Scene` 全局几何缓冲、`RenderPass`、`RenderGraph`、DX12/Vulkan 图形接口、Compute/Graphics Slang Shader、`Source/Tools` 工具目录和 `Source/Samples` 示例程序。
> 范围: 本文是实现流程设计文档，不直接等同于 UE5 Nanite 的完整复制。优先实现静态三角网格的 Cluster/ClusterGroup 虚拟几何显示，再逐步扩展材质、遮挡、流式加载、动画和光追兼容。

---

## 1. 总体目标

Nanite 风格系统要解决的问题是: 输入高面数模型，离线切分成小规模 Cluster，构建层级 LOD 和依赖关系，运行时只提交当前视角真正需要的 Cluster，并用 GPU 完成筛选与绘制。

最小可用版本需要达成以下能力:

1. 离线工具把普通三角网格转换为 Nanite 资产。
2. 每个 Nanite 资产包含 Cluster、ClusterGroup、层级节点、误差度量、包围体、材质段和压缩顶点/索引数据。
3. Falcor 运行时能加载该资产并创建 GPU Buffer。
4. 渲染端根据相机、屏幕误差、视锥和 HZB 遮挡筛选可见 Cluster。
5. 可见 Cluster 通过软件光栅化或 mesh/task shader 路径写入 Visibility Buffer。
6. 后续材质阶段根据 Visibility Buffer 解析三角、重建属性并执行 Falcor 现有材质/光照。
7. 提供调试视图: Cluster ID、LOD、屏幕误差、遮挡状态、材质 ID、overdraw、三角密度。

---

## 2. 模块落点

建议新增以下模块和文件:

| 模块 | 目录 | 职责 |
|---|---|---|
| NaniteBuilder 工具 | `Source/Tools/NaniteBuilder` | 离线生成 `.fnanite` 资产 |
| Nanite 核心数据 | `Source/Falcor/Scene/Nanite` | 资产格式、加载器、运行时资源对象 |
| Nanite Scene 集成 | `Source/Falcor/Scene` | `SceneData` 增加 Nanite 几何描述和实例数据 |
| Nanite RenderPass | `Source/RenderPasses/NaniteRaster` | GPU 筛选、光栅化和 Visibility Buffer |
| Nanite 示例 | `Source/Samples/NaniteViewer` | 独立显示、调试和性能验证 |
| 文档与测试资源 | `docs/`、`data/nanite/` | 示例资产、截图和流程说明 |

首个版本不建议直接替换 `GBuffer`。先新增独立 `NaniteRaster` pass，输出 `visibility`、`depth`、`normal`、`baseColor` 等调试/材质解析纹理，再与现有 `GBuffer` 或 PBRT/Filament 示例逐步合并。

---

## 3. 离线生成工具完整流程

### 3.1 输入

`NaniteBuilder` 支持以下输入:

- glTF/OBJ/PBRT 导入后的三角网格。
- 仅处理 `Vao::Topology::TriangleList`。
- 静态网格优先，跳过 skinned mesh、curve、SDF 和 procedural primitive。
- 材质按 Falcor `MaterialID` 或导入器材质名拆分。
- 必须具备 position，normal/texcoord/tangent 可缺省并在构建阶段补齐。

命令示例:

```powershell
NaniteBuilder.exe --input data/models/statue/statue.gltf --output data/nanite/statue.fnanite --cluster-tris 128 --group-clusters 32 --target-error 1.0
```

### 3.2 网格预处理

处理步骤:

1. 导入模型并提取每个 mesh 的三角列表。
2. 统一坐标、三角绕序和单位尺度。
3. 删除退化三角形和重复索引。
4. 根据材质拆分 mesh section，避免一个 Cluster 内跨多个材质。
5. 生成缺失 normal/tangent/texcoord。
6. 计算每个三角的面积、法线、质心和 AABB。
7. 建立原始三角到资产三角的 remap 表，供调试和回归使用。

建议复用 `SceneBuilder::processMesh()` 的数据约定，保持顶点属性与 Falcor 的 `PackedStaticVertexData` 能互相转换。

### 3.3 Cluster 构建

Cluster 是运行时筛选和绘制的基本单位。建议约束:

- 每个 Cluster 目标 64 到 128 个三角。
- 顶点数建议不超过 256，便于本地索引使用 `uint8_t` 或 `uint16_t`。
- Cluster 内只包含一种材质。
- Cluster 必须记录 object-space AABB、bounding sphere、cone normal、cone angle、surface area。
- Cluster 保存局部顶点表和局部三角索引。

构建算法:

1. 对三角按空间位置和法线一致性分区。
2. 使用 graph partition 或 Morton 排序初版切分。
3. 控制每个 Cluster 的三角数、顶点数和材质一致性。
4. 对每个 Cluster 重新生成局部顶点表。
5. 计算 cluster error，表示该 Cluster 被父级 LOD 替代时的几何误差。

初版可以用 Morton code + 贪心填充实现，后续再替换为基于 mesh adjacency 的图划分。

### 3.4 ClusterGroup 与 LOD 层级

ClusterGroup 表示一组 sibling clusters，也是 LOD 选择的依赖单位。

构建步骤:

1. 将底层 Cluster 按空间相邻关系合并为 ClusterGroup。
2. 对每个 ClusterGroup 的三角集合执行简化，生成父层 Cluster。
3. 记录 child group 到 parent cluster 的依赖关系。
4. 重复简化直到根层只剩一个或少量 Cluster。
5. 为每个节点计算:
   - object-space bounds
   - parent error
   - child error
   - LOD level
   - material range
   - page range

LOD 选择标准:

```text
screen_error = object_error * projection_scale / distance_to_camera
```

当 `screen_error <= threshold` 时可以使用当前 Cluster，否则继续展开子节点。

### 3.5 顶点和索引压缩

建议资产内存布局:

- position 使用 Cluster 局部 AABB 量化到 16 bit 或 20 bit。
- normal/tangent 使用 octahedral encoding。
- texcoord 使用 16 bit 或按材质需求保留 32 bit。
- index 使用 Cluster 局部索引。
- material ID 单独存储，避免每顶点重复。

初版可以先使用未压缩 `float3 position + packed normal + float2 uv`，保证正确性。压缩作为第二阶段优化。

### 3.6 Page 与流式数据

Page 是运行时加载和上传的单位。

推荐规则:

- 每个 page 包含多个 Cluster。
- page 大小目标 64 KB 到 256 KB。
- 顶层常驻 page 必须在资产头中标记。
- child page 可按需加载。
- 每个 Cluster 记录 `pageID` 和 page 内偏移。

初版可以不做磁盘流式，全部加载到 GPU。资产格式仍保留 page 表，避免后续重构。

### 3.7 输出资产格式

建议扩展名: `.fnanite`

文件布局:

```text
FNaniteHeader
ChunkTable
MaterialTable
MeshTable
InstanceTable
ClusterPageTable
ClusterTable
ClusterGroupTable
HierarchyNodeTable
VertexData
IndexData
DebugRemapData
```

核心结构草案:

```cpp
struct FNaniteHeader
{
    uint32_t magic;          // 'FNAN'
    uint32_t version;
    uint32_t flags;
    uint32_t meshCount;
    uint32_t clusterCount;
    uint32_t clusterGroupCount;
    uint32_t pageCount;
    uint64_t chunkTableOffset;
};

struct NaniteClusterDesc
{
    uint32_t pageID;
    uint32_t vertexOffset;
    uint32_t vertexCount;
    uint32_t indexOffset;
    uint32_t triangleCount;
    uint32_t materialID;
    uint32_t groupID;
    uint32_t lodLevel;
    float4 boundingSphere;
    float3 boundsMin;
    float3 boundsMax;
    float geometricError;
    uint32_t flags;
};

struct NaniteHierarchyNode
{
    uint32_t childNodeOffset;
    uint32_t childNodeCount;
    uint32_t clusterOffset;
    uint32_t clusterCount;
    float4 boundingSphere;
    float minError;
    float maxError;
};
```

---

## 4. Falcor 运行时加载流程

### 4.1 Scene 集成

建议新增数据类型:

- `NaniteMeshDesc`: 一个 Nanite mesh 的 GPU 数据范围和材质范围。
- `NaniteInstanceData`: 实例 transform、meshID、visibility flags。
- `NaniteSceneData`: 全局 cluster/page/hierarchy/vertex/index buffer 描述。
- `NaniteGeometryID`: 区分普通 triangle mesh 和 Nanite mesh。

`Scene::SceneData` 可增加:

```cpp
std::vector<NaniteMeshDesc> naniteMeshDesc;
std::vector<GeometryInstanceData> naniteInstanceData;
ref<NaniteAsset> pNaniteAsset;
```

为了减少初版风险，也可以先不改 `Scene` 核心，先在 `NaniteViewer` 里直接加载 `.fnanite`，等显示链路稳定后再合入 `SceneBuilder`。

### 4.2 导入器策略

推荐两条路径:

1. 离线路径: `NaniteBuilder` 生成 `.fnanite`，运行时 `NaniteAsset::load()` 直接读取。
2. 自动路径: 普通 scene import 后发现高面数 mesh，调用 builder 库生成缓存文件。

初版只做离线路径。自动路径需要处理缓存 key、源文件修改时间、材质映射和多线程构建，放到后续阶段。

### 4.3 GPU Buffer 创建

加载 `.fnanite` 后创建:

| Buffer | 用途 |
|---|---|
| `clusterDescBuffer` | Cluster 元数据 |
| `clusterGroupBuffer` | LOD 依赖和 page 信息 |
| `hierarchyBuffer` | 层级遍历 |
| `pageTableBuffer` | page 驻留状态 |
| `vertexDataBuffer` | 压缩或未压缩顶点数据 |
| `indexDataBuffer` | Cluster 局部索引 |
| `visibleClusterBuffer` | GPU 筛选输出 |
| `drawCommandBuffer` | indirect draw 或 dispatch 参数 |

运行时资源对象建议命名为 `NaniteAsset` 和 `NaniteSceneResources`，分别负责 CPU 文件数据和 GPU Buffer。

---

## 5. 显示管线完整流程

### 5.1 RenderGraph 节点

推荐图形链路:

```text
Scene Load
  -> Depth Prepass / Previous Depth
  -> HZB Build
  -> Nanite Hierarchy Culling
  -> Nanite Cluster Culling
  -> Nanite Rasterize Visibility
  -> Nanite Material Resolve
  -> Lighting / Composite
  -> ToneMapper
```

`NaniteRaster` pass 输入:

- camera matrices
- previous frame depth 或 current depth
- HZB texture
- Nanite buffers
- scene material buffers

输出:

- `visibility`: `uint2` 或 `uint4`，保存 instance/cluster/triangle/barycentric 或 packed primitive ID。
- `depth`: Nanite 深度。
- `normal`: 可选调试输出。
- `baseColor`: 可选材质解析输出。
- `stats`: 可见 Cluster 数、绘制三角数、剔除原因计数。

### 5.2 HZB 构建

HZB 用于遮挡剔除。流程:

1. 使用上一帧 depth 或当前普通几何 depth。
2. 逐级 downsample，保存每层最大深度或最保守深度。
3. Cluster 包围盒投影到屏幕 rectangle。
4. 根据 rectangle 覆盖范围选择 HZB mip。
5. 比较 Cluster 近深度和 HZB 深度，判断遮挡。

初版可以先只做视锥剔除和屏幕误差，不做 HZB，确保能显示。第二阶段加入 HZB。

### 5.3 层级遍历与 LOD 选择

Compute pass: `NaniteHierarchyCull.cs.slang`

输入:

- `hierarchyBuffer`
- `clusterGroupBuffer`
- camera constants
- screen size
- error threshold
- page residency table

输出:

- candidate cluster list
- missing page request list
- traversal stats

逻辑:

```text
从 root node 开始:
  计算 node bounds 是否在视锥内
  计算 screen_error
  如果 page 不驻留:
    写入 page request
    使用父级 fallback cluster
  如果 screen_error 小于阈值:
    输出当前 node/cluster
  否则:
    展开 child node
```

为了避免递归，GPU 上使用 append/consume queue 或双 buffer frontier。

### 5.4 Cluster 剔除

Compute pass: `NaniteClusterCull.cs.slang`

剔除项:

- 视锥剔除。
- 背面 cone 剔除。
- 屏幕尺寸过小剔除。
- HZB 遮挡剔除。
- 材质/可见层过滤。

输出 `visibleClusterBuffer`，供后续光栅化使用。

### 5.5 光栅化路径选择

建议保留两条路径:

| 路径 | 优点 | 适用阶段 |
|---|---|---|
| Software Raster Compute | 不依赖 mesh shader，容易控制 Visibility Buffer 格式 | 初版 |
| Mesh/Task Shader | 更贴近现代 GPU 几何管线，可能更快 | 优化阶段 |

初版软件光栅化流程:

1. 一个 thread group 处理一个 Cluster 或多个小 Cluster。
2. 读取 Cluster 顶点和局部索引。
3. 解压顶点并变换到 clip space。
4. 对三角计算屏幕 bbox。
5. 分 tile 或像素执行 edge function。
6. depth test 后写 `visibility` 和 `depth`。

Visibility Buffer 推荐保存:

```text
visibility.x = instanceID
visibility.y = clusterID
visibility.z = triangleID
visibility.w = packed barycentric/depth flags
```

如果使用 `uint2` 压缩，需要保证 cluster 和 triangle 位数足够，并在文档中明确最大限制。

### 5.6 材质解析

Compute pass: `NaniteMaterialResolve.cs.slang`

流程:

1. 对每个像素读取 `visibility`。
2. 找到 instance、cluster、triangle。
3. 解压三角三个顶点。
4. 用 barycentric 插值 position、normal、tangent、texcoord。
5. 查找 material ID。
6. 调用或复用 Falcor 的材质采样逻辑。
7. 输出 GBuffer 或直接输出 shaded color。

初版可以输出 base color、normal 和 depth，之后再接入完整 PBR lighting。

### 5.7 与现有渲染的合成

有三种集成方式:

1. Nanite 独立显示: `NaniteViewer` 只渲染 Nanite 几何。
2. 混合 GBuffer: 普通 mesh 用现有 `GBuffer`，Nanite 写入同一组 GBuffer，再统一光照。
3. Visibility-only: Nanite 先输出 visibility，后续材质和光照 pass 统一解析。

建议阶段顺序:

1. 独立显示。
2. Nanite depth 与普通 depth 合并。
3. Nanite material resolve 输出兼容 GBuffer。
4. 与 deferred/Filament/PBRT 显示链路合并。

---

## 6. 关键实现阶段

### Phase 1: 资产格式和 CPU 工具

目标:

- 新增 `NaniteBuilder`。
- 能读取一个三角 mesh 并输出 `.fnanite`。
- 资产包含 header、cluster table、vertex data、index data。
- 生成 debug json，记录每个 cluster 的 bounds 和 triangle range。

验收:

- 命令行能处理百万三角以内模型。
- 生成资产可被 loader 重新读回。
- cluster 统计正确，退化三角被剔除。

### Phase 2: 简单显示

目标:

- 新增 `NaniteAsset` loader。
- 新增 `NaniteViewer` sample。
- 全量加载所有 Cluster，不做 LOD 和剔除。
- 使用 compute software raster 输出 depth/baseColor。

验收:

- 模型轮廓正确。
- 深度遮挡正确。
- Cluster ID 调试图稳定。

### Phase 3: LOD 层级

目标:

- Builder 生成 ClusterGroup 和 hierarchy。
- GPU 根据屏幕误差选择 LOD。
- 提供 LOD debug view。

验收:

- 远距离三角数明显下降。
- 近距离无明显裂缝或 popping。
- 相机移动时 LOD 过渡稳定。

### Phase 4: 遮挡剔除和间接绘制

目标:

- 构建 HZB。
- 加入 cluster occlusion culling。
- 生成 visible cluster list 和 indirect args。

验收:

- 被遮挡模型可见 Cluster 数下降。
- GPU stats 显示各类剔除数量。
- 无明显错误遮挡。

### Phase 5: 材质和现有 RenderGraph 集成

目标:

- Nanite material resolve 输出兼容 GBuffer。
- 接入 deferred lighting 或当前样例的 forward/Filament 链路。
- 支持多材质 mesh。

验收:

- base color、normal、roughness/metallic 正确。
- Nanite 和普通 mesh 可在同一场景显示。
- 后处理链路可正常工作。

### Phase 6: Page 驻留和流式加载

目标:

- 资产按 page 存储。
- 运行时维护 page residency table。
- GPU 输出 missing page request，CPU 异步加载并上传。
- 未驻留时使用父级 fallback。

验收:

- 大资产启动不需要一次性上传全部几何。
- 快速移动相机时不会出现大面积空洞。
- page cache 可限制显存预算。

---

## 7. 工具实现细节

### 7.1 `NaniteBuilder` CMake

在 `Source/Tools/CMakeLists.txt` 增加:

```cmake
add_subdirectory(NaniteBuilder)
```

工具目录建议:

```text
Source/Tools/NaniteBuilder/
  CMakeLists.txt
  NaniteBuilder.cpp
  NaniteBuildOptions.h
  NaniteBuildPipeline.h
  NaniteBuildPipeline.cpp
  NaniteClusterBuilder.h
  NaniteClusterBuilder.cpp
  NaniteHierarchyBuilder.h
  NaniteHierarchyBuilder.cpp
  NaniteAssetWriter.h
  NaniteAssetWriter.cpp
```

### 7.2 Builder 数据流

```text
Input Scene
  -> Extract Mesh Sections
  -> Validate Triangles
  -> Generate Missing Attributes
  -> Build Leaf Clusters
  -> Build Parent LOD Clusters
  -> Build Hierarchy Nodes
  -> Quantize/Pack Vertex Data
  -> Build Page Table
  -> Write .fnanite
  -> Write debug .json
```

### 7.3 Builder 参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--cluster-tris` | 128 | 每个 cluster 目标三角数 |
| `--max-cluster-verts` | 256 | 每个 cluster 最大局部顶点数 |
| `--group-clusters` | 32 | 每个 cluster group 目标 cluster 数 |
| `--target-error` | 1.0 | 屏幕误差阈值基准 |
| `--page-size-kb` | 128 | page 目标大小 |
| `--no-compress` | false | 使用未压缩顶点便于调试 |
| `--debug-json` | true | 输出构建统计 |

---

## 8. Shader 与 Buffer 设计

### 8.1 Shader 文件

建议新增:

```text
Source/RenderPasses/NaniteRaster/
  NaniteRaster.cpp
  NaniteRaster.h
  NaniteShared.slangh
  NaniteHierarchyCull.cs.slang
  NaniteClusterCull.cs.slang
  NaniteRasterize.cs.slang
  NaniteMaterialResolve.cs.slang
  NaniteDebug.cs.slang
```

### 8.2 GPU 常量

```cpp
struct NaniteFrameConstants
{
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;
    float4x4 prevViewProj;
    float3 cameraPosition;
    float lodErrorThreshold;
    uint2 frameDim;
    uint debugMode;
    uint enableHZB;
};
```

### 8.3 统计输出

```cpp
struct NaniteStats
{
    uint totalClusters;
    uint testedClusters;
    uint visibleClusters;
    uint frustumCulledClusters;
    uint backfaceCulledClusters;
    uint occludedClusters;
    uint rasterizedTriangles;
};
```

调试 UI 应展示这些计数，并支持固定 LOD、关闭遮挡、关闭 cone culling、显示 cluster bounds。

---

## 9. 验证流程

### 9.1 单元测试

测试项:

- header 读写版本检查。
- cluster 三角数量限制。
- local index 不越界。
- bounds 包含所有三角。
- hierarchy parent bounds 覆盖 child bounds。
- LOD error 单调性。

### 9.2 图像验证

场景:

- 单个高模雕像。
- 多材质建筑模型。
- 大规模实例化岩石或树干。
- 近裁剪面极近视角。
- 远距离密集几何。

输出:

- beauty
- depth
- cluster ID
- LOD level
- visible cluster heatmap
- overdraw heatmap

### 9.3 性能指标

每帧记录:

- asset resident bytes
- visible cluster count
- rasterized triangle count
- hierarchy traversal time
- culling time
- raster time
- material resolve time
- page miss count

建议在 `NaniteViewer` 中加入 CSV dump，便于和普通 GBuffer 渲染对比。

---

## 10. 风险和处理策略

| 风险 | 影响 | 处理 |
|---|---|---|
| LOD 裂缝 | 画面出现缝隙 | ClusterGroup 必须保持边界一致，初版可保守使用相同边界简化 |
| 软件光栅慢 | 高分辨率性能不足 | 先使用 tile binning，再考虑 mesh shader |
| 材质解析复杂 | 难以复用 Falcor 材质系统 | 初版只解析 baseColor/normal，后续接完整 MaterialSystem |
| 流式加载闪烁 | page 未驻留导致空洞 | 使用父级 fallback cluster |
| 动态网格不兼容 | skinned mesh 无法显示 | 初版仅支持静态 mesh，动态对象走普通 GBuffer |
| 光追不兼容 | DXR pass 看不到 Nanite | 初版 Nanite 只参与 raster；后续为 coarse LOD 建 BLAS |

---

## 11. 最小可用提交顺序

1. 新增 `.fnanite` 文件格式定义和 reader/writer。
2. 新增 `NaniteBuilder`，支持单 mesh leaf cluster 输出。
3. 新增 `NaniteAsset`，能创建 GPU Buffer。
4. 新增 `NaniteViewer`，用固定颜色显示所有 cluster。
5. 新增 software raster visibility pass。
6. 新增 material resolve，输出 baseColor/depth/normal。
7. 新增 hierarchy LOD 和 debug view。
8. 新增 HZB occlusion。
9. 新增 page table 和异步 streaming。
10. 合并到主 Scene/RenderGraph。

---

## 12. 与 Falcor 当前结构的对应关系

| 当前结构 | Nanite 对应扩展 |
|---|---|
| `SceneBuilder::Mesh` | Builder 输入 mesh section |
| `SceneBuilder::ProcessedMesh` | 转换为 leaf cluster 的中间数据 |
| `Scene::SceneData::meshDesc` | 增加 `naniteMeshDesc` |
| `Scene::mMeshStaticData` | Nanite 独立压缩 vertex buffer |
| `Scene::draw()` | Nanite 不走普通 draw，走 `NaniteRaster` |
| `GBuffer` | 后续接收 Nanite material resolve 输出 |
| `RenderGraph` | 增加 Nanite cull/raster/resolve pass |
| `Source/Tools` | 增加离线构建工具 |
| `Source/Samples` | 增加调试 viewer |

---

## 13. 初版完成定义

初版完成时应具备:

- 能从一个静态高模生成 `.fnanite`。
- 能在 `NaniteViewer` 中显示该资产。
- 能切换 cluster ID、LOD、depth、normal、baseColor 调试视图。
- 能输出每帧 Nanite stats。
- 能与普通 mesh 同屏显示，至少 depth 正确。
- 文档中记录命令行、资产格式版本和已知限制。

不要求初版支持:

- skinned mesh。
- displacement。
- alpha tested material。
- 完整虚拟纹理。
- 完整 DXR BLAS 同步。
- Nanite 与所有现有 RenderPass 的无缝兼容。

