# LumenGI 接手入口（2026-08-10 CodeGraph 复核）

> [!IMPORTANT]
> **当前唯一权威执行计划：[`docs/LumenGI_Production_Chain_Closure_Plan.md`](docs/LumenGI_Production_Chain_Closure_Plan.md)。**
> 目标仍是实现借鉴 UE5 Lumen 公开架构思想的实时动态 GI；本次调整的是生产主链闭环、完成定义和执行顺序，不是把项目改成离线渲染或普通 HWRT 降噪器。
> **UE5.8 参考对齐计划：[`docs/LumenGI_UE5.8_Reference_Optimization_Plan.md`](docs/LumenGI_UE5.8_Reference_Optimization_Plan.md)。** 该文档记录 `F:\UE_5.8` CodeGraph 初始化状态、源码契约、C4 Trace Router 优先级、C0-C12 依赖和 Luna 接手提示。

> [!WARNING]
> **当前权威状态以本节和上述生产主链计划为准。** 本文件后半部分保留的 2026-08-09 状态、Gate 结论、下一步和 agent 分工均为历史记录，仅用于追溯证据；即使旧段落写有“Gate 已关闭”“下一步”或可直接复制的开场指令，也不得据此跳过新的 C0→C12 执行顺序。

## 当前权威状态

| 原阶段 | 2026-08-10 状态 | 当前可采信结论 |
|---|---|---|
| S0 工程骨架 | **完成** | 构建、基础测试和诊断骨架证据可保留。 |
| S1 HWRT Baseline | **完成** | 单反弹 HWRT 基线可作为回退和对照；不代表完整 LumenGI。 |
| S2 Cards / Surface Cache | **组件完成，生产集成未闭环** | Cards、capture、atlas/页管理证据可保留；cache radiance 尚未证明被最终 GI 消费。 |
| S3 Cache Lighting | **组件完成，C1 部分执行；生产集成未闭环** | env importance sampler 变体仍触发 `E_INVALIDARG`；当前使用 uniform-environment fallback，all-on 可运行但不能关闭 Gate。 |
| S4 Screen Trace / Screen Probe | **组件完成，生产集成未闭环** | HZB、screen trace、probe trace/integrate/interpolate 可运行；命中辐射来源与场景追踪路由仍不完整。 |
| S5 Temporal / Spatial | **组件完成，生产集成未闭环** | 时域、空域组件测试可保留；滤波结果尚未通过 Final Resolve 成为最终 `diffuseGI`。 |
| S6 Mesh SDF / GDF / Hybrid | **未完成：控制流和 Hybrid 错误** | 组件存在，但 GDF 执行顺序、MeshSDF fallback 和真正 Hybrid 都必须重做并重验。 |
| S7 Radiance Cache / Far Field | **CPU-only** | 仅 CPU 数据结构/单测可采信；GPU 生产主链尚未接线。 |
| S8 Quality Preset / 优化 | **部分接线** | preset 表存在，运行时主要只接入 cache-lighting samples/texel；probe、分辨率、预算和重建等参数尚未完整生效。 |
| S9 发布 Gate | **未开始** | 必须等待生产主链、画质、动态稳定、性能和多场景多视角证据全部闭环。 |

当前准确名称是：**LumenGI 组件原型 + HWRT Baseline，生产主链尚未闭环**。组件文件存在、debug 输出非零、CPU 单测通过或 checkbox 可切换，都不能单独关闭 Production Integration Gate。

## 下一步：严格按 C0→C12 推进

详细任务、修改范围、Gate、证据目录和回滚条件均以 [`docs/LumenGI_Production_Chain_Closure_Plan.md`](docs/LumenGI_Production_Chain_Closure_Plan.md) 为准；本表只作为接手导航。

