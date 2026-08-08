# LumenGI 继续实现 TODO / 对话交接单

> 最后整理：2026-08-09。本文记录当前工作区的**实际状态**和下一步入口；完整设计与全阶段拆解分别见 `docs/LumenGI_Technical_Roadmap.md` 和 `task.md`。新对话应先读本文，再按 `task.md` 的 S0→S9 Gate 顺序继续。

## 0. 接手时先确认

- [ ] 实际仓库：`F:\project\FalcorRendering`
- [ ] 当前分支：`codex/lumen-gi`
- [ ] 当前基线：`eb540f6748774680ce0039aaf3ac9279266ec521`（`Fix linearZ slope in GBufferRT (#434)`）
- [ ] 当前修改全部未提交；不要执行 `git reset --hard`、`git clean` 或覆盖未跟踪文件。
- [ ] `.codegraph/` 已存在；定位或理解源码时按 `AGENTS.md` 要求，先用 `codegraph explore`。
- [ ] Codex 工作区到真实仓库的 junction：`C:\Users\Administrator\Documents\ChatGPT\FalcorRendering\_repo -> F:\project\FalcorRendering`。使用 `apply_patch` 时从 `_repo/...` 修改。
- [ ] 用户已批准**任务池并行模型**（2026-08-09）：纯代码 agent 每批可放行 4-6 路（文件所有权互斥即可），不再局限于 3 路；仓库级 MSBuild 仍只能 1 个（`--parallel 1`，C1041 历史坑），GPU 测试仍只能 1 路（单物理 GPU）。构建与 GPU 由 root 串行调度，agent 只写代码。
- [ ] 用户要求核心实现优先处理 C++/Slang，不要把时间转移到 Python 环境问题；现有 Python 文件主要是 Falcor 图和测试入口。

建议接手后的第一组只读命令：

```powershell
$repo = 'F:\project\FalcorRendering'
git -C $repo branch --show-current
git -C $repo status --short
codegraph explore "LumenGI current implementation and RenderPass call paths"
Get-CimInstance Win32_Process |
  Where-Object { ($_.Name -in @('cmake.exe','MSBuild.exe','cl.exe','link.exe')) -and $_.CommandLine -like '*FalcorRendering*' }
```

## 1. 当前阶段结论

| 阶段 | 当前状态 | 说明 |
|---|---|---|
| S0 工程骨架 | Gate 证据基本齐备，尚未在 task.md 打勾 | Release/Debug 全目标编译、CPU 单测 29/29、smoke/validation/hot-reload/GBuffer 对照/图像 run-only 全过，Phase 0 报告已生成（`artifacts\lumengi\S0\phase0-report.md`）。Debug 构建发生于 S1 代码集成前，语义上按 S0 证据存档。 |
| S1 HWRT 基线 | 主体完成，剩 PathTracer 定量对比 | MIS/RR/clamp/NaN/firefly 计数、调试分量（6-11）、displaced anyhit、LightCollection 修复（Cornell GI 恢复非零：mean 0.317/max 61.6）、解析光 on/off 完美归零（cornell_pointlight 场景）、动态回归全 PASS（静态能量平台 growth 1.0014、camera cut/光移动平滑、无 NaN/负值）。待办：PathTracer 256/1024 spp 参考定量对比 + 正式打勾。 |
| S2 Cards/Surface Cache | S2.1 组件齐备，未接入 RenderPass | A: `Cards/LumenCardScene.h/.cpp`（六轴 AABB、96B 布局、UpdateFlags 映射）；B: `SurfaceCache/LumenSurfaceCache.h`（header-only，16x16 tile 页表/LRU/最小驻留/预算，10 测试）；C: `Capture/LumenCardCaptureData.slang`+`LumenCardCapture.3d.slang`+`LumenGICards.py/test`（atlas 布局冻结）；H: `Capture/LumenCaptureScheduler.h`（header-only 调度器，10 测试；root 已把 AtlasFull 压力测试语义修正为"预算=命令数上限"并冻结）。全部已注册进 FalcorTest/LumenGI CMake，39/39 CPU 测试通过。 |
| S3 组件 | Shader 已写，未集成 | J: `Lighting/LumenSurfaceCacheLighting*.slang`（direct cache lighting，Falcor sampler 消费端）。 |
| S4 组件 | Shader 已写，未集成 | root 亲写 `ScreenTrace/LumenScreenTraceData.slang`+`LumenScreenTrace.cs.slang`（精确透视 HZB march；Agent E 三次空返回已弃用）。Falcor 无现成 HZB 工具，S4-A1 需自建。 |
| S6A/S6B 组件 | 工具与 GPU 布局已写 | D: `Source/Tools/MeshSDFBuilder/`（.msdf 格式+hash+检测，161 自测）；G: `MeshSDF/LumenMeshSDF*.slang/.h/.cpp`（R16Float/R8Snorm mip 链、MinAbs 池化、sphere trace，161 自测）；I: `MeshSDF/LumenMeshSDFAtlas.h/.slang`（落盘未验证，S6B 集成时 cl/MSBuild 验证）。**均未注册 CMake**（按 task.md §11 待格式冻结）。 |

