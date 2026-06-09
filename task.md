# Nanite 任务计划

本文档跟踪 `feature/nanite` 分支的 Nanite 实现任务，任务拆分依据 `docs/NANITE_IN_FALCOR_PLAN.md`。每个阶段都需要完成实现、构建验证和最小端到端验证后再进入下一阶段。

## Phase 0：分支、文档与工具 MVP

- [x] 创建 `feature/nanite` 分支。
- [x] 新增 Nanite 总体实现文档 `docs/NANITE_IN_FALCOR_PLAN.md`。
- [x] 新增 `Source/Tools/Nanite/`，作为 Nanite 命令行工具和共享工具代码目录。
- [x] 新增 `NaniteToolCore` 静态库，负责 `.fnanite` 资产结构、二进制读写、校验、OBJ 导入和 Cluster 构建。
- [x] 新增 C++20 可执行工具 `NaniteBuilder`。
- [x] 新增 C++20 可执行工具 `NaniteLoader`。
- [x] 将 Nanite 工具接入 `Source/Tools/CMakeLists.txt`。
- [x] 使用 `windows-vs2022` preset 重新配置 CMake。
- [x] Release 构建 `NaniteBuilder` 和 `NaniteLoader`。
- [x] 使用 `data/framework/meshes/cube.obj` 完成生成和加载的端到端验证。
- [x] 引入 taskflow，并将 mesh 级 Cluster 构建改为 taskflow DAG 并行执行。
- [x] 为 `NaniteBuilder` 增加 `--workers` 参数，允许控制 taskflow worker 数量。

## Phase 1：Builder 质量提升

- [ ] 将当前顺序打包 Cluster 改为空间排序，第一步使用三角形质心 Morton 排序。
- [ ] 增加基于邻接关系的 Cluster 划分，提升边界质量。
- [ ] 增加退化三角形过滤，并在 debug JSON 中输出剔除数量。
- [ ] 增加每个 Cluster 的 surface area 和 normal cone 统计。
- [ ] 增加 source mesh section 内的可选顶点去重。
- [ ] 增加稳定的 source triangle remap 数据，便于调试和回归测试。
- [ ] 增加 glTF 输入路径，优先复用 Falcor importer 或使用小型专用导入器。
- [ ] 增加 PBRT 输入路径，或支持从 PBRT importer 生成的 mesh section 构建。

## Phase 2：资产格式 V2

- [ ] 增加 chunk table，显式记录各块 offset 和 size。
- [ ] 增加 ClusterGroup table。
- [ ] 增加 hierarchy node table。
- [ ] 增加 page table，同时保留 MVP loader 的全量 eager load 能力。
- [ ] 增加文件格式版本迁移和兼容性检查。
- [ ] 增加基于 Cluster local bounds 的 position 压缩。
- [ ] 增加 octahedral normal packing。
- [ ] 增加未压缩 debug 模式。

## Phase 3：运行时资产加载

- [ ] 将可复用资产格式代码从 `Source/Tools/Nanite/Common` 迁移到 `Source/Falcor/Scene/Nanite`。
- [ ] 工具侧保留为 engine-side asset API 的薄包装。
- [ ] 新增运行时对象 `NaniteAsset`。
- [ ] 为 cluster、vertex、index、material、hierarchy 数据创建 GPU Buffer。
- [ ] 增加运行时表范围和格式版本校验。
- [ ] 增加 Nanite 资产内存占用统计。

## Phase 4：Nanite Viewer

- [ ] 新增 `Source/Samples/NaniteViewer`。
- [ ] 从命令行加载 `.fnanite`。
- [ ] 在 UI 中显示资产 bounds 和 per-cluster 调试信息。
- [ ] 增加相机控制和简单场景初始化。
- [ ] 增加 Cluster AABB debug draw。
- [ ] 增加截图和回归场景路径。

## Phase 5：Raster 显示 MVP

- [ ] 新增 `Source/RenderPasses/NaniteRaster`。
- [ ] 新增 `NaniteShared.slangh`。
- [ ] 增加 visibility buffer 输出纹理。
- [ ] 增加全驻留 Cluster 的 software raster compute 路径。
- [ ] 增加 depth 输出。
- [ ] 增加 Cluster ID 调试输出。
- [ ] 增加 material resolve，输出 position、normal、texcoord 和 base color。
- [ ] 将 `NaniteRaster` 集成到 `NaniteViewer`。

## Phase 6：GPU Culling 与 LOD

- [ ] 增加 hierarchy traversal compute pass。
- [ ] 增加 screen-space error LOD 选择。
- [ ] 增加 frustum culling。
- [ ] 增加 normal cone backface culling。
- [ ] 增加 visible cluster append buffer。
- [ ] 增加 indirect dispatch 或 indirect draw 参数生成。
- [ ] 增加 LOD level、screen error 和 cull reason 调试模式。

## Phase 7：HZB 遮挡

- [ ] 增加 HZB builder pass。
- [ ] 增加 HZB sampling helper。
- [ ] 增加 Cluster occlusion culling。
- [ ] 增加保守 bounds projection。
- [ ] 增加遮挡调试 heatmap。
- [ ] 增加 tested、visible、occluded Cluster 计数器。

## Phase 8：Scene 与 RenderGraph 集成

- [ ] 在 `Scene::SceneData` 中增加 Nanite mesh descriptor。
- [ ] 增加 Nanite instance data。
- [ ] 为预构建 `.fnanite` 资产增加 SceneBuilder 加载路径。
- [ ] 支持 Nanite 与普通 GBuffer geometry 混合渲染。
- [ ] 合并 Nanite depth 与普通 scene depth。
- [ ] 输出兼容 GBuffer 的 material resolve 数据。
- [ ] 增加 RenderGraph scripting 支持。

## Phase 9：Streaming

- [ ] 增加 page residency table。
- [ ] 增加 missing page request buffer。
- [ ] 增加 CPU 侧 page 上传路径。
- [ ] 当 child page 未驻留时使用 parent fallback。
- [ ] 增加 page 显存预算。
- [ ] 增加 resident bytes 和 page miss 统计。

## Phase 10：测试与文档

- [ ] 增加 `.fnanite` 读写单元测试。
- [ ] 增加 Cluster range 校验测试。
- [ ] 增加 bounds containment 测试。
- [ ] 增加 `NaniteViewer` 图像测试。
- [ ] 增加性能 CSV dump。
- [ ] 增加 `NaniteBuilder` 和 `NaniteLoader` 使用文档。
- [ ] 增加示例资产生成脚本。