| 顺序 | 闭环项 | 最小完成判据 |
|---|---|---|
| C0 | 冻结失败复现与可信截图/参考协议 | C0.1 已记录稳定失败和 dispatch telemetry；C0.2 capture 已生成 raw/probe/temporal/spatial/PT，resolved/final 明确 SKIP。 |
| C1 | 修复 Arcade cache-lighting `E_INVALIDARG`（当前为 env sampler fallback） | 重新启用 environment importance sampler 后，Arcade 解析光、环境光、发光同时开启且 validation 无新增错误。 |
| C2 | 修复 800x450/非 8 倍数分辨率问题 | 640x360、800x450、1280x720 均稳定运行且资源/dispatch 边界正确。 |
| C3 | 修复 MeshSDF 无 GDF 时的黑帧回退 | trace mode × feature toggle 矩阵无黑帧，缺失能力明确回退 HWRT。 |
| C4 | 重排 GDF 并建立 Trace Router 数据契约 | GDF/HWRT 等后端统一 hit record，GDF hit 在 Probe Integrate 前可见。 |
| C5 | 实现真正 Hybrid 与 backend counters | 同帧可证明 SDF/GDF 命中和 HWRT fallback；GDF 不再只是 debug 输出。 |
| C6 | 接入 Surface Cache radiance lookup、generation 与 fallback | cache radiance 被 Probe hit lighting 消费；开关/失效会对最终 GI 产生可解释差异。 |
| C7 | 修复跨帧 Screen Probe 方向采样 | 方向集跨帧新增有效样本，8→32→96 帧误差持续下降。 |
| C8 | 内部化生产中间资源 | 算法不依赖 RenderGraph `markOutput()`；export/debug 开关不改变最终数值。 |
| C9 | Final Resolve 接入最终 `diffuseGI` | Probe→Temporal→Spatial/Upscale→Resolve 全链生效，输出完成材质调制并进入最终合成。 |
| C10 | Radiance Cache/Far Field GPU 接线 | query/refresh/fallback/预算进入最终 GI，cache miss 不黑屏且无持续显存增长。 |
| C11 | 完整 Quality Preset 与性能优化 | Low/Medium/High/Reference 参数单调、热切换稳定，画质和性能均有证据。 |
| C12 | 完整发布矩阵 | 多场景三视角、动态、validation、三轮性能和 30min/2h soak 全部通过。 |

## 2026-08-10 runtime evidence delta

The implementation pass has now exercised the executable C0-C9 work in order.
Use the detailed table in the production plan as the authority:

- C2 has a current GPU matrix; C7 has history/count evidence but its direction-union gate is still open; C8/C9 have current host/runtime evidence with the mark-off endpoint contract still open.
- C1 is still partial because the environment importance sampler remains on the safe fallback (`envSampler=0`).
- C3 fallback is usable, but C4/C5 are blocked by `E_INVALIDARG` in `runGDFCompose` with `useGDF=true`; MeshSDF/GDF must not be marked complete.
- C6 has host lookup/history wiring but still needs an on/off invalidation comparison.
- The final showcase is a resolved indirect-radiance composite preview. `finalColor` is intentionally not claimed until a production composite exists.
- C10-C12 remain deferred until the open C1/C4/C5 gates are closed; do not start Radiance Cache GPU work early.

执行约束：C0–C12 的生产依赖默认串行；仅互不写同一文件的诊断、测试资产和证据整理可并行。仓库级 MSBuild 与 GPU 测试仍由 root 各自单路调度。C0–C9 完成前不得接 C10–C12，禁止以降低画质阈值代替修复。

---

# 历史快照：2026-08-09 交接单（只读追溯，不再作为执行入口）