不要把当前结果称为“完整 Lumen”或“完整实时 GI”。目前准确名称是：**HWRT GI Baseline / LumenGI 原型 + S2.1 组件库**。

## 1.5 并行开发中的独立组件（未注册/未集成）

- `Source\Tools\MeshSDFBuilder\`（S6-A1，Agent D）：独立 CLI 工具，`.msdf` 自描述格式 + FNV-1a64 hash + open/thin/watertight 检测，纯标准库，`cl /Zs` 已验语法。S6A Gate 过 + 格式冻结后再注册 CMake。
- `tests\lumengi\run_reference.py` / `run_analytic.py` / `run_dynamic.py` / `scenes\cornell_pointlight.pyscene`（S1 Gate 脚本，Agent F）：均已 GPU 实跑通过。
- `Source\RenderPasses\LumenGI\ScreenTrace\`（S4-B1，root 亲写）：精确透视 HZB screen trace，已注册进 CMake 复制列表，运行时编译待 S4-A1 集成验证。
- `Source\RenderPasses\LumenGI\Lighting\LumenSurfaceCacheLighting*.slang`（S3-B1，Agent J）：direct cache lighting，待 S3 集成。
- `Source\RenderPasses\LumenGI\MeshSDF\LumenMeshSDFAtlas.*`（S6-B2，Agent I）：落盘未验证，S6B 集成时 cl/MSBuild 验证。

## 1.6 下一步（root 集成优先序）

1. **S2 集成**（下一大块）：LumenGI.cpp 接线 LumenCardScene + LumenCaptureScheduler + LumenSurfaceCache + Capture 3D pass（每 card 一 draw 写 atlas 页）；S2 Gate（card placement 可视化、coverage、resize/reload、churn）。
2. **S1 收尾**：PathTracer 参考定量对比（run_reference.py 已出 LumenGI 侧数据；需建 PathTracer graph + 固定曝光 + RMSE/FLIP 计算）。
3. **S4-A1**：HZB 构建 pass + screen trace dispatch + miss reason 统计。
4. **S3 集成**：cache lighting compute pass + 解析光/emissive/env sampler 绑定。
5. **S6B**：MeshSDF 集成（注册 CMake、cl 验证 Atlas、GDF clipmap 之后）。

## 2. 已落盘实现

### 2.1 插件和宿主

- [x] `Source/RenderPasses/CMakeLists.txt` 注册 `LumenGI` 子目录。
- [x] `Source/RenderPasses/LumenGI/CMakeLists.txt` 创建插件并复制 Shader。
- [x] `LumenGI.h/.cpp`：RenderPass 注册、属性解析、反射、编译、执行、UI、场景更新和热重载。
- [x] 支持开关：`enabled`、`traceMode`、`qualityPreset`、`debugMode`、Surface Cache/Screen Trace/Screen Probes/Temporal/Spatial/Radiance Cache 预留开关。
- [x] `TraceMode` 已暴露 `HardwareRT / MeshSDF / Hybrid`，但后两者当前仍回退到 HWRT，不能视为已实现。
- [x] 场景更新信号已连接；resize/选项变化会重置当前历史状态。

### 2.2 已冻结的初始通道契约

输入：

- 必需：`vbuffer`、`linearZ`（RG32F）、`mvec`（RG32F）、`normWRoughnessMaterialID`（RGB10A2）、`viewW`（RGBA32F）。
- 可选：`mvecW`、`diffuseOpacity`、`emissive`、`directLighting`。

输出：

- `diffuseGI`：RGBA16F，已经乘 primary diffuse albedo 的最终间接漫反射。
- `diffuseRadianceHitDist`：RGBA16F，未乘 albedo 的 diffuse radiance + hit distance，供 NRD 风格接口使用。
- `confidence`：R16F。
- `bentNormal`：可选 RGBA16F，目前只清零。
- `debugOutput`：RGBA16F。

### 2.3 Shader 和 HWRT 基线

- [x] `LumenGIData.slang`：共享常量和数据定义。
- [x] `LumenGIDebug.cs.slang`：None/Normal/LinearDepth/Motion/MaterialID/Confidence 调试输出。
- [x] `Tracing/LumenHardwareTrace.rt.slang`：每像素一条 cosine-weighted 间接 diffuse ray。
- [x] `Tracing/LumenHitLighting.slang`：环境 miss、emissive secondary hit、解析光采样、alpha-aware inline visibility query 和材质 BSDF 求值。
- [x] RT 配置：1 ray type、1 miss、24-byte payload、最大递归深度 1；覆盖 triangle/displaced triangle/curve/SDF hit/intersection groups。
- [x] Host 会设置 `USE_ENV_LIGHT`、`USE_ANALYTIC_LIGHTS`、`USE_EMISSIVE_LIGHTS`。
- [ ] `Lighting/LumenGILighting.slang` 和 `Lighting/LumenGILightSampling.slang` 已复制进插件，但当前活动 tracing 路径没有 import 它们；集成前必须通过真实运行时编译验证。
- [ ] MIS、Russian roulette 接口、radiance clamp、firefly/NaN/Inf 统计尚未完成。

### 2.4 测试和性能骨架

- [x] `LumenGIStats.h` 与 `LumenGIStatsTests.cpp`。
- [x] `scripts/LumenGIBenchmark.py`、benchmark manifest/schema 和 Python manifest 单测。
- [x] `scripts/LumenGI.py` 主图。
- [x] `tests/lumengi/run_smoke.py`、`run_validation.py`。
- [x] LumenGI 基础、动态、lighting toggle 和 PathTracer reference 图像测试入口已创建。
- [ ] 图像测试目前只完成语法/发现检查，没有冻结 reference，也没有正式比较通过记录。
- [ ] 性能脚本只有骨架，尚无可作为 Gate 的 Phase 0 GPU/显存报告。

## 3. 已验证证据

已生成二进制：

- Release `LumenGI.dll`：`build\windows-vs2022\bin\Release\plugins\LumenGI.dll`，2026-08-08 15:08:43。
- Debug `LumenGI.dll`：`build\windows-vs2022\bin\Debug\plugins\LumenGI.dll`，2026-08-08 15:13:47。
- Release `FalcorTest.exe`：`build\windows-vs2022\bin\Release\FalcorTest.exe`。
- Release `Mogwai.exe`：`build\windows-vs2022\bin\Release\Mogwai.exe`。

已通过：

- [x] CMake configure：`tools\.packman\cmake\bin\cmake.exe --preset windows-vs2022`
- [x] Release `LumenGI` 编译。
- [x] Debug `LumenGI` 编译。
- [x] Release `FalcorTest` 和 `Mogwai` 编译。
- [x] `LumenGIStatsTests.cpp` 6/6：证据在 `artifacts\lumengi\S0\unit-stats.xml`。
- [x] Release D3D12 headless smoke：`artifacts\lumengi\S0\smoke.log`。
- [x] Release Arcade resize/camera validation：`artifacts\lumengi\S0\validation.log`。
- [x] S1 Cornell/Arcade HWRT 运行时编译和执行：`artifacts\lumengi\S1\smoke.log`、`smoke-analytic.log`、`validation-analytic.log`。
- [x] S1 输出 EXR 非空且体积明显非零，证明 tracing 路径写出了 radiance；这不是图像正确性 Gate 的替代品。

尚未验证：

- [ ] Debug `Mogwai` 与 Debug `FalcorTest` 本轮全目标编译。
- [ ] D3D12 debug layer + ray tracing validation 的 Debug 路径。
- [ ] RenderGraph hot reload 和 scene reload 的专门自动回归。
- [ ] GBuffer debug view 像素级对照。
- [ ] LumenGI image tests 和相邻 RenderPass 回归。
- [ ] PathTracer reference 定量比较。
- [ ] 性能、显存、30 分钟 churn/soak。

当前日志只看到 Falcor 既有 Slang implicit-conversion 警告、Arcade emissive UV quantization 警告和 GFX TLAS prebuild warning；未观察到新的 runtime error。继续工作时仍需区分“既有警告”和“LumenGI 新增 validation error”。

## 4. 下一步：先关闭 S0 Gate

- [ ] 修改 `task.md` 的测试命令：当前文档中的 `FalcorTest --filter` 不正确。CPU suite 的已验证命令是：

```powershell
New-Item -ItemType Directory -Force artifacts\lumengi\S0 | Out-Null
build\windows-vs2022\bin\Release\FalcorTest.exe `
  --test-suite LumenGIStatsTests.cpp `
  --xml-report artifacts\lumengi\S0\unit-stats.xml
```

  CPU 单测不要附加 `--device-type d3d12`，否则 device filter 会排除 CPU tests。先用 `FalcorTest.exe --help` 确认其他过滤参数。

- [ ] 单独编译 Debug `Mogwai` 和 `FalcorTest`，保留日志；`LumenGI` Debug 已成功，无需重复证明。
- [ ] 用 `Mogwai.exe --help` 核实 `--enable-raytracing-validation` 是否受当前构建支持，再运行 Debug validation。
- [ ] 增加/执行 hot reload、scene reload、resize 的专门 smoke sequence。
- [ ] 让 debug Normal/Depth/Motion/MaterialID 与 GBuffer 输出做自动或可复现对照。
- [ ] 运行 LumenGI image test 的 run-only 模式；随后运行 GBufferRT/VBufferRT/PathTracer 等相邻回归。
- [ ] 生成 Phase 0 报告：GPU、驱动、分辨率、场景版本、构建配置、GPU time、显存、输出文件列表。
- [ ] S0 Gate 全部满足后，再把 `task.md` S0 对应项和总看板改成 `[x]`。不要只因插件能加载就提前关闭 S0。

推荐构建命令（仓库内 PowerShell）：

```powershell
tools\.packman\cmake\bin\cmake.exe --build build\windows-vs2022 `
  --config Debug --target Mogwai FalcorTest --parallel 1
```