> [!CAUTION]
> 以下内容原样保留用于追溯旧证据、旧文件位置和历史决策，其中的状态表、S0→S9 顺序、下一步清单、并行 agent 分工及开场指令均已被上方权威入口取代。

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
| S1 HWRT 基线 | **Gate 已关闭（2026-08-09）** | emissive NEE 已接入（LightBVHSampler，Cornell coverage 0.29%→18.8%）；方向一致 cosine 0.946；公平覆盖率基准（1-spp PT 对比 ratio 0.936）冻结；Pearson/z 移出 gating。证据：`artifacts/lumengi/S1/reference-compare/metrics2.json`+`report2.md`（Agent L）。剩余：多样本/时序累积提升覆盖（S2/S3 范畴，R1）。 |
| S2 Cards/Surface Cache | **Gate 已关闭（2026-08-09）** | CardsOverlay debug 视图（M）+ cardCoverage 通道（root 修正为 R32Float + 卡片覆盖率语义）+ 周期 capture 统计日志；coverage=1.0（Cornell/Arcade）；churn 60s 无泄漏（alloc 恒定 12、residentBytes 61440 恒定、fail/lost=0）；debug layer 零 error；CPU 39/39。nightly 项：30 分钟 churn、capture image tests。 |
| S3 Cache Lighting | **集成完成（2026-08-09），Gate 部分证据** | P 集成：cache lighting compute pass 接入 LumenGI.cpp（render-list 模式、独立 counters、cacheDirectRadiance 可选通道、质量档映射、useCacheLighting 开关）；root 修复：CB static_assert、SampleGenerator defines、**sampler 条件声明 + _EMISSIVE_LIGHT_SAMPLER_TYPE pin NULL**（根治 RT program 重编译 dxc 崩溃 + cornell_pointlight dispatchRays E_INVALIDARG 回归）。验证：cacheDirectRadiance 非零有限、emissive 分量 on/off 响应、稳定性 15/15 PASS（黑房间不自发增亮/白炉平台/强 emissive clamp/能量有界）、灯光阶跃跑通。S3 Gate 剩余：cache lighting vs hit-lighting reference 对比、S3-B2 多反弹反馈、动态光更新延迟。 |
| S3 组件 | Shader 已写，未集成 | J: `Lighting/LumenSurfaceCacheLighting*.slang`（direct cache lighting，Falcor sampler 消费端）。 |
| S4 组件 | Shader 已写，未集成 | root 亲写 `ScreenTrace/LumenScreenTraceData.slang`+`LumenScreenTrace.cs.slang`（精确透视 HZB march；Agent E 三次空返回已弃用）。Falcor 无现成 HZB 工具，S4-A1 需自建。 |
| S5 Temporal/Spatial | **S5-B1 集成完成、S5-B2 集成完成（2026-08-09），S5 Gate 4/5 关闭** | S5-A1/B1（Z7）：temporal history ping-pong + 相机 cut 检测，`run_temporal_verify.py` 14/14 PASS。S5-B2（Z10）：variance-guided spatial filter 接线（`spatialFiltered` 通道、gConfidenceInput 置信度来源 = `temporalConfidence`、CB 经 `LumenReconstruction::makeSpatialFilterCB`），spatial gate 14/14 + ghost 4/4 PASS。S5 门禁：camera cut 失效 ✓、动态拖影 ✓、NaN/负/溢出 ✓、flicker ✓；half-res 重建（S5-A2 upscale）冻结为 S8 项。证据 `artifacts/lumengi/S5/gate/`。剩余：Z6 `run_temporal_ghost.py` 地板标定 bug、S5-C2 NRD/SVGF 对比。 |
| S6A/S6B 组件 | 工具与 GPU 布局已写 | D: `Source/Tools/MeshSDFBuilder/`（.msdf 格式+hash+检测，161 自测）；G: `MeshSDF/LumenMeshSDF*.slang/.h/.cpp`（R16Float/R8Snorm mip 链、MinAbs 池化、sphere trace，161 自测）；I: `MeshSDF/LumenMeshSDFAtlas.h/.slang`（落盘未验证，S6B 集成时 cl/MSBuild 验证）。**均未注册 CMake**（按 task.md §11 待格式冻结）。 |

不要把当前结果称为“完整 Lumen”或“完整实时 GI”。目前准确名称是：**HWRT GI Baseline / LumenGI 原型 + S2.1 组件库**。

## 1.5 并行开发中的独立组件（未注册/未集成）

- `Source\Tools\MeshSDFBuilder\`（S6-A1，Agent D）：独立 CLI 工具，`.msdf` 自描述格式 + FNV-1a64 hash + open/thin/watertight 检测，纯标准库，`cl /Zs` 已验语法。S6A Gate 过 + 格式冻结后再注册 CMake。
- `tests\lumengi\run_reference.py` / `run_analytic.py` / `run_dynamic.py` / `scenes\cornell_pointlight.pyscene`（S1 Gate 脚本，Agent F）：均已 GPU 实跑通过。
- `Source\RenderPasses\LumenGI\ScreenTrace\`（S4-B1，root 亲写）：精确透视 HZB screen trace，已注册进 CMake 复制列表，运行时编译待 S4-A1 集成验证。
- `Source\RenderPasses\LumenGI\Lighting\LumenSurfaceCacheLighting*.slang`（S3-B1，Agent J）：direct cache lighting，已集成（S3）。
- `Source\RenderPasses\LumenGI\MeshSDF\LumenGlobalDistanceField.h` + 测试（S6-A3，Agent Q2，2026-08-09 落盘）：相机中心多级 clipmap、ceil 取整、滚动/dirty/驻留、预算，CPU 测试全绿。
- `tests\lumengi\run_screentrace.py` / `run_hzb_check.py`（S4 测试资产，Agent R2）：骨架 + S4_TODO 契约。
- `Source\RenderPasses\LumenGI\Spatial\`（S5-B2，Z8 shader + Z10 集成）：`LumenSpatialFilterData.slang`/`LumenSpatialFilter.cs.slang`（variance-guided bilateral + firefly clamp，已接线）+ `LumenReconstruction.h`（S5-A2 纯 CPU：full/half/quarter 尺寸、CB mirror、upscale 权重；half/quarter 为 S8 项）。已注册 CMake 复制列表。
- `tests\lumengi\run_spatial_gate.py` / `run_spatial_ghost.py` / `run_spatial_diag.py`（S5-B2 gate 资产，Agent Z10）：GPU 已跑 PASS。
- **待办（Atlas 打磨）**：`LumenMeshSDFAtlas` 4 个 CPU 测试失败（SharedPagesAcrossInstances 引用计数、MultiBrickTiling/NonUniformScaleRoundTrip 采样精度、EvictionAndReload）——S6B 轮修复。

## 1.6 下一步（root 集成优先序）

1. **S5 收尾**：冻结 Z6 `run_temporal.py`/`run_temporal_ghost.py` 的 S5_TODO 阈值（moving-light ghost 地板按场景标定、`get_material` pybind bug、animated_cubes 无动画）；S5-C2 NRD/SVGF 对比。
2. **S3-B2 多反弹反馈**（P2 被中断未做）：S3 gate 关键项（白炉收敛、动态光延迟）。
3. **S3 Gate 收尾**：cache lighting vs hit-lighting reference 对比、动态光更新延迟。
4. **S6B**：Atlas 4 个测试修复 + MeshSDF 集成 + GDF clipmap 接线。
5. **S8（half-res 质量档）**：用 `LumenReconstruction` 冻结的 half/quarter 尺寸 + bilateral upscale 实现 S5-A2 half-res GI（gate 项“half-res 接近 full-res reference”在此关闭）。

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
- `Source/RenderPasses/LumenGI/LumenGI.cpp`
- `Source/RenderPasses/LumenGI/LumenGI.h`
- `Source/RenderPasses/LumenGI/CMakeLists.txt`
- `Source/RenderPasses/LumenGI/Spatial/LumenSpatialFilterData.slang`
- `Source/RenderPasses/LumenGI/Spatial/LumenSpatialFilter.cs.slang`
- `task.md`

主要新增目录/文件：

- `Source/RenderPasses/LumenGI/`
- `Source/RenderPasses/LumenGI/Spatial/`（LumenReconstruction.h 等）
- `Source/Tools/FalcorTest/Tests/RenderPasses/LumenGIStatsTests.cpp`
- `scripts/LumenGI.py`
- `scripts/LumenGIBenchmark.py`
- `tests/image_tests/renderpasses/graphs/LumenGI*.py`
- `tests/image_tests/renderpasses/test_LumenGI*.py`
- `tests/lumengi/`（含本轮新增 `run_spatial_gate.py` / `run_spatial_ghost.py` / `run_spatial_diag.py`）
- `docs/LumenGI_Technical_Roadmap.md`
- `task.md`
- `artifacts/lumengi/`（含本轮 `artifacts/lumengi/S5/gate/`）

## 10. 新对话可直接使用的开场指令

```text
继续 F:\project\FalcorRendering 的 codex/lumen-gi 分支。先完整阅读 todo.md、task.md 和 docs/LumenGI_Technical_Roadmap.md；当前所有改动未提交，不要 reset/clean/stage/commit。先用 CodeGraph 核实 LumenGI 当前调用路径和 Falcor API，再关闭 todo.md 中剩余的 S0 Gate，补完 S1 HWRT Baseline Gate，然后按 S2-S9 最大化三路 subagent 并行开发、root 单点集成和逐阶段回归。一次只允许一个 MSBuild，所有新 Shader 必须通过 Mogwai 真实运行时编译。持续执行到所有 Gate 完成；不要把当前原型提前称为完整 Lumen。
```
## Runtime evidence delta (2026-08-11)

- C1: fallback-only production state reverified. Keep `kUseCacheLightingEnvImportanceSampler=false` until the enabled variant stops returning D3D12 `E_INVALIDARG`.
- C4/C5: E0 `(1,1,1)` compose experiment still fails, so the next bounded task is no-op shader + single-UAV descriptor bisection. Do not start C10.
- C4 source follow-up: MeshSDF fine/coarse runtime atlas staging is now R32Float with raw float uploads to match `Texture3D<float>`; rebuild and runtime-compile before treating the fix as valid. E1/E2 diagnostic shaders exist, but host wiring is still pending.
- C6: surface-cache lookup/invalidation/low-budget GPU Gate PASS; artifact `artifacts/lumengi/C6/surfacecache-effect-20260811`.
- C8/C9: mark-on/export equivalence PASS for both filter policies in `artifacts/lumengi/C8/export-equivalence-20260811d/export-equivalence.json`; mark-off cases are correctly `BLOCKED` because direct production endpoints are unavailable without graph marking. Sentinel data is diagnostic only; `finalColor` is still SKIP.
- Host fix: GDF sphere trace now receives logical frame dimensions; rerun only after compose dispatch is fixed.
- Latest GPU evidence: fresh R32Float atlas build still returns `E_INVALIDARG` in `artifacts/lumengi/C4/r32-verified-20260811.log`; E1/E2 compile PASS is recorded in `artifacts/lumengi/C4/gdf-diag-compile-20260811d.json` (the harness then hits shutdown-only DXGI_DEVICE_REMOVED). C7 history/count is finite and monotonic in `artifacts/lumengi/C7/probe-direction-union-20260811/probe-direction-union.json`, while `directionUnionGate=SKIP` until sample identity telemetry exists.

### Runtime evidence delta (2026-08-22)

- Live renderer provenance is now available through `m.device.info` after the
  additive Mogwai binding and Release rebuild. The 60-second smoke is a
  provenance PASS, not a soak result.
- C9 same-frame endpoint/resource invariance is `PASS_BOUNDED`; strict
  recompiled export equivalence remains `FAIL/OPEN` at the frozen error limits.
- S2 30-minute dynamic churn completed, but the 2-hour churn failed at
  `m.renderFrame()` with `MemoryError: bad allocation` after approximately
  422.6 seconds. This is a renderer resource-lifetime blocker; do not lower
  mutation cadence or mark the soak complete.
- Keep the final completion checklist below intentionally unmarked until the
  renderer lifetime issue, C9 strict equivalence, production rough/transmission
  paths, and the remaining release matrix are closed with authoritative evidence.

### Runtime evidence delta (2026-08-12)

- C4 E1 passes with host wiring and a fresh binary: `artifacts/lumengi/C4/E1-20260812/mogwai.log`.
- C4 E2 full descriptors and production compose still fail `E_INVALIDARG`; valid production groups `(8,1,512)` are logged in `artifacts/lumengi/C4/production-20260812/mogwai.log`.
- E2a CB+GDF buffers and E2b atlas/scalars pass independently (`artifacts/lumengi/C4/E2a-20260812b/gdf-diagnostic.json`, `artifacts/lumengi/C4/E2b-20260812/gdf-diagnostic.json`). Keep C4/C5 BLOCKED and debug the combined descriptor/root contract next.
- C8 sentinel BlitPass outputs are now explicitly `RGBA16Float`; mark-off direct endpoints remain a documented API BLOCKED case, not a GI failure.

### Runtime evidence delta (2026-08-12b)

- E2d confirms the failing boundary is explicit compose CB plus a global uniform
  (`artifacts/lumengi/C4/E2d-20260812b/mogwai.log`).
- Moving all atlas scalars into `LumenGDFComposeCB` fixes the production compose
  dispatch; both GDF levels complete at `(8,1,512)` in
  `artifacts/lumengi/C4/production-cbfix-20260812/mogwai.log`.
- C4 is now “compose dispatch fixed, Trace Router gate open”; C5 Hybrid and
  C10-C12 remain blocked by the missing end-to-end backend evidence.

### Runtime evidence delta (2026-08-30)

- [ ] S2 strict two-hour churn remains BLOCKED after v4 `MemoryError: bad allocation`
  (~422.6s). Material-only 1200s/72k-frame isolation passed; scene replacement
  now fences before/after release in `Source/Mogwai/Mogwai.cpp`. Re-run the
  authoritative launcher with unchanged cadence and thresholds; require a
  complete post-fix child artifact before changing the gate.
- [ ] C9 strict export equivalence remains OPEN/FAIL at the frozen mean/max
  bounds; `PASS_BOUNDED` same-frame retention is not a substitute.

### Runtime evidence delta (2026-08-30, checkpointed retry)

- [ ] S2 v8 dynamic phase passed, but the soak phase was safely stopped after
  approximately four minutes at <0.5 GB free system RAM; launcher artifact
  `artifacts/lumengi/release/soak-launch-20260830-postfix-v8/release-soak-gate.json`
  is `BLOCKED` with no soak child JSON. Rerun on a host with adequate memory;
  do not lower cadence or duration.

- [ ] C9 replay mark-set isolation reduced p99/max but v3/v4 mean error remains
  above the frozen `2e-5` limit; keep strict equivalence OPEN/FAIL and retain
  the v3/v4 artifacts for follow-up determinism work.