## 5. 下一步：补完并关闭 S1 Gate

- [ ] 完成 BSDF/采样正确性：PDF 与能量单位、MIS、sample sequence、radiance clamp、NaN/Inf guard、firefly 统计。
- [ ] 增加 direct-only / indirect-only / environment / emissive / analytic-light debug 或测试分量。
- [ ] 检查透明、alpha-test、curve、displaced triangle、SDF geometry 的 hit/fallback 行为。
- [ ] 确认 scene/TLAS 更新后 RT vars 与 binding table 正确重建，不使用陈旧数据。
- [ ] 跑 `test_LumenGIReference.py`：固定 seed、固定曝光，使用 256/1024 spp PathTracer reference，给出 Cornell 定量误差和图像差异。
- [ ] 跑 `test_LumenGILighting.py`：分别关闭解析光、环境光、自发光，验证对应分量消失。
- [ ] 跑 `test_LumenGIDynamic.py`：camera pan/orbit/cut、光源移动、刚体移动、材质与 emissive 变化。
- [ ] 验证静态长帧无能量发散，所有输出无 NaN/Inf。
- [ ] 所有 S1 Gate 通过后，阶段名称仍保持 `HWRT GI Baseline`。

## 6. S2 可恢复的三路并行任务

此前三个 subagent 都已中断，**没有 S2 文件落盘**。恢复时可重新分配以下互斥所有权：

### Agent A：Card scene / placement

- [ ] 新建 `Source/RenderPasses/LumenGI/Cards/LumenCardManager.h/.cpp`（或按 `task.md` 统一命名为 `LumenCardScene.*`，先由 root 冻结名称）。
- [ ] 新建 `Cards/LumenCardPlacement.cs.slang`。
- [ ] 用 `Scene::getGeometryInstanceCount()`、`getGeometryInstance()`、`getMeshBounds()`、`AnimationController::getGlobalMatrices()` 建立静态 mesh 六轴 AABB Cards。
- [ ] 维护 mesh/instance/card 映射、bounds、优先级和 dirty flags。
- [ ] 不修改 `LumenGI.cpp/.h` 和中央 CMake；由 root 集成。

### Agent B：Surface Cache / page allocator

- [ ] 新建 `SurfaceCache/LumenSurfaceCache.h/.cpp` 和必要共享结构。
- [ ] 固定 tile atlas、free-list、LRU、最小驻留帧、页表、resize/reset、显存/驻留统计。
- [ ] 先写 CPU allocator 单测：重复分配、atlas 满、eviction、最小驻留、resize、scene reload。
- [ ] 不修改 root-owned 集成文件。

### Agent C：Card capture / 测试

- [ ] 新建 Card capture shader，输出 base color、normal、roughness、emissive、opacity、depth。
- [ ] 明确定义 double-sided、alpha-test、unsupported material fallback。
- [ ] 新建 `test_LumenGICards.py` 及 placement/coverage/residency/eviction debug 测试入口。
- [ ] 不修改 root-owned 集成文件。

### Root 集成与 S2 Gate

- [ ] 先审 API、数据布局、线程/资源生命周期和格式，再统一命名。
- [ ] 只由 root 修改 `LumenGI.cpp/.h`、`LumenGI/CMakeLists.txt` 和中央测试 CMake，避免并行冲突。
- [ ] 一次只运行一个仓库级 MSBuild；三个 agent 完成后统一 Release 编译和运行时 Shader 编译。
- [ ] 增加 card placement、atlas occupancy、page age、dirty reason、coverage/fallback debug output。
- [ ] 验证 Cornell/Sponza/Bistro、resize、scene reload、atlas pressure、局部材质/实例失效和 30 分钟 churn。
- [ ] 达成 `task.md` S2 Gate 后再进入 S3。

## 7. S3-S9 执行原则

- [ ] 阶段内容、依赖 DAG、各 Wave 和 Gate 以 `task.md` 第 8-14 节为准。
- [ ] 严格顺序：S2 Surface Cache → S3 Cache Lighting → S4 Screen Trace/Screen Probes → S5 Temporal/Spatial → S6 Mesh SDF/GDF/SWRT → S7 Radiance Cache/Far Field → S8 优化/质量档 → S9 全量发布回归。
- [ ] S6A 可以在接口冻结后提前独立开发，但不能在 Card/Surface Cache 数据格式未冻结时贸然集成。
- [ ] 每个 Wave 最多三个 subagent 并行，root 同时做接口审查、集成和单一构建调度。
- [ ] 每个 agent 只拥有独立目录/文件；中央 CMake、`LumenGI.cpp/.h`、共享 ABI 和总测试入口由 root 维护。
- [ ] 每个阶段必须保存 build/unit/image/dynamic/validation/perf/memory 证据，再更新 `task.md` 看板。
- [ ] 任何阶段失败均记录复现命令、首个错误、责任文件、修复和复测结果；不通过降低阈值来“过 Gate”。

## 8. 已知坑与避免重复工作

- [ ] **不要并发跑多个 MSBuild。** 此前重叠构建触发过 `C1041` PDB 写入冲突。仓库级构建使用 `--parallel 1` 或 `/m:1`。
- [ ] **Shader 被复制不等于已编译。** Falcor 的 Slang shader 多在运行 RenderGraph 时才实际编译；每个新 shader 都要通过真实 Mogwai 路径触发。
- [ ] **先创建日志目录。** `Mogwai --logfile artifacts\...` 的父目录不存在时 Logger 会抛异常；这不是渲染代码故障。
- [ ] **不要用 `FalcorTest --filter`。** 当前可执行文件使用 `--test-suite` / `--test-case`；先查 `--help`。
- [ ] `clearOutputs()` 在 trace 前清空所有输出；HWRT 随后写 required outputs，`bentNormal` 仍为清零状态。
- [ ] `MeshSDF` 和 `Hybrid` 目前只是 UI/属性占位并回退到 HWRT。
- [ ] `Lighting/*.slang` 当前未接入活动路径，不要把“文件存在”误判为功能完成。
- [ ] 现有 EXR 不能直接交给当前 `view_image`；需要先用仓库工具或受控转换生成 PNG，再做视觉检查。
- [ ] `.codegraph/` 是接手前已存在的未跟踪目录，不要删除或提交，除非用户明确要求。
- [ ] 当前所有 LumenGI 改动均未 commit；不要在没有用户要求时擅自 stage/commit/push。

## 9. 当前修改范围

已修改的跟踪文件：

- `Source/RenderPasses/CMakeLists.txt`
- `Source/Tools/FalcorTest/CMakeLists.txt`

主要新增目录/文件：

- `Source/RenderPasses/LumenGI/`
- `Source/Tools/FalcorTest/Tests/RenderPasses/LumenGIStatsTests.cpp`
- `scripts/LumenGI.py`
- `scripts/LumenGIBenchmark.py`
- `tests/image_tests/renderpasses/graphs/LumenGI*.py`
- `tests/image_tests/renderpasses/test_LumenGI*.py`
- `tests/lumengi/`
- `docs/LumenGI_Technical_Roadmap.md`
- `task.md`
- `artifacts/lumengi/`

## 10. 新对话可直接使用的开场指令

```text
继续 F:\project\FalcorRendering 的 codex/lumen-gi 分支。先完整阅读 todo.md、task.md 和 docs/LumenGI_Technical_Roadmap.md；当前所有改动未提交，不要 reset/clean/stage/commit。先用 CodeGraph 核实 LumenGI 当前调用路径和 Falcor API，再关闭 todo.md 中剩余的 S0 Gate，补完 S1 HWRT Baseline Gate，然后按 S2-S9 最大化三路 subagent 并行开发、root 单点集成和逐阶段回归。一次只允许一个 MSBuild，所有新 Shader 必须通过 Mogwai 真实运行时编译。持续执行到所有 Gate 完成；不要把当前原型提前称为完整 Lumen。
```
