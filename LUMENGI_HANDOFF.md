# LumenGI 交接单（2026-08-10 CodeGraph 复核版）

> 供后续对话直接接续。仓库：`F:\project\FalcorRendering`，分支 `codex/lumen-gi`。
> **当前权威执行计划：`docs/LumenGI_Production_Chain_Closure_Plan.md`。** 长期技术路线见 `docs/LumenGI_Technical_Roadmap.md`，历史阶段拆解见 `task.md`，历史进度见 `todo.md`。接手顺序：本文件 → 当前权威执行计划 → 按需查历史文档。
>
> 总目标不变：实现借鉴 UE5 Lumen 公开架构思想的实时动态漫反射 GI。当前重点不是继续堆独立组件，而是先关闭 Surface Cache、追踪后端、Screen Probe、重建滤波和最终 `diffuseGI` 之间的生产数据链。

## 0. 接手必读

- 分支 `codex/lumen-gi`，基线 `eb540f67`；最近提交见文末 git log。
- 最近一次已推送的源码提交是 `ce180fab`；本轮计划文档修改仍在工作区，接手时以 `git status --short` 为准。
- **AGENTS.md 规则**：代码分析先用 `codegraph_explore`；`.codegraph/` 不要提交。
- **不要并发跑多个 MSBuild**（C1041 PDB 冲突历史）；仓库级构建用 `--parallel 1`。
- **GPU 只有 1 块**（RTX 2060 SUPER）：GPU 测试串行。
- **新 shader 必须过 Mogwai 实机编译**（文件存在≠已编译）。

## 1. 当前阶段状态

2026-08-10 已执行 `codegraph sync .` 并复核当前运行时数据流。下表把“组件证据”和“生产主链集成”分开；旧 artifacts 和测试仍是有效的组件证据，但不能单独证明最终实时 GI 已闭环。

| 阶段 | 组件状态 | 生产主链状态 | 说明 |
|---|---|---|---|
| S0 工程骨架 | ✅ 完成 | ✅ 完成 | 证据 `artifacts/lumengi/S0/` |
| S1 HWRT 基线 | ✅ 完成 | ✅ 可作为基线/回退 | emissive NEE、MIS/数值防护；证据 `S1/`（`reference-compare/metrics2.json`） |
| S2 Cards/Capture | ✅ 组件 Gate 通过 | ❌ 未关闭 | Cards/Atlas/调度器/Capture、coverage/churn 证据保留；不能外推为 cache radiance 已被最终 GI 消费 |
| S3 Cache Lighting | ✅ 组件 Gate 通过 | 🔶 C1 部分执行，仍未关闭 | Arcade 隔离已确认 env importance sampler 变体触发 `E_INVALIDARG`；当前用 uniform-environment fallback 保证 all-on 可运行，真实 sampler 仍待修复 |
| S4 Screen Trace/Probe | ✅ 组件 Gate 通过 | 🔶 部分闭环 | HZB、screen trace、probe grid/integrate/interpolate 可运行；screen-probe history 与 `screenProbeStats` 已有生产绑定，radiance source/fallback 仍需独立 Gate |
| S5 Temporal/Spatial | ✅ 组件 Gate 通过 | ✅ C8/C9 host/runtime 已接线 | temporal/spatial、moments/variance 和 Final Resolve 已接入 `resolvedDiffuseGI`；完整 production composite 仍未声明 |
| S6 Mesh SDF/GDF | 🔶 组件可运行 | ❌ GDF compose 阻塞 | `useGDF=false` 的 MeshSDF/Hybrid 能回退 HWRT；`useGDF=true` 在 compose dispatch 仍报 `E_INVALIDARG`，真正 Hybrid 尚未成立 |
| S7 Radiance Cache | 🔶 CPU 组件完成 | ❌ GPU 未接 | `LumenRadianceCache.h` + 23 CPU 测试；`mUseRadianceCache` 目前仅解析/序列化/UI，没有 execute 主链消费 |
| S8 优化/质量档 | 🔶 参数表完成 | ❌ 仅部分接线 | `LumenQualityPreset.h` + 7 CPU 测试；当前主要只有 cache-lighting samples/texel 受 preset 控制，不能称四档完成 |
| S9 发布回归 | 🔶 历史核心回归已跑 | ❌ 未开始完整发布 Gate | analytic/dynamic/stability/s2verify/smoke + **110/110 CPU 单测**保留；主链修改后需重跑 image/dynamic/perf/soak/validation |

**CPU 单测 110/110**（11 套件：Stats/Sampling/SurfaceCache/Scheduler/HZB/GDF/Atlas/Cache/Scene/Quality/RadianceCache）。

## 2. 剩余待办：C0–C12 小批次

执行细节、输入输出和 Gate 以 `docs/LumenGI_Production_Chain_Closure_Plan.md` 为准。以下批次严格按编号推进；每批只解决一个清晰边界，先保存复现证据，再修改代码，再运行该批 Gate。C0–C9 完成前，不接 Radiance Cache 或全面性能优化。

| 批次 | 唯一范围 | 最小 Gate |
|---|---|---|
| C0 | 冻结 Arcade 两个失败；建立 raw/probe/temporal/spatial/resolved/final 与 PT indirect 的可信截图协议 | C0.1 已稳定复现并记录 dispatch telemetry；C0.2 640x360/front capture PASS，resolved/final 明确 SKIP |
| C1 | 修复 Arcade cache-lighting `E_INVALIDARG`（当前为 env sampler fallback） | 重新启用 environment importance sampler 后，env + analytic + emissive 同开且 D3D12/RT Validation 零 error |
| C2 | 修复 800x450 与非 8 倍数分辨率资源/dispatch | Cornell/Arcade 在 640x360、800x450、1280x720 无 crash、黑帧、NaN |
| C3 | 修复 `MeshSDF + useGDF=false` 黑帧 | trace mode × feature toggle 矩阵通过；缺失能力明确回退 HWRT |
| C4 | 前移 GDF，建立统一 Scene Trace Router/hit record | GDF/HWRT 在 Probe Integrate 前可见，滤波后不再被后端覆盖 |
| C5 | 实现真正 Hybrid 与 backend counters | 同帧存在 SDF/GDF 命中及 HWRT fallback 证据；GDF 不再只是 debug |
| C6 | 接入 Surface Cache radiance lookup、generation 与 fallback | Probe hit lighting 消费 cache；stale/evicted 页不可读；miss 不黑屏 |
| C7 | 修复跨帧 probe direction sampling 与插值输入 | 固定 seed 可复现，8→32→96 帧误差持续下降 |
| C8 | 内部化生产中间资源，移除 `markOutput()` 算法依赖 | debug/export on/off 的最终 `diffuseGI` 数值一致 |
| C9 | Final Resolve 接入最终 `diffuseGI` | Probe→Temporal→Spatial/Upscale→Resolve 生效；albedo 只乘一次 |
| C10 | Radiance Cache / Far Field GPU 接线 | 远场 query/refresh/fallback/预算影响最终 GI，且无泄漏或黑帧 |
| C11 | 完整 Quality Preset 与性能优化 | 四档参数单调、热切换稳定，画质和性能变化均有证据 |
| C12 | 图像、动态、validation、性能和 soak 发布矩阵 | 多场景三视角、三轮独占 GPU、30min/2h soak 同时通过 |

C0–C9 构成 S5.5 Production Chain Closure。旧 S2–S6 组件证据继续保留，但受调用顺序、资源生命周期和 Resolve 改动影响的 GPU/image Gate 必须重跑。

### 2026-08-10 执行记录

- C0.1: Arcade 640x360 与 800x450 均在首个 cache-lighting dispatch 复现 `E_INVALIDARG`。新 telemetry 为 `pages=30`, `groups=(30,1,1)`, `tg=(16,16,1)`，当前不是 dispatch 尺寸超限。
- C0.2: `tests/lumengi/run_chain_closure_capture.py` 已实际生成 raw/probe/history/temporal/moments/spatial/variance/resolved 的线性 HDR 与 FrameCapture EXR，并生成 PT direct/one-bounce/final 对照；`finalColor` 仍明确 SKIP，`resolvedDiffuseGI` 已有运行时输出。
- C1: 8-case isolation 证明 env-only 与 all-on 只在 environment importance sampler 开启时失败。暂时关闭 `kUseCacheLightingEnvImportanceSampler` 后 all-on 通过且数值有限；这只是安全降级，不是 C1 Gate 完成。
- C2: `artifacts/lumengi/C2/full-20260810c/resolution-matrix.json` 的 640x360、800x450、非 8 倍数和 1280x720 矩阵通过；`screenProbeStats` 报告了 resize 后的 resource dimension、probe count 与 fallback 统计。
- C3: HardwareRT 以及 `useGDF=false` 的 MeshSDF/Hybrid fallback 矩阵通过；`useGDF=true` 仍在 `runGDFCompose` 触发 `E_INVALIDARG`，因此 C4/C5 不得关闭。
- C7/C8/C9: history alpha 在 1/8/32/96 帧为 16/112/496/1520；temporal moments 与 filtered variance finite/non-negative；`resolvedDiffuseGI` 已进入 Final Resolve 并通过运行时 capture。方向 identity/union 尚未暴露（`run_probe_direction_union.py` 记录 `directionUnionGate=SKIP`）。展示图使用 `run_resolved_showcase.py` 的截图专用 composite，不能称 full-scene `finalColor`。
- 证据目录：`artifacts/lumengi/chain-closure/P0/`、`artifacts/lumengi/chain-closure/C1/`（运行时目录被忽略，不应提交）。

### 2026-08-10 当前收口判定

可进入下一步的阶段：C0、C2、C3 fallback、C7、C8、C9 host/runtime。仍需补证或修复：C1 environment importance sampler、C4/C5 GDF/Hybrid、C6 cache lookup on/off effect，以及 full-scene final composite。C10-C12 按计划继续延后，不得因组件文件存在而提前启动。

## 3. 关键技术坑（避免重踩）

- **D3D12 原生 mip 链是 floor-halving**；LumenGI 的 HZB 用 **ceil-halving**（保守），因此 HZB 是**每级独立纹理数组**（`gHZBMips[16]`），不能用 create2D 的 mipCount 参数。消费端 `hzbMipSize` 也是 ceil。
- **UAV 数组绑定必须 `setUav`**（Assignment 绑 SRV 描述符 → dispatch E_INVALIDARG）。
- **shader 的 Texture3D/Texture2D`<float>`（32 位）与 R16Float/R8Snorm 资源类型化视图不匹配** → dispatch E_INVALIDARG；host 侧统一 R32Float（或 shader 用 half/snorm 类型）。
- **未绑定 sampler/纹理全局参数进 root signature → dispatchE_INVALIDARG**：可选 sampler 必须 `#if HAS_*` 条件声明；`_EMISSIVE_LIGHT_SAMPLER_TYPE` 未绑定时 host 必须 `addDefine("_EMISSIVE_LIGHT_SAMPLER_TYPE","255")`（Null）pin，否则宏残留致重编译 dxc 崩溃。
- **类名是 `LumenGIPass`**（插件 id 仍是 "LumenGI"）。`LumenGI` 已被 `namespace LumenGI::MeshSDF`（MeshSDF 组件）占用，**不要加 `using LumenGI = LumenGIPass`**（重定义冲突）。测试引用用 `LumenGIPass::`。
- **生产链不能依赖 `markOutput()` 才工作**：Falcor 未 mark 的可选 graph 输出可能不分配，旧 showcase 曾因此全黑。调试/展示时仍可 mark `probeInterpolated`、`temporalFiltered`、`spatialFiltered`，但生产所需 history/intermediate 必须由 pass 内部保证分配；debug output 是否暴露不能改变最终 `diffuseGI`。
- **source_group(TREE) 限制**：FalcorTest 的 target_sources 只能列 `Tests/` 内 .cpp；外部 .cpp（如 LumenMeshSDF.cpp）会触发顶层 source_group 报错 → 用 header-only（把实现 inline 进 .h）或测试内 `#include` 外部 .cpp。
- **MeshSDF 组件命名空间**：`LumenGI::MeshSDF`（Z9/Z11/Z19 的 Cache/InstanceTable/Scene/RadianceCache），GDF 是 `LumenGI::GlobalDistanceField`。
- **shader 修改后需重建才复制到 bin/Release/shaders**（`target_copy_shaders` 在构建时复制）。
- **Mogwai python print 不进 --logfile**（进 stdout）；日志文件主要看 Falcor (Info)/(Warning)/(Fatal)。
- **agent 执行规律**：大任务（"集成"、多文件"修复失败测试"）多次空返回；**小任务**（单文件、明确接口）成功率较高。S7 组件前 4 次空返回，拆成单 header 后一次成功。后续派 agent 尽量拆小。
- **LightCollection 惰性构建**：`useEmissiveLights()` 需要 `getLightCollection` 先被调用（LumenGI execute 已处理 `mLightCollectionInitialized`）。

## 4. 测试命令（已核实）

```powershell
# 构建（单 MSBuild）
tools\.packman\cmake\bin\cmake.exe --build build\windows-vs2022 --config Release --target LumenGI FalcorTest Mogwai --parallel 1

# CPU 单测（110/110；不挂 --device-type）
build\windows-vs2022\bin\Release\FalcorTest.exe --test-suite "Lumen" --xml-report artifacts\lumengi\unit.xml

# GPU 脚本（headless）
build\windows-vs2022\bin\Release\Mogwai.exe --device-type d3d12 --headless --precise --script tests\lumengi\<脚本>.py --logfile artifacts\lumengi\<阶段>\<name>.log
```

## Runtime evidence delta (2026-08-11)

- C1 fallback is verified again on Arcade (`artifacts/lumengi/C1/fallback-verified-20260811.log`): all-on cache lighting is finite/non-negative, but the environment importance sampler remains disabled (`envSampler=0`). Re-enabling it still returns `E_INVALIDARG` in `artifacts/lumengi/C1/env-flat2-20260811.log`.
- C4 E0 kept all compose bindings and reduced the logical dispatch to `(1,1,1)`; `artifacts/lumengi/C4/E0-20260811.log` still returns `E_INVALIDARG`. Dispatch dimensions are not the root cause. Do not close C4/C5; next is no-op/single-UAV descriptor bisection.
- C4 follow-up source fix: both runtime MeshSDF atlas staging resources are now `R32Float` to match the shader's `Texture3D<float>` declarations, with raw float uploads. This has not yet been built/runtime-verified; the prior `gdf-r32` log has stale binary provenance. E1/E2 diagnostic shaders are present at `Source/RenderPasses/LumenGI/MeshSDF/LumenGDFComposeDiag*.cs.slang`, but host wiring remains root-owned.
- C6 runtime Gate is PASS in `artifacts/lumengi/C6/surfacecache-effect-20260811` (lookup on/off, reload reset, low budget).
- C8/C9 mark-on/export equivalence is PASS for both filter policies. The final matrix is `artifacts/lumengi/C8/export-equivalence-20260811d/export-equivalence.json`: mark-off cases are `BLOCKED` because direct `diffuseGI`/`resolvedDiffuseGI` endpoints are unavailable without graph marking. Sentinel arrays are diagnostics only; `finalColor` remains SKIP.
- Host GDF sphere-trace dispatch was corrected to pass logical frame dimensions; this has no runtime evidence yet because C4 compose remains blocked.
- Latest C4 R32Float rebuild still fails `runGDFCompose` (`artifacts/lumengi/C4/r32-verified-20260811.log`); the typed-view source fix is necessary but not sufficient. E1/E2 both compile in Mogwai (`artifacts/lumengi/C4/gdf-diag-compile-20260811d.json`), while that standalone compile harness has a shutdown-only DXGI_DEVICE_REMOVED after writing the PASS report. Latest C7 history/count report is `artifacts/lumengi/C7/probe-direction-union-20260811/probe-direction-union.json`; direction identity remains `SKIP`.

### Runtime evidence delta (2026-08-12)

- C4 E1 host diagnostic passes at logical `(1,1,1)`: `artifacts/lumengi/C4/E1-20260812/mogwai.log`.
- C4 E2 full descriptors and production compose still return `E_INVALIDARG` (`artifacts/lumengi/C4/E2-20260812/mogwai.log`, `artifacts/lumengi/C4/production-20260812/mogwai.log`). E2a CB+GDF buffers and E2b atlas descriptors each pass (`artifacts/lumengi/C4/E2a-20260812b/gdf-diagnostic.json`, `artifacts/lumengi/C4/E2b-20260812/gdf-diagnostic.json`), isolating the next investigation to the combined root/descriptor contract.
- C4/C5 remain blocked; no Hybrid router or C10-C12 work may be promoted. `run_export_equivalence.py` now keeps diagnostic sentinels in RGBA16F and does not reinterpret them as production GI.

### Runtime evidence delta (2026-08-12b)

- E2d (`CB + one global uniform + UAV`) still returns `E_INVALIDARG` at
  `(1,1,1)`: `artifacts/lumengi/C4/E2d-20260812b/mogwai.log`.
- Production compose now keeps the three atlas scalar constants inside
  `LumenGDFComposeCB`; the fresh Release plugin completes both levels at
  `(8,1,512)` with no fatal error: `artifacts/lumengi/C4/production-cbfix-20260812/mogwai.log`.
- Only the compose dispatch/root-CBV failure is closed. C4 Trace Router and C5
  real Hybrid backend selection/counters remain open; C10-C12 stay deferred.

### UE5.8 reference-plan handoff (2026-08-10)

Read [`docs/LumenGI_UE5.8_Reference_Optimization_Plan.md`](docs/LumenGI_UE5.8_Reference_Optimization_Plan.md)
before starting the next Wave. `F:\UE_5.8` has an initialized but interrupted
CodeGraph index (queryable, not a complete caller graph). UE source evidence
requires the Falcor chain to be organized as mark/feedback -> cache update ->
trace router -> hit lighting -> filter/integrate -> history -> resolve. The
next implementation node is C4 Trace Router: route Screen miss to GDF, then
HWRT fallback, and expose producer-frame/backend telemetry. Do not promote
standalone `gdfTrace`, `gdfStats.sphereHit`, a checkbox, or `markOutput()` to
production evidence. C5 Hybrid and C10-C12 remain dependency-blocked.

### UE5.8 function-level audit delta (2026-08-10c)

Before the C4 ABI Wave, fix three bounded Host defects under a single root
owner: Temporal currently can consume stale internal Probe interpolation when
Screen Probes did not produce this frame; Spatial can overwrite graph
`filteredVariance` with an unwritten internal texture; Geometry/Mesh changes do
not reliably invalidate MeshSDF/GDF. Then freeze separate geometry/radiance
validity, producer generation and route telemetry. C6 remains numerically wired
but is only a narrow effect Gate: the current shader scans every card per probe
direction, lacks complete page-generation/clear/feedback semantics, and must be
reopened after C4/C5. Full details and Gates are in the UE5.8 reference plan.

### I0 Host implementation and runtime evidence (2026-08-10)

- Implemented in `Source/RenderPasses/LumenGI/LumenGI.cpp`: Temporal now requires
  `mScreenProbes.producedThisFrame`; Spatial writes internal `pVariance` before
  copying the optional graph mirror; geometry, mesh, moved-instance, scene-graph,
  and recompile updates invalidate MeshSDF/GDF lazily.
- Release build passed with `/m:1` (0 warnings, 0 errors).
- Final GPU artifact: `artifacts/lumengi/I0/run-20260810-2213/`. Cornell 320x180
  loaded on D3D12, both GDF compose levels dispatched successfully, and no
  `E_INVALIDARG` occurred.
- `tests/lumengi/run_i0_validity_regression.py` passed `py_compile`. Its runtime
  report deliberately marks mutable property transition, independent internal
  variance equality, and generation/reset telemetry as `BLOCKED`; these are
  observability gaps, not inferred passes.

常用 GPU 脚本：
`run_smoke.py`（Cornell 冒烟）、`run_s2verify.py`（S2 回归）、`run_analytic.py`/`run_dynamic.py`/`run_stability.py`/`run_lightstep.py`（S1/S3 gate）、`run_cards_coverage.py`/`run_churn_short.py`（S2）、`run_screentrace.py`/`run_probe.py`/`run_probe_interp.py`（S4）、`run_temporal_verify.py`/`run_spatial_gate.py`/`run_temporal_ghost.py`（S5）、`run_s6_gdf.py`/`run_sdf_*.py`（S6，通道 S6_TODO 未冻结大多 SKIP）、`run_showcase.py`（效果展示，`artifacts/lumengi/showcase/*.png`）。

## 5. 历史效果图（仅诊断，不是当前质量 Gate）

- `artifacts/lumengi/showcase/cornell-gi.ToneMapperDisplay.dst.96.png` — 旧 Cornell 展示链（96 帧，tone-mapped）；当前链路未闭环，不能称“全功能最终 GI”
- `artifacts/lumengi/showcase/cornell-gi-effect.png` — 同图截图
- `artifacts/lumengi/showcase/cornell-gi.*.exr` — probeInterpolated/temporalFiltered/spatialFiltered 中间层
- `artifacts/lumengi/screenshots/panel-lumen-vs-pt.png` — S1 对比面板
- 历史 Cornell 中间层数值：spatialFiltered mean≈0.51/max≈8.8；emissive_glow mean≈73/max≈896。只用于回归定位，不代表最终合成正确或与 PT 公平可比。

## 6. 最近提交

```
ce180fab GI effect showcase + chain output allocation fix + Arcade cache-lighting edge case note
dc9eadbd S9 core regression evidence; stage board update
f7163a4e S7 radiance cache + S8 quality presets; LumenGIPass rename fallout fix
6d5d4a71 Complete S6 GDF compose/sphere-trace wiring; fix dispatch E_INVALIDARG
d9850b48 Fix GDF compose dispatch E_INVALIDARG; gate S6 behind useGDF + resource guard
3821d232 S5-B2 spatial filter integration; close S5 gate; S6 MeshSDF scene pipeline
1b434aae S5 temporal/spatial integration; S6 runtime cache + instance table
7dd825a7 S4.3 probe integrate/interpolate integration; S5 temporal filter shader
96dfaa7f S4.2 screen probe grid
0620d28b S4-A1 integration + S6B GDF shaders
444c74c5 S3-B2 multi-bounce feedback; Atlas CPU test fixes; S3 reference assets
64f7465e GDF clipmap component; HZB conservativeness fix; S4 test assets
fc6ca5ba S3 cache lighting integration; RT program recompile crash fix
25c0ac66 render screenshot script
e4145ca6 Close S2 gate; CardsOverlay; cardCoverage; churn telemetry
53f145eb S2 host integration + emissive NEE
02ffdd3a Close S1 gate; freeze thresholds; NEE diagnostics
f397b56c Add LumenGI baseline + S2 components
f8783caf Add offline Mesh SDF builder tool
```

## 7. I1 visual-noise Wave (2026-08-10)

- `ScreenProbeTrace.cs.slang` now converts screen-hit `t` from trace-view space to
  world hit distance using `hitUV + linearZ` before the 32B record is consumed by
  Surface Cache lookup.
- `ScreenProbeInterpolate.cs.slang` skips non-finite/zero-confidence records when
  normalizing weights and nearest fallback; RGB=0 remains a legal radiance value.
- Trace/integrate/temporal/spatial/resolve radiance ceiling is 10 and temporal
  history clamp is enabled by default, following UE5.8's max-ray-intensity and
  history-clamp direction.
- Release build passed with `/m:1`. Latest visual preview:
  `artifacts/lumengi/screenshots/current-final-20260810e/`; Arcade is visibly less
  firefly-noisy. Cornell still has residual probe/history structure and the
  preview is not a full-scene `finalColor` gate.
- `run_resolved_showcase.py` accepts `LUMEN_RESOLVED_USE_SURFACE_CACHE`,
  `LUMEN_RESOLVED_USE_CACHE_LIGHTING`, and `LUMEN_RESOLVED_SETTLE_FRAMES` for
  reproducible A/B runs. Cache on/off did not materially change the remaining
  linear noise, so the next implementation Wave is explicit producer generation,
  depth/normal/motion history rejection, confidence separate from history length,
  and frame 1/8/32/96 variance/p99 evidence.

## 8. I1.2 UE-style temporal/spatial denoise closure (2026-08-10)

- Temporal history now stores a previous packed normal (RGBA16F blit) and uses normal/material
  validation when the GBuffer normal is available.
- The showcase marks `temporalConfidence` separately from `temporalFiltered.a` history length.
- Spatial reconstruction runs three small bilateral ping-pong passes; the temporal AABB clamp skips
  non-finite or zero-confidence current probes so invalid samples cannot pull history to black.
- Release `/m:1` passed with 0 warnings/0 errors. Final D3D12 artifact:
  `artifacts/lumengi/screenshots/current-final-20260810l/` (Arcade three views and Cornell).
- Showcase defaults to the low-noise Lumen indirect gate. `LUMEN_RESOLVED_USE_DIRECT_LIGHTING=1`
  enables RTXDIPass for direct+indirect composition, but its stochastic direct reservoir remains a
  separate quality gate and is not accepted as a no-noise Lumen result.

This closes the bounded I1 visual implementation only. Producer generation/age, reprojected
moments, GDF probe-route telemetry, and separately denoised direct+indirect final color remain open.

Serial Arcade-front convergence evidence is in
`artifacts/lumengi/screenshots/convergence-arcade-front-{1,8,32,96}/`: frame 1 has expected sparse
probe outliers, frame 8 is reduced, and frames 32/96 are visually stable. It is a static-scene Gate,
not proof of global HWRT seed determinism or moving-camera/light/geometry recovery.

## 9. Full-scene low-noise composition (2026-08-11)

The showcase now has a complete low-noise composition path. RTXDI direct diffuse/specular
illumination (RGB plus hit distance in alpha) is denoised by NRD ReLAX Diffuse+Specular using
GBuffer linear depth, packed normal/roughness/material, and world-space motion. ModulateIllumination
restores reflectance and emissive radiance, then the result is added to Lumen indirect GI.

The default screenshot quality mode also denoises `LumenGI.diffuseRadianceHitDist` through NRD
RelaxDiffuse before material modulation. Use `LUMEN_RESOLVED_USE_INDIRECT_DENOISING=0` to inspect
the screen-probe resolved source directly. This improves screenshot stability but does not close
the screen-probe C7/C8 production Gate; generation/age, reprojected moments, and GDF route
telemetry remain required.

Evidence directories:

- `artifacts/lumengi/screenshots/direct-nrd-smoke-20260810/`
- `artifacts/lumengi/screenshots/indirect-nrd-experiment-20260811/`
- `artifacts/lumengi/screenshots/indirect-nrd-arcade-20260811/`
- `artifacts/lumengi/screenshots/direct-nrd-convergence-20260811/`
- `artifacts/lumengi/screenshots/final-low-noise-20260811/` (96-frame six-view run and
  `contact-sheet.png`)

## 10. Latest probe evidence and visual ledger (2026-08-11)

The five-pass spatial ping-pong alias fix is built and runtime-compiled. The honest ScreenProbe
diagnostic is `artifacts/lumengi/screenshots/probe-confidence-gate-20260811/` with
`LUMEN_RESOLVED_USE_INDIRECT_DENOISING=0`; outputs are finite/non-negative and logs contain no
`E_INVALIDARG`/fatal/D3D12 error, but low-frequency wall mottle remains. This is still a C7/C8
visual Gate failure. The shader-only validity fixes are:

- Spatial skips non-finite/zero-confidence neighbors in variance and bilateral accumulation.
- FinalResolve no longer treats RGB length near zero as invalid; finite alpha/confidence controls
  fallback, preserving valid black radiance.

The latest six-view presentation files are in
`artifacts/lumengi/screenshots/final-low-noise-postfix-20260811/`. The quality branch uses raw
HWRT-compatible `diffuseRadianceHitDist -> NRD -> Modulate`; it is suitable for visual review but
must not be reported as proof that Probe→Temporal→Spatial→FinalResolve is noise-free.

All screenshots, EXRs, logs, visual comparisons, and Gate decisions are indexed in
`docs/LumenGI_Visual_Debug_Log.md`. Future work must append unique artifact directories and record
the exact environment, build, resolution, settle frames, view, and denoiser branch.

Latest diagnostic additions:

- `artifacts/lumengi/screenshots/probe-spatial-wide-20260811/` tested a wider spatial radius
  (`min=2`, `max=4`, variance-high `0.10`); the mottle was nearly unchanged, so radius alone is
  not the root cause.
- `artifacts/lumengi/screenshots/probe-history-20260811/` marks `probeHistory`; its RGB EXR is
  finite, but the current EXR readback drops alpha, so accumulated direction count is not yet a
  valid Gate.
- The visual process ledger is `docs/LumenGI_Visual_Debug_Log.md`; every new run must append a
  unique artifact directory and label production-chain versus quality-branch evidence.

## 11. Runtime evidence supersession (2026-08-11)

The older C1 fallback-only entries above are historical and are superseded by the post-fix runs:
`artifacts/lumengi/C1/emissive-off-env-sampler-parameterblock-20260811/` and
`artifacts/lumengi/C1/all-on-env-sampler-parameterblock-20260811/` both use
`ParameterBlock<EnvMapSampler>`, dispatch with `envSampler=1`, and pass eight 640x360 Arcade frames
with finite/non-negative GI and no `E_INVALIDARG`, `Fatal`, or 39019 root-shape warning. Keep the
uniform-environment run as a control and add 800x450/multi-view/env-map-change coverage before
calling the release matrix complete.

The C6 page-generation/state fence is runtime-verified by
`artifacts/lumengi/C6/page-metadata-20260811-v2/`: lookup on/off, reload, and one-page-per-frame
budget all pass finite/non-negative Cornell 640x360 output. Demand feedback, deduplication,
last-used/eviction counters and moving-card stale-owner coverage remain open.

## 12. C4 compose/runtime evidence (2026-08-11)

The post-C1 Release build (`cmake --build build/windows-vs2022 --config Release --target LumenGI Mogwai --parallel 1`) was used for the C4 matrix. Production GDF compose dispatched both dirty levels at Cornell 320x180 with no fatal/D3D12 error in `artifacts/lumengi/C4/post-build-production-20260811/`. The descriptor bisect passes E1, E2a, and E2b; E2/E2c/E2d remain retained `E_INVALIDARG` controls for the nested-CB plus implicit-global-CB root-shape failure.

Standalone MeshSDF/Hybrid trace health is recorded in `artifacts/lumengi/C4/standalone-s6-corrected-20260811/`: finite trace, `sphereHitRate=0.2566`, and no NaN. Both modes still report `hwrtPrimary=1`, `gdfRadianceSelected=0`, and `hybridFallbackToHWRT=1`; the corrected script explicitly marks `sdfPrimaryWritesOutputs=false`. Therefore C4 is compose-dispatch PASS but production Screen -> GDF -> HWRT routing remains BLOCKED; do not use the standalone trace or `gdfStats.sphereHit` as probe-route evidence.

## 13. C4 probe router supersession (2026-08-11)

The previous C4 “router BLOCKED” statement is superseded by the unified probe-route wave. The
ScreenProbe trace now performs Screen miss -> GDF sphere trace -> HWRT fallback, writes
`kLumenProbeHitFlagGDFHit`, and exposes `gdfHits/gdfMisses` in the 40-byte probe counter ABI.

- `artifacts/lumengi/C4/gdf-probe-router-v4-20260811/`: Cornell 320x180, MeshSDF/useGDF/screen
  probes, four frames; `gdfRouteEnabled=true`, `gdfHits=607`, `gdfMisses=12260`, finite/
  non-negative outputs, `PASS`.
- `artifacts/lumengi/C4/gdf-probe-router-baseline-20260811/`: HardwareRT/useGDF=false control;
  `gdfRouteEnabled=false`, `gdfHits=0`, finite/non-negative, `PASS`.

C4 geometry routing is now runtime-verified. Hit lighting remains partial: GDF hits reuse the
existing screen/environment fallback radiance resolver, not a dedicated UE Surface Cache/card
lighting producer. C5 Hybrid policy, backend sidecar identity, and GDF material-lighting quality
remain open. Historical compose-only and standalone S6 “router blocked” entries remain useful
negative evidence but must not override these newer artifacts.
## 14. C6 page lifecycle telemetry closure (2026-08-11)

The C6 runner now requires the host page-lifecycle contract and reports disabled-cache controls as
`NOT_APPLICABLE`. The Release GPU artifact
`artifacts/lumengi/C6/page-telemetry-20260811-v5/surfacecache-effect.json` is complete for
Cornell 640x360: lookup-on, lookup-off, reload, and one-page-per-frame low budget all pass with
finite/non-negative GI. Host stats include page generation/state counts, generation and stale-owner
rejects, scheduler request deduplication, page-cache touch (`lastUsed`), and evictions. This is a
static-scene lifecycle/telemetry closure; moving-card demand feedback and C5 quality remain open.

## 15. C7 producer-validity sidecar (2026-08-11)

The optional `LumenGI.probeValidity` raw-buffer output now has a frozen `uint4`-per-direction
layout: backend/source bits, producer frame, history generation, and probe age. The host clears the
buffer before diagnostic dispatch and the test decoder reinterprets Falcor's byte-address readback
as little-endian `uint32` words.

The current steady artifact is
`artifacts/lumengi/C7/probe-validity-sidecar-20260811-v9/`. It proves raw-buffer allocation,
binding, dispatch, and finite readback at Cornell 800x450 frames 1/8/32/96 with no D3D12 fatal or
`E_INVALIDARG`. Evenly distributed sampling reports Screen/HWRT/Invalid backend counts consistent
with the host screen-hit and fallback counters. This is a producer ABI/backend-distribution PASS,
not transition proof.

`tests/lumengi/run_probe_validity_gate.py` and
`tests/lumengi/run_probe_validity_transitions.py` intentionally report `BLOCKED` until they observe
generation change, age reset, and reset reason from the raw sidecar. The transition harness timed
out before producing a manifest, so no C7/A1 reset claim is made. The next bounded experiment should
use a lightweight first-frame/first-post-transition capture and inspect the active shader variant
and raw UAV writes before attempting the full 1/8/32/96 matrix. Previous v3-v5 sidecar artifacts
used an incorrect byte decoder and are historical negative evidence only. The reduced camera-cut
artifact `artifacts/lumengi/C7/probe-validity-transitions-20260811-camera-v1/` timed out during
Mogwai initialization and produced no manifest; reset/generation/age remains BLOCKED.

The formal matrix
`artifacts/lumengi/C7/probe-validity-transitions-20260811-full-v1/probe-validity-transitions.json`
supersedes that early timeout: 800x450 and 641x361 (partial-tile), scene reload and camera cut,
all pass continuous checkpoints 1/8/32/96 with readable sidecar, generation change, age reset, and
reset reason. A1 producer identity/reset observability is closed; A2 normal/material/moments and
image-quality convergence remain open.

## 16. C8/C9 export-equivalence runtime (2026-08-11)

The current Release binary was rerun at Cornell 800x450 with filters off and partial temporal-only
policies. `artifacts/lumengi/C8/export-equivalence-20260811-current/export-equivalence.json` is
`PARTIAL`: mark-on/export-off and mark-on/export-on both pass for `diffuseGI` and
`resolvedDiffuseGI`, with linear-HDR equivalence, finite/non-negative output, and no D3D12 fatal.
Mark-off policies are intentionally `BLOCKED` because unmarked RenderGraph endpoints are not a
production readback contract; sentinel BlitPass values are diagnostic only.

This closes the tested marked-endpoint export contract, not the full C8/C9 source-validity contract.
The next C8 wave must keep internal temporal confidence separate from history length, propagate
producer generation/reset stamps into FinalResolve, and add a GPU albedo-once/valid-black regression.

## 17. C7/A2 guide-history runtime (2026-08-11)

The full-resolution screen-radiance history now has a normal/material guide ping-pong resource.
History reprojection requires depth agreement, a 45-degree normal agreement, and exact material-id
agreement. The RGBA16F history alpha remains secondary hit distance; it is not reused as age or
confidence.

Artifact: [`A2 guide-history convergence`](F:/project/FalcorRendering/artifacts/lumengi/A2/screenradiance-guide-20260811-v1/screenprobe-convergence-manifest.json).
The Release D3D12 run passes Cornell 800x450 at continuous checkpoints 1/8/32/96, with finite and
 non-negative probe/temporal/spatial/resolve outputs and no fatal, `E_INVALIDARG`, missing-member,
 or shader-link errors. This closes the guide-history component/runtime Gate only. Source moments,
 lighting-generation rejection, and the no-noise image-quality Gate remain open; the source-moments
 component is recorded below and now needs paired image/performance measurements.

The source-moments substep is also runtime-verified in
`artifacts/lumengi/A2/screenradiance-moments-20260811-v2/`: Cornell 800x450, continuous 1/8/32/96,
four captures, zero capture errors, and no fatal, `E_INVALIDARG`, missing-member, or shader-link
messages. Moments store luminance mean/mean-square in a separate RG32F ping-pong pair and only
adjust source-history weighting; they do not replace hit-distance alpha or downstream irradiance
moments. Image variance reduction and GPU-cost comparison remain open.

The post-A2 C8 regression artifact
`artifacts/lumengi/C8/export-equivalence-20260811-post-a2/export-equivalence.json` keeps the
marked export matrix passing for filters off and partial. Mark-off remains `BLOCKED` by the
RenderGraph endpoint contract, with no new fatal/D3D12 error. This confirms source moments did not
change the marked export behavior; it does not close the no-noise or final source-precedence Gate.

The static A2 image-quality comparison is now closed by
`artifacts/lumengi/A2/screenradiance-moments-compare-20260811-v2/moments-comparison.json`.
Using identical Cornell 800x450 runs, moments+2.5σ clamp lowers frame96 spatial local variance
by 1.63% and the 32→96 tail RMSE by 3.56%; the comparison script returns `NO_IMPROVEMENT` instead
of PASS when the effect is absent. Remaining C7 work is dynamic lighting/environment/material
generation rejection, multi-scene quality, and GPU-time budget.

## 18. C7 dynamic lighting-generation fence (2026-08-11)

Screen-radiance history now has a separate R32Uint ping-pong epoch texture. The RGBA16F radiance
alpha remains secondary hit distance. Previous history is accepted only when its stored lighting
epoch equals the current `mLightingGeneration`; dynamic light, emissive-material, and environment
changes reject stale raw radiance without conflating hit distance and history age. The optional
`screenRadianceLightingGeneration` output mirrors the producer epoch for diagnostics.

Release build and serial GPU artifact:
`artifacts/lumengi/C7/lighting-generation-20260811-v2/probe-validity-transitions.json`.
At 800x450 on RTX 2060 SUPER, light/material/environment transitions all PASS with changed
`lightingGeneration`, readable generation mirror, and mirror/stat agreement; no fatal,
`E_INVALIDARG`, or shader-link errors. Remaining C7 work is multi-scene/multi-angle image quality,
GPU timing/VRAM/soak, and explicit rough-specular/transmission producers.

## 19. C5 GDF probe cache-consumer evidence (2026-08-12)

The unified probe counter now distinguishes GDF geometry hits from Surface Cache lighting hits.
The cache-off control passes
(`artifacts/lumengi/C5/gdf-probe-router-cache-off-20260811/`) with finite/non-negative outputs
and `gdfHits=608`. The cache-on Cornell run is deliberately `BLOCKED`: `gdfHits=607` but
`cacheLookupHits=0` despite cache-lighting dispatches. The router runs, but authoritative cache
radiance consumption is not yet proven. Next action is reject-reason telemetry and correction of
the first failing GDF-hit/page-coverage contract.

The reject telemetry points first to card plane/extent coverage (`56091` rejects) and then card
metadata (`488`), with no page generation/state rejects. Arcade records 22 cache hits but no GDF
hits, so it is not a combined-route PASS. The next C5 implementation is therefore a hit-position
and card-candidate mapping fix, followed by a combined GDF+cache gate.

The combined gate now passes on `material_test` at 320x180 for four frames
(`artifacts/lumengi/C5/gdf-probe-router-material-cache-on-20260812/`): `gdfHits=42` and
`cacheLookupHits=1`, finite/non-negative outputs, and no runtime errors. Cornell remains a
coverage/metadata negative case; keep that limitation explicit until a multi-scene/higher-
resolution matrix is run.

## 20. Post-epoch Release status (2026-08-12)

The older C1 fallback-only rows above are historical. The current
`ParameterBlock<EnvMapSampler>` variant is the tested production shape; the
post-fix emissive-off and all-on artifacts run with `envSampler=1` and no
Fatal/E_INVALIDARG/39019 errors.

C6 reload evidence is now explicit rather than inferred from local page IDs:
the Host exports monotonic `surfaceCacheSceneGeneration` and
`surfaceCacheResetCount`, while page-local generations still reset safely on
scene reload. The full material 320x180 C6 matrix is PASS at
`artifacts/lumengi/C6/surfacecache-effect-material-320x180-postepoch-20260812/`.

A1 post-build validity transitions are PASS for 800x450 and 641x361, covering
scene reload and camera cut at 1/8/32/96 sequential checkpoints:
`artifacts/lumengi/A1/probe-validity-transitions-postepoch-20260812/`.

The current full ScreenProbe/Temporal/Spatial/Surface Cache benchmark at
Cornell 1280x720 (120 warmup + 600 capture) reports whole-frame GPU
`p95/p99/max=10.179/10.437/10.609 ms` and LumenGI
`9.533/9.618/9.951 ms`:
`artifacts/lumengi/benchmark/release-postepoch-cornell-1280x720-20260812/`.
Repeated distributions, GPU-wide VRAM, non-zero request/last-used/eviction
coverage, long soak, and rough-specular/transmission remain open release work;
do not promote C10-C12 yet.

### C6.1 resident-page feedback (2026-08-12)

The post-build C6.1 path is now runtime-verified at
`artifacts/lumengi/C6/feedback-postbuild-320x180-20260812-v3`.
ScreenProbe cache hits atomically record page hit counts and generations; the
host validates scene/page state and generation before touching the page LRU
before the next scheduler pass. `lookup_on`, `invalidate`, and `low_budget`
report non-zero feedback hits/pages/dedup; reload reports a stale-feedback
reject, and all outputs are finite/non-negative with no runtime validation
errors. This is a resident-page hit/touch checkpoint, not full UE demand
feedback: unmapped-card request buffering, request prioritization,
capture-completion/next-frame validity, eviction pressure and soak are C6.2
open work. Do not promote C10-C12 or claim rough-specular/transmission from
this checkpoint.

### C6.2 bounded request/capture smoke (2026-08-12)

The current Release binary now exercises the miss-to-request path. Probe
integration emits per-card request counts/reason bits for rejected page,
metadata, or visibility lookups; the host consumes the previous-frame request
buffer before the capture scheduler, validates scene/card identity, deduplicates
through the scheduler worklist, and reports capture completion separately.

Artifact:
`artifacts/lumengi/C6/request-postbuild-320x180-20260812-v5`

The Cornell 320x180 matrix is PASS for `lookup_on`, `lookup_off`, `invalidate`,
and `low_budget`. `lookup_on` reports request raw/cards/dedup/completed
`5510/111/5399/111` and feedback hits/pages `28/14`; `invalidate` reports
`8053/166/7887/166`; `low_budget` reports `5538/22/5516/14`, demonstrating
budget deferral without black substitution. Logs contain no fatal,
E_INVALIDARG, validation, or device-removed errors.

This is a bounded C6.2 smoke only. UE-complete priority, explicit next-frame
validity, tiny-atlas eviction/stale-owner reuse, moving-card demand, request
reason telemetry, repeated distributions, GPU-wide VRAM, and long soak remain
open. C10-C12 and production rough-specular/transmission claims stay deferred.

### A2 post-C6 convergence checkpoint (2026-08-12)

The latest Release binary passed the continuous Cornell/front 800x450
screen-probe history harness through frames 1/8/32/96:
`artifacts/lumengi/screenprobe-convergence/post-c62-a2-20260812/`.
All required channels are finite/non-negative with alpha retained, and the log
contains no Fatal/E_INVALIDARG/validation/device-removed matches. Resolved-GI
MAE is `0.00770`, `0.00368`, `0.00213` across 1→8, 8→32, 32→96; spatial
filtered MAE is `0.05297`, `0.02173`, `0.01117`.

This is a bounded history/convergence smoke, not a no-noise PASS. Source
mottle, dynamic guide/generation rejection, reprojected moments, repeated
performance distributions, and the explicit UE rough-specular/transmission
producers remain open.

### A2 raw-radiance age sidecar (2026-08-12)

The raw screen-radiance history now has a separate R32Uint ping-pong age
texture; RGBA16F alpha remains secondary hit distance. Release 800x450 Cornell
runtime evidence at
`artifacts/lumengi/screenprobe-convergence/post-age-a2-20260812/` passes
1/8/32/96. Age max/mean is `1/0.313`, `7/2.770`, `31/13.011`, `95/40.398`;
all channels are finite/non-negative and the log has no fatal/E_INVALIDARG/
validation/device-removed matches. Dynamic reject reasons, no-noise quality,
performance repeats/VRAM/soak, and rough-specular/transmission producers remain
open, so this is not a full completion claim.

### Final Release rebuild checkpoint (2026-08-12)

The final serial Release build (`LumenGI` + `Mogwai`, `/m:1`) passed. A
post-build C6.2 smoke at 320x180 Cornell passed `lookup_on`, `invalidate`, and
`low_budget` with request raw/cards/dedup/completed values of
`5510/111/5399/111`, `8053/166/7887/166`, and `5538/22/5516/14`.
Artifact: `artifacts/lumengi/C6/request-finalbuild-320x180-20260812-v6`.
No runtime fatal, E_INVALIDARG, validation, device-removed, traceback,
missing-member, or shader error-30015 was found. This confirms the current
binary and C6.2 bounded smoke; it does not close the already documented
UE-complete eviction/soak, no-noise, rough-specular, transmission, C10, C11,
or C12 gates.

### C7 direction identity sidecar (2026-08-12)

The fixed-size `probeValidity` record now exposes sample slot and an
octahedral direction fingerprint in reserved bits. The v2 transition decoder
passed a final-build 800x450 camera-cut run through checkpoints 1/8/32/96;
the cross-checkpoint union contains 161,204 probe/fingerprint identities:
`artifacts/lumengi/A1/direction-v2-finalbuild-800x450-20260812-v2/`.
Runtime logs are clean of fatal, E_INVALIDARG, validation, device-removed,
missing-member, and error-30015 matches. Exact direction reconstruction and
noise/variance quality are intentionally still separate gates.

### Current execution delta (2026-08-13)

The C9 full-color endpoint is now runtime-verified on Cornell/front at 800x450
with direct RTXDI/NRD plus `LumenGI.resolvedDiffuseGI`. Mark-on and mark-off
executions both record the producer pair, finite/nonnegative output, and
mark-off/export equivalence; the contract accepts only mean/max agreement
within `1e-4` when independent-process low-bit hashes differ. Artifact roots:
`artifacts/lumengi/C9/runtime-v3-markon-20260813/` and
`artifacts/lumengi/C9/runtime-v4-markoff-20260813/`.

C10 now has a scene-ray producer, eight-ray bounded analytic/emissive NEE,
ping-pong probe validity buffers, and a full-resolution
`radianceCacheValidity` R32Uint output (hit/sky/radiance/producer bits).
`artifacts/lumengi/C10/emissive-nee-smoke-20260813.json` compiles and dispatches
with no Fatal/E_INVALIDARG and records nonzero hit radiance. C10 is still
**PARTIAL**: persistent GPU indirection/allocator commit, stale-write/readback
counters, non-black fallback, multi-view/dynamic quality, and final-resolve
consumption remain open. Do not promote it to production GI.

The short-frame stats follow-up at
`artifacts/lumengi/C10/emissive-nee-stats-20260813` records the eight-ray
budget (`probeRayCount=1024` for 128 allocated probes), producer/interpolator
enabled, and `readyNextFrame=1`. Its legacy S7 trajectory still leaves GPU
VRAM/scroll/dynamic-light verdicts as `SKIP`; it is runtime evidence only, not
the strict C10 v1 production gate.

The A2 source-quality off-run was retried with a five-minute timeout but still
produced no manifest after plugin startup; this remains missing GPU evidence,
not a quality PASS/FAIL. Raw ScreenProbe mottle and C5 card-coverage rejects
remain the dominant visual-quality blockers. No shutdown is authorized until
those quality, VRAM/soak, rough-specular, transmission, C11 and C12 gates close.

The post-fix C5 grid smoke at
`artifacts/lumengi/C5/grid-fallback-gdf-true-20260813/gdf-probe-router.json`
kept `cacheLookupHits=2` at `4715` attempts, matching the same-scene full-scan
control while preserving finite outputs and GDF fallback. This validates the
new grid-to-full-scan correctness fallback only; high coverage rejects and
the all-card/tiny-atlas/eviction gates remain open.

The strict C5 equivalence report
`artifacts/lumengi/C5/grid-equivalence-telemetry-20260814.json` passes
same-scene full-scan vs grid-on correctness, including equal `2/4715` cache
hits and `12/12` card indexing. Coverage reject rate remains OPEN and is still
the dominant source-quality blocker.

The historical pre-wiring strict C10 producer report
`artifacts/lumengi/C10/producer-gate3-20260814.json` passes schema, counters,
next-frame readiness, validity, and hit-distance checks, but is PARTIAL because
the cache was not connected to `resolvedDiffuseGI` in that run. The later
`resolve-wiring-producer-gate-20260814.json` supersedes only this connectivity
field; lifecycle and quality gates remain open.

## 2026-08-14 execution delta: cache fallback wiring and rough-spec boundary

The C9 resolve shader now has an explicit optional Radiance Cache fallback
contract. A cache sample is usable only when producer/radiance validity bits,
finite RGB, and positive confidence are present; otherwise the existing
spatial/temporal/probe/HWRT precedence is unchanged. The host exports
`finalResolveConnected` only after those resources are bound for the current
frame. This source change has a clean Release build, but a fresh Mogwai run is
still required to prove the runtime flag and fallback image.

The E1 rough-specular wave is a standalone, disabled-by-default directional
producer with an offline contract gate. It does not alias `diffuseGI` or RTXDI
direct specular. Host resource/history binding, directional probe data, and
the UE roughness/cone temporal gate remain required before production enablement.
Offline contract artifact: `artifacts/lumengi/E1/rough-specular-gate/rough-specular-gate.json`.

The follow-up smoke
`artifacts/lumengi/C10/resolve-wiring-producer-gate-20260814.json` now proves
the optional cache fallback is runtime-bound: schema, producer counters,
frame-N to frame-N+1 readiness, validity/hit-distance contracts, and
`finalResolveConnected=true` all pass. The parent trajectory is still PARTIAL
because VRAM/scroll/dynamic-light checks are skipped and persistent UE-style
allocator, eviction, stale-owner, multi-view quality, and soak gates remain
open.

The post-completion C6 smoke
`artifacts/lumengi/C6/postcompletion-smoke-20260814/surfacecache-effect.json`
also runs clean at 320x180: lookup-on/off outputs are finite/nonnegative,
page-clear and metadata fields are readable, request/capture completion is
consistent, and validated feedback touches are observed. It is bounded static
evidence only; tiny-atlas eviction, stale-owner reuse, dirty-card mutation, and
next-frame-valid timing remain separate gates.

The tiny dirty-pressure run
`artifacts/lumengi/C6/tiny-dirty-pressure-20260814/surfacecache-effect.json`
applied five material mutation batches and observed next-frame metadata
pending/ready transitions with 12 pressure partitions and distinct page IDs.
It is intentionally **BLOCKED** for the strict lifecycle gate: no eviction or
generation reuse occurred, card identity for mutation targets is incomplete,
and stale-owner/texel evidence is therefore not promotable.

The stricter one-page rerun
`artifacts/lumengi/C6/tiny-onepage-dirty-20260814/surfacecache-effect.json`
still produced no eviction/generation-reuse event despite applied mutations.
This confirms the remaining issue is distinct page-owner demand in the capture
scheduler, not atlas capacity or a weakened test threshold.

The earlier supported `sphere_array.pyscene` pressure artifact remains the
authoritative bounded C6.2 result:
`artifacts/lumengi/C6/sphere-array-pressure-20260813-v4/`. It ran 144 frames
with a 64px atlas, observed 13 evictions, generation 1→2 reuse, seven
generation/stale-owner rejects, page-clear completion, and finite outputs.
Miss-request priority and long-soak are still separate release gates.

## 2026-08-14 current visual / A2 status

The clean presentation reference remains
`artifacts/lumengi/screenshots/final-realtime-lumengi-20260811/cornell-front-resolved.ToneMapperDisplay.dst.96.png`:
the room has stable red/green bounce, a readable box shadow, and no obvious
black-hole or overexposure artifact. The probe-only diagnostic
`artifacts/lumengi/A2/reject-telemetry-20260813/screenprobe-cornell-front.ToneMapperDisplay.dst.96.png`
still has blocky low-frequency wall mottle. This is a producer/coverage quality
signal, not evidence that the final composite is missing all GI.

The 2026-08-14 A2 source-quality retry timed out before writing a manifest, so
the no-noise gate remains BLOCKED. The next run must produce paired raw,
history, spatial, resolved and full-color artifacts at 1/8/32/96 plus dynamic
light/material/geometry transitions; do not close A2 from a smooth PNG alone.
The follow-up 320x180, 1/2/4-frame retry also timed out before its first
manifest; the spawned Mogwai process was terminated after verification. This
is an infrastructure/runtime-evidence blocker, not a quality PASS or FAIL.

The ScreenRadianceHistory update now uses a UE-style bounded accumulation
weight (10-frame cap, 0.10 minimum new-sample weight) instead of a fixed 0.15
EMA. Release shader compilation and a 96-frame Cornell run are clean; the
result is captured at
`artifacts/lumengi/screenshots/history-adaptive-20260814/`. The image is
visibly less blocky than the prior 96-frame diagnostic, but the source-quality
Gate remains open until raw/probe/history A/B manifests and cache-coverage
telemetry are captured together.

The adaptive-history production performance smoke at
`artifacts/lumengi/benchmark/adaptive-history-production-1280x720-20260814/`
completed 60 captured frames after 60 warmup frames. On the RTX 2060 SUPER,
LumenGI GPU p95/p99/max were 9.665/9.692/9.762 ms and whole-frame GPU
p95/p99/max were 10.275/10.363/10.646 ms. This is a short regression smoke,
not a replacement for the existing 600-frame repeats and long soak gate.

The C6 request readback now exports reason-bit counters for unmapped,
stale-owner, metadata-invalid, and visibility-invalid misses. The short smoke
`artifacts/lumengi/C6/reason-telemetry-smoke-20260814/` is runtime-clean and
shows stale-owner reasons; priority ordering and long soak remain open.

After the producer-record validity guard, C10 requires the normal ping-pong
settle window: a three-frame warmup produced valid hit/sky sidecar coverage and
passed the strict gate at
`artifacts/lumengi/C10/producer-compat-long-gate-20260814.json`. A one-frame
warmup is intentionally insufficient and reports no ready samples; this is a
timing constraint, not hidden evidence.

The C10 stats binding also separates `queryAttempts` from `queryHits`:
interpolation dispatch count is now reported without claiming every pixel was a
valid cache hit. A real `queryHits` value remains pending a GPU validity atomic
or readback, so current quality reports must use the typed validity texture and
hit/sky fractions instead.

The post-consumer-validity Release rerun
`artifacts/lumengi/C10/final-validity-consumer-gate-20260814.json` is the current
C10 bounded PASS. It verifies producer+radiance validity bits at interpolation,
independent hit/sky and hit-distance semantics, request-to-next-frame-ready
evidence, and `finalResolveConnected=true`, with no runtime device error. This
is still a bounded Radiance Cache producer/resolve gate; it does not close the
remaining VRAM, soak, far-field coverage, or full release matrix.

The standalone transmission contract at
`artifacts/lumengi/E1/transmission-gate/transmission-gate.json` is explicitly
`UNSUPPORTED_REFERENCE_ONLY`. It validates IOR/Fresnel/thickness/refraction
data and invalid-medium behavior without wiring a fake glass result into
`finalColor`. Production transmission still needs a dedicated ray/path
producer, history, resolve, and PathTracer reference comparison.

The C9 full-color mark/export gate is now closed for the current composite
contract. A direct-enabled mark-on run and an independent mark-off run are
recorded at
`artifacts/lumengi/C9/runtime-finalcolor-current-20260814/` and
`artifacts/lumengi/C9/runtime-finalcolor-markoff-20260814/`. The latter is
validated by
`artifacts/lumengi/C9/finalcolor-contract/finalcolor-contract.json` with
`status=PASS`: `DirectResolve.output + LumenGI.resolvedDiffuseGI` is finite,
nonnegative, producer-tagged, and numerically equivalent with mark/export
policy changes. This closes C9 only; it does not add rough-specular or
transmission terms.

## 2026-08-14 C6 provenance hardening

The Surface Cache host now defers GPU miss-request insertion by one host frame
and exports `requestObservedFrame` / `requestCaptureFrame` alongside cumulative
request counters. This prevents a readback/scheduler handoff from being
mistaken for same-frame publication. The strict N+1 runtime report remains
BLOCKED until a fresh artifact proves the frame stamps and explicit ready
transition; no image-derived inference is accepted.

## 2026-08-14 bounded-gate handoff

The strict C6 next-frame validator is available at
`tests/lumengi/run_c6_nextframe_gate.py`; its fixture passes and the older
reason-telemetry artifact is correctly BLOCKED for missing frozen request /
capture fields. C10 producer validity and C9 full-color equivalence are
bounded runtime PASSes. A2 source-quality, full C6 lifecycle, rough-specular
Host integration, transmission/glass production, GPU-wide VRAM, long soak,
and C11/C12 remain OPEN. Do not shut down or claim full release completion
until those artifacts exist or are explicitly handed off as blocked.

The subsequent C6 host change defers miss-request scheduler insertion by one
host frame and exports `requestObservedFrame` / `requestCaptureFrame`. A fresh
320x180 ten-frame runtime is clean, but the strict validator still reports
BLOCKED because the current cumulative counters do not yet form an unambiguous
N-to-N+1 event series. This is retained as an open provenance/lifecycle gate.

## 2026-08-14 latest bounded execution

The Surface Cache clock-domain fix now uses the monotonic capture-scheduler
frame for page ready/pending fences and request stamps; history resets no
longer rewind the cache lifecycle clock. The authoritative sphere-array
pressure run is
`artifacts/lumengi/C6/sphere-array-clockfix-v2-20260814/`: 27 evictions,
generation 1->2, stale-owner rejection, and page-clear telemetry all pass.
The universal per-request N+1 validator remains intentionally open because
cache-hit origin must be frame-associated and the final request needs a later
sample.

Radiance Cache query readback is now wired through an atomic UAV/readback
counter path. In
`artifacts/lumengi/C10/query-readback-v5-20260815/c10-producer-gate.json`,
query attempts reconcile exactly with hits plus misses and the producer gate
passes. The same run emits the frozen consumer projection
`radiance-cache-gpu-gate.json`, whose eight checks also pass. Coverage is
still bounded: only 4/32 projected probes are in bounds, so this is not
UE-equivalent far-field quality or release closure.

The remaining release blockers are unchanged: A2 raw-source/dynamic image
quality, C6 universal latency and long soak, C10 broad probe-position lighting
coverage, GPU-wide VRAM and 30-minute/2-hour soak, and production rough
specular/transmission. The latter remain diagnostic/reference-only and are not
connected to `finalColor`.

## 2026-08-15 C4/C5 current runtime

C4 Screen -> GDF -> HWRT was re-run on the current Release binary at
`artifacts/lumengi/C4/gdf-probe-router-current-20260815/`: 607 GDF hits and
2255 HWRT fallback hits, with finite/nonnegative outputs. C5 same-scene
full-scan/grid candidate equivalence passes at 320x180 and all 12 cards are
indexed in both paths. The coverage-quality portion remains OPEN because
coverage rejects exceed lookup attempts; the card-facing direction fix in
`ScreenProbe/LumenScreenProbeIntegrate.cs.slang` has standalone Slang compile
evidence, while a fresh cache-lighting runtime is still timing out before a
new post-fix image artifact is produced.

The scheduler-frame provenance fix has an independent strict-latency result at
`artifacts/lumengi/C6/strict-latency-clockfix-20260815/nextframe-gate.json`:
the high-budget lookup-on case passes frame-associated request/capture/ready/
lookup ordering with no same-frame publication. Tiny-atlas pressure remains a
separate eventual per-card completion and soak gate.

The follow-up event-ledger run
`artifacts/lumengi/C6/event-ledger-clockfix2-20260815/nextframe-gate-events.json`
passes 18 settled card/page events with explicit request/capture/ready and
generation frames; six final-tail records remain explicitly pending. A sphere
array pressure run records 59 card-specific events, including pending
allocation records, so the strict gate remains BLOCKED for unresolved budget
pressure rather than inferring completion from aggregate counters.

For C5, stable C6-graph A/B runs
`artifacts/lumengi/C5/c6-grid-off-v2-20260815/` and
`artifacts/lumengi/C5/c6-grid-on-v2-20260815/` have equal lookup attempts and
one hit each at 160x90. Grid-on still has more candidate rejects; candidate
preservation is bounded, while cache coverage quality remains OPEN.

## 2026-08-15 A2/C9 closure delta

A2 now has a real Release/Mogwai source-quality A/B at 640x360, checkpoints
1/8/32/96. The off/on manifests are
`artifacts/lumengi/A2/source-quality/off-20260815/` and
`artifacts/lumengi/A2/source-quality/on-20260815/`; the comparison report is
`artifacts/lumengi/A2/source-quality/compare-20260815/` and passes the frozen
raw-alpha invariant plus the spatial-variance improvement check. Camera-cut,
scene-reload, lighting-generation, and material/geometry dynamic manifests
under `artifacts/lumengi/A2/dynamic/*-20260815/` also pass their numeric
history/reset contracts. This closes only the bounded source/history gate;
multi-scene no-noise, raw producer coverage, and long soak remain OPEN.

C9 full-color/export equivalence is closed for the frozen direct-plus-diffuse
composite. Independent mark-on/mark-off runs are recorded at
`artifacts/lumengi/C9/runtime-markon-v3-20260815/` and
`artifacts/lumengi/C9/runtime-markoff-v4-20260815/`; the strict contract is
`artifacts/lumengi/C9/finalcolor-contract-v4-20260815/`. Pixel comparison
uses explicit mean/P99/absolute-max/relative-max tolerances for independent
process RT/NRD low-bit jitter; it does not waive producer metadata, finite,
nonnegative, or mark-off checks. Rough-specular and transmission remain
diagnostic/reference-only and are not connected to finalColor.

Current release blockers are C5 broad cache coverage, C6 pressure eventual
per-card completion and soak, C10 broad probe-position lighting coverage,
GPU-wide VRAM plus 30-minute/2-hour soak, and production rough-specular /
transmission. Do not shut down or claim full release completion until those
gates have a fresh artifact or are explicitly handed off as blocked.

## Verification snapshot (2026-08-15)

The single-thread Release `LumenGI.vcxproj` build completed with zero warnings
and zero errors. The Release CPU suite ran 111 tests from 11 Lumen suites,
with 111 passed and no failures; XML is at
`artifacts/lumengi/verification-20260815/lumen-tests.xml`. Python static
checks and self-tests for the C5/C6/C9/C10/A2 runners also pass. No Mogwai
process is left running. This is build/test hygiene only; it does not upgrade
the explicitly open production quality and release gates above.

## 2026-08-15 C10 raycast-gridfix evidence

The latest unique Release/Mogwai run is
`artifacts/lumengi/C10/raycast-gridfix-20260815/`. Runtime shader compilation
completed, and the strict producer and consumer reports both pass:
`c10-producer-gate.json` and `radiance-cache-gpu-gate.json`. Query readback is
reconciled at `1,382,400 = 30,900 hits + 1,351,500 misses`, with explicit
hit/sky validity and N-to-N+1 readiness. This closes the bounded
producer/consumer contract only. Coverage remains open because the miss
fraction is 97.76% and only 4/32 projected probes are in bounds; the legacy
S7 mirror report remains `partial` by design. Do not call this UE-equivalent
far-field quality or release closure.

E1 rough-specular runtime follow-up: the separate SceneRayQuery producer was
copied and compiled in Release. The unique Cornell artifact is
`artifacts/lumengi/E1/roughspec-runtime-20260815-v3/`; it completed without
Fatal/E_INVALIDARG and produced independent rough-specular diagnostic
outputs. It is not yet a fullColor/resolve consumer and has no multi-angle
roughness or performance/VRAM gate. Transmission remains `REFERENCE_ONLY`.

## 2026-08-15 C10 per-level telemetry

The C10 host/shader now exports explicit `coverageByLevel` counters. The
runtime artifact is
`artifacts/lumengi/C10/levelcoverage-runtime-20260815/`; producer and consumer
contracts pass, while the strict coverage report is `OPEN` because the run has
4/32 in-bounds projected probes and 97.76% query misses. Do not promote this to
far-field or release quality.

## 2026-08-15 C6 strict pressure gate

The current Release build and real sphere-array pressure run are recorded at
`artifacts/lumengi/C6/sphere-array-pressure-final11-20260815/`. With 384 cards,
16 atlas pages, one capture page per frame, 190 scheduler frames, and two tail
frames, the strict per-card pressure gate passes: 25 evictions, 9 generation
mismatch rejects, zero event drops, same-page generation reuse, and explicit
capture/ready or stale-terminal outcomes. This is bounded pressure/reuse only;
the universal request gate remains BLOCKED until every lookup/request has a
frame-origin and card association.

## 2026-08-15 C10 sphere-array coverage follow-up

The strict producer gate now handles the runtime interpolated hit/sky bitmask
encoding without weakening ordinary exclusive validity checks. Long and
static-only sphere-array Release runs at
`artifacts/lumengi/C10/sphere-levels-runtime-long-20260815/` and
`artifacts/lumengi/C10/sphere-levels-static-only-20260815/` pass producer and
GPU-consumer contracts. Per-level coverage is still `OPEN`: level 0 has valid
query hits, while levels 1-4 have no valid probe samples and in-bounds fraction
remains below threshold. Keep C10 broad far-field quality, A2 source quality,
rough/transmission resolve, VRAM/soak, and release matrix gates open.

## 2026-08-15 C10 scheduler fairness follow-up

Cold empty clipmap cells are now scheduled ahead of allocated slots without GPU
confidence, preventing dynamic level-0 starvation. The Release rebuild and 111
Lumen CPU tests pass. Static-only sphere-array evidence at
`artifacts/lumengi/C10/sphere-levels-fair-scheduler-v2-20260815/` reaches 3,072
resident probes and validates levels 0-4; producer/consumer gates pass. The
coverage gate remains `OPEN` for level-4 in-bounds ratio and level-5 validity;
keep broad far-field quality, A2, rough/transmission resolve, VRAM/soak, and
release matrix gates open.

## 2026-08-15 C5 planar cards and C10 reset hygiene

The Cornell planar-card producer now pads only the degenerate AABB axis into a
shared thin slab. Fresh Release/GPU A/B evidence is in
`artifacts/lumengi/C5/coverage-runtime-planar-20260815/`: 8 supported
instances/48 cards, 48 indexed cards, zero missing cards, finite output, and
grid/fullscan equivalence PASS. Cache-hit quality is still OPEN, so this is
not a production coverage pass. `LumenGIPass` also preserves an explicit
Radiance Cache reset request across setScene/hot-reload so the next execute
clears cross-scene payload and query state.

Latest C5 long-warmup confirmation: the 48-frame Cornell full-scan artifact at
`artifacts/lumengi/C5/coverage-runtime-planar-long-20260815/fullscan/` has no
fatal/device error but still reports zero valid cache hits from roughly 4.7k
attempts per frame. Keep grid/full-scan equivalence bounded PASS and Surface
Cache producer coverage OPEN; do not promote the planar-card fix to a quality
closure claim.

## 2026-08-15 C5 metadata-neighborhood result

Release/Mogwai validation of the bounded same-page 3x3 metadata/visibility
fallback is recorded at
`artifacts/lumengi/C5/coverage-neighborhood-20260815/fullscan/`. Runtime
compilation and 12 frames are clean, but cache hits remain 0/4,692; 453
owner-valid candidates still fail metadata, with depth/axis coverage rejects
dominant. Treat this as diagnostic evidence only; Surface Cache quality stays
OPEN.

## 2026-08-15 C6 per-card lifecycle result

The strict per-card event-ledger recheck
`artifacts/lumengi/C6/sphere-array-pressure-final11-20260815/nextframe-gate-v3.json`
is PASS: request frame, card/page/generation identity, capture/ready timing,
stale-terminal outcomes, sequence monotonicity, and ring-drop checks all pass.
This closes bounded pressure lifecycle association, not universal cache-hit
quality or soak.

## 2026-08-15 C5 facing-sign A/B result

The isolated facing-sign A/B at
`artifacts/lumengi/C5/coverage-facing-sign-20260815/` did not improve the
producer: cache hits remained `0/4692` and
`cacheLightingCounterTraced` remained zero. The temporary sign change was
reverted and the Release shader copy rebuilt. Keep C5 coverage OPEN; the
remaining producer-debug baseline is the capture metadata/depth/axis path.

## 2026-08-16 current execution delta

The capture shader was rebuilt with an explicit host-matching card record and no
capture-side backface rejection. This is the correct external-card-camera fix,
not a gate relaxation. The 48-frame fullscan/grid artifacts at
`artifacts/lumengi/C5/capture-backface-fix-20260816-long/` produce 21/31 real
cache hits and traced pages with finite outputs. The strict A/B gate still fails
the intermediate `probeInterpolated` delta while final resolved diffuse is
nearly identical, so C5 quality remains OPEN.

The sphere-array far-field run at
`artifacts/lumengi/C10/far-field-gpu-20260816/` reconciles GPU query counters and
passes bounded producer/consumer/fallback checks. Levels 0, 2, 3, and 4 have
valid query samples; level 1 and level 5 remain below the frozen coverage
threshold. C10 broad far-field, A2 dynamic source quality, rough-specular and
transmission production resolve, VRAM/soak, and release matrix gates remain OPEN.

The 60-second churn proxy at
`artifacts/lumengi/release/churn-proxy-20260816/` completed 3,600 frames with
material mutation, six scene reloads, twelve resizes, and no runtime error
records. Allocator statistics were not script-bound, so this is a stability
proxy only; it does not replace the 30-minute/2-hour soak or GPU-wide VRAM gate.

The strict C6 recheck
`artifacts/lumengi/C6/sphere-array-pressure-final11-20260815/nextframe-gate-v4.json`
also passes the monotonic surface-cache/scheduler clock contract and per-card
request/capture/ready association. This remains bounded pressure evidence only.

### 2026-08-16 deterministic grid fallback and far-field v4

`ScreenProbeIntegrate.cs.slang` now refuses an incomplete card-grid cell and
uses the full-card authority path; host grid bounds include the matching 0.04
shader epsilon. Release build/runtime compile pass. The 16-frame artifact
`artifacts/lumengi/C5/grid-deterministic-fallback-20260816/` has equal lookup
counters and complete candidate/page telemetry, but independent-process
`probeInterpolated` equivalence still misses the frozen 1e-4 threshold. Treat
grid acceleration as disabled-safe fallback and keep C5 quality OPEN.

The EnvMap-free far-field artifact
`artifacts/lumengi/C10/far-field-scene-z220-20260816-v4/` reaches all six levels
and reconciles GPU query readback. Producer validity and readiness pass; strict
consumer/fallback and broad level coverage do not. C10 remains bounded evidence.

### 2026-08-16 scheduler fairness and far-field v8/v9

The Radiance Cache CPU scheduler now reserves one refresh candidate for each
clipmap level when the refresh budget permits, with the remaining slots filled
by the prior deterministic score. `FalcorTest.exe --test-suite='Lumen.*'
--parallel=1` passes all 113 tests, including the new per-level fairness test.

The authoritative bounded far-field artifact is
`artifacts/lumengi/C10/far-field-scene-z220-20260816-v8/`: producer validity,
GPU consumer readiness, non-black fallback, and query reconciliation pass, and
all six levels are observed. C10 broad coverage remains OPEN because level-0
in-bounds coverage is only about 0.0813 against the frozen 0.5 threshold.
The v9 near-camera experiment is preserved as a negative control: producer
readiness passes, but fallback is black and query counters collapse to level 0,
so its coverage/GPU gates fail. Do not use v9 to claim far-field quality.

The remaining release blockers are unchanged: C5 intermediate grid equivalence,
C6 universal request-to-next-frame latency, A2 source variance/no-noise,
rough-specular and transmission production resolve, GPU-wide VRAM, dynamic
soak, multi-view screenshots, and final release matrix.

The fresh same-binary C5 run at
`artifacts/lumengi/C5/grid-disabled-fresh-20260816/` has identical lookup and
candidate counters for fullscan/grid-disabled, but the strict intermediate
output comparison still fails (`probeInterpolated` mean delta about `0.00256`).
The threshold remains unchanged and C5 is not promoted.

### 2026-08-16 feedback provenance and paired C5 runtime

`LumenGI.cpp/.h` now carries the Surface Cache feedback submission frame into
the per-card event ledger. Release and `FalcorTest --test-suite='Lumen.*'
--parallel=1` pass (`113/113`). The fresh sphere-array artifact
`artifacts/lumengi/C6/feedback-provenance-sphere-20260816/` shows 65 completed
request/capture/ready/lookup associations passing the strict frame-origin
checks; its only block is the final unsampled request batch. The explicit drain
run still has five frame-40 requests pending at its last sample, so this is
bounded provenance evidence, not universal C6 closure.

The same-process paired C5 runtime at
`artifacts/lumengi/C5/paired-equivalence-runtime-20260816-v2/` compiles and
executes both full-scan and grid passes with matching 48/48 candidate/page
telemetry. Frozen `1e-4` output equivalence fails from frame 2
(`probeInterpolated` mean delta `0.0121`, resolved diffuse `0.00120`),
therefore C5 quality remains `OPEN/FAIL` and the grid threshold is unchanged.

### 2026-08-16 latest bounded evidence: C5 control and C10 phases

The C5 full/full control artifact
`artifacts/lumengi/C5/paired-equivalence-full-full-20260816/` fails the same
strict intermediate output threshold from frame 2 as the full/grid run. This
points to Surface Cache lighting or independent temporal state as the next
debug boundary; it is not a reason to relax the 1e-4 contract.

The C10 phase manifest
`artifacts/lumengi/C10/two-phase-coverage-20260816/c10-coverage-phase-gate.json`
keeps near level 0 and far levels 1--5 separate. Far levels pass; near level 0
is OPEN at 0.0897 in-bounds versus 0.5. C10 broad coverage and release remain
OPEN, and no shutdown is authorized.

The cache-lighting control
`artifacts/lumengi/C5/paired-equivalence-cache-no-temporal-20260816/` still
fails with temporal/spatial disabled, while the all-cache-off control
`artifacts/lumengi/C5/paired-equivalence-no-cache-20260816/` passes. Continue
from Surface Cache/cache-lighting producer state; do not tune the denoiser or
relax the 1e-4 contract.

The authored near-field C10 diagnostic
`artifacts/lumengi/C10/near-field-authored-surface-focal12-20260816-v2/`
raises level-0 in-bounds to about 0.452, but fallback is black and the strict
consumer gate fails. Keep it diagnostic only; the two-phase manifest remains
the authoritative OPEN report.

An explicit capture-to-lighting UAV barrier was added for the three
capture-owned atlases and the LumenGI plugin was rebuilt. The recheck at
`artifacts/lumengi/C5/paired-equivalence-pointlight-uavbarrier-4f-20260816/`
still fails from frame 2 (`probeInterpolated` mean delta 0.00559) with equal
host publication hashes. Keep the barrier as ordering hygiene, but treat the
cache-lighting producer/runtime divergence as OPEN; no threshold was relaxed.

### 2026-08-16 fresh-plugin producer isolation

After a correct `/t:Rebuild /m:1` of the LumenGI plugin (not just Falcor.dll),
the point-light full/full artifact
`artifacts/lumengi/C5/paired-equivalence-pointlight-cache-hash-plugin-rebuilt-4f-20260816/`
shows identical page-metadata/page-to-card/render-list hashes, yet cache
lighting diverges from frame 2 (`probeInterpolated` mean delta 0.00559;
resolved diffuse 0.00048). The cache-direct atlas and cache-lighting counters
also differ. The capture-only control
`artifacts/lumengi/C5/paired-equivalence-pointlight-capture-only-20260816/`
passes four frames with zero deltas. Therefore the remaining C5 owner is the
cache-lighting producer/runtime state; no grid threshold was relaxed and C5
quality remains OPEN/FAIL.

### 2026-08-16 C6 tail closure and C10 near/far coverage

`tests/lumengi/run_surfacecache_effect.py` now records a four-frame provenance
tail after the last checkpoint. The fresh bounded pressure artifact
`artifacts/lumengi/C6/sphere-array-farfield-tail4-20260816/` has 148 samples and
`nextframe-gate.json` is `PASS` for per-card capture, ready and feedback timing.

`run_radiance_cache.py` accepts `LUMEN_RC_CAMERA_FOCAL_LENGTH`. The near-field
z=1.2/focal=10 artifact at
`artifacts/lumengi/C10/near-field-z1p2-focal10-20260816-v1/` passes producer and
level-0 coverage (0.585 in-bounds, 0.971 query-hit, 0.972 hit-distance,
finite/non-black fallback). The two-phase AND report
`artifacts/lumengi/C10/two-phase-coverage-20260816/c10-coverage-phase-gate-near-z1p2-focal10.json`
passes near level 0 plus existing far levels 1--5. This does not promote the
remaining C5/A2/rough-transmission/VRAM-soak/release gates.

The rebuilt visibility-atlas-clear recheck at
`artifacts/lumengi/C5/paired-equivalence-pointlight-visibility-clear-4f-20260816/`
still fails at frame 2 with the same `0.00559` probe delta. The per-dispatch
clear remains as stale-owner hygiene, but C5 producer determinism is still
OPEN/FAIL.

The paired diagnostic now snapshots the capture atlas before S3 lighting and the
post-lighting atlas. In
`artifacts/lumengi/C5/paired-equivalence-pointlight-capture-snapshot5-4f-20260816/`,
host publication fingerprints are equal, while sparse cache-lighting differences
still produce about `0.00559` probe-interpolated mean delta at frame 2. C5 stays
OPEN/FAIL at `1e-4`; the remaining owner is cache-lighting producer/runtime
determinism, not card-grid publication.

The rebuilt-plugin recheck
`artifacts/lumengi/C5/paired-equivalence-pointlight-raytype1-4f-20260816/`
uses the explicit canonical one-ray-type TLAS binding for inline cache-lighting
visibility queries. Frames 1--7 are exact, but frame 8 remains above the frozen
threshold (`probeInterpolated` mean delta `7.426e-4`). Keep this as a bounded
selection improvement only; C5 is still OPEN/FAIL.

The subsequent true grid-on/off run
`artifacts/lumengi/C5/paired-equivalence-pointlight-raytype1-grid-ab-4f-20260816/`
also runtime-compiles but fails from frame 2 while candidate/indexed-card
telemetry remains present. Keep C5 OPEN/FAIL at the unchanged `1e-4` threshold.

The provenance-instrumented full/full recheck at
`artifacts/lumengi/C5/paired-equivalence-pointlight-provenance-4f-20260816/`
has equal seed/frame/TLAS/ray-type/light-sampler/feedback variant fields and
equal card/grid telemetry on every frame, yet still fails the output contract
from frame 2. The next C5 owner is therefore GPU atlas/lighting producer
determinism internal to the dispatch; no host-table or threshold workaround is
justified.

The stricter tail-freeze rerun
`artifacts/lumengi/C6/sphere-array-farfield-tail8-freeze-20260816/` completed
with Mogwai exit 0 and full card/event telemetry, but `nextframe-gate.json` is
`BLOCKED`: sequence 161 remains pending at the final sample (request frame 151,
last sample 152). Keep the earlier tail4 artifact as bounded PASS only; this
rerun is negative evidence for unresolved request drain, not release closure.

A separate budget control at
`artifacts/lumengi/C6/sphere-array-farfield-budget64-tail64-20260816/`
keeps the same frozen pressure scene but raises capture budget to 64 pages per
frame and drains 64 frames. It is still `BLOCKED` after 208 samples: six
card-specific events remain pending and 169 events were dropped. This confirms
request-admission/scheduler backlog under tiny-atlas pressure rather than a
short-tail or runner-stimulus problem.

The event ledger capacity is now 8192 (was 512). The fresh runtime
`artifacts/lumengi/C6/sphere-array-farfield-budget64-ring8192-20260816/` reports
zero dropped events, while the strict lifecycle gate remains BLOCKED on
unresolved requests. This is telemetry preservation, not a relaxed PASS.

### 2026-08-16 C5 capture-order and consumer-barrier fix

Release `LumenGI` now publishes capture→lighting and lighting→export/lookup
UAV dependencies, and each capture page has an `R32Uint` atomic order atlas.
Overlapping raster triangles select a deterministic depth/primitive winner before
writing material and metadata, preventing cross-instance cache producer drift.

`artifacts/lumengi/C5/paired-equivalence-pointlight-capture-order-20260816/`
passes the strict 8-frame paired gate (`192/0/0`, unchanged tolerance `1e-4`),
including equal per-frame cache hits. This closes C5 paired equivalence only;
coverage reject quality, C6 universal request convergence, and release soak/VRAM
gates remain open.

### 2026-08-16 C2 resize matrix and C6 bounded latency

`artifacts/lumengi/C2/resolution-matrix-final-20260816/resolution-matrix.json`
passes the full Release resize matrix, including odd dimensions and the
same-probe-count/aspect-change cases. Marked outputs were finite,
non-negative, and correctly sized at every step.

`artifacts/lumengi/C6/strict-latency-clockfix-20260815/nextframe-gate.json`
passes the bounded high-budget lookup-on latency workload. Tiny-atlas pressure
still has unresolved per-card requests, so universal C6 convergence remains
open and must not be inferred from this bounded result.

### 2026-08-16 C11 quality-preset defaults

The Release plugin now maps Low/Medium/High/Reference to monotonic probe,
capture, history, spatial, GDF, and Mesh-SDF defaults while preserving explicit
graph overrides and leaving producer switches unchanged. The four-preset smoke
at `artifacts/lumengi/C11/preset-smoke-qualitydefaults-20260816/` completed with
exit 0 for every preset. Full image/performance/VRAM and hot-switch evidence is
still required before C11 is release-closed.

`LumenGIPass::setProperties()` now performs the same preset-default
transaction for RenderGraph hot updates, and the UI dropdown follows that
path. Release build plus 113/113 Lumen tests and the dependency-free fixture
pass. The bounded Mogwai hot-switch run did not finish and is retained as
`BLOCKED` under
`artifacts/lumengi/C11/preset-hot-switch-20260816-runtime-blocked/`; do not
promote this configuration change to a complete C11 release gate without a
clean runtime series.

The clean runtime series is now available at
`artifacts/lumengi/C11/preset-hot-switch-20260816-final/quality-hot-switch.json`.
It supersedes the idle-launcher record for the current build: all five preset
updates executed, `qualityPresetStats` matched the expected defaults, and the
marked outputs passed finite/non-negative readback. Record this as bounded C11
hot-switch PASS only; image-quality, performance, VRAM, and soak gates remain
open.

The scheduler completion contract now includes page/generation-validated
`isCaptureComplete()`, and Host telemetry advances request events only after
that validation. Release build and all 113 Lumen tests pass. The fresh pressure
artifact
`artifacts/lumengi/C6/scheduler-completion-api-pressure-tail64-20260816/nextframe-gate.json`
passes strict per-card lifecycle validation with zero dropped events. Full
tiny-atlas closure remains open because stale-texel and unique card/page
identity evidence are still absent.

The 320x180 Cornell A2 paired source-quality run at
`artifacts/lumengi/A2/source-quality/compare-release-320x180-20260816/source-quality-comparison.json`
passes raw-source/alpha/history telemetry, but is `NO_IMPROVEMENT` under the
unchanged tail-RMSE rule. Moments lower local variance slightly without closing
the production no-noise gate.

The follow-up single-point history-weight recheck is available at
`artifacts/lumengi/A2/source-quality/a2-weight08-compare-320x180-20260816/source-quality-comparison.json`.
It is a bounded `PASS` with unchanged alpha/validity contracts and both frozen
quality checks passing. Do not generalize it to dynamic or multi-scene closure.

The producer-isolation run
`artifacts/lumengi/C5/producer-isolation-full-full-20260816-v1/c5-paired-equivalence.json`
remains a strict `FAIL` despite matching capture arrays and candidate metadata;
cache-direct radiance differs, so C5 producer determinism/coverage is still an
open release dependency.

The S2 churn runner now binds `surfaceCacheStats`; the 60-second proxy
`artifacts/lumengi/release/churn-stats-bound-20260816/churn.json` has readable
series and `divergence_ok=true` with zero allocation failures/lost pages. It is
not a 30-minute/2-hour soak or GPU-wide VRAM proof.

The follow-up C5 capture-owner tie-break rebuild is available at
`artifacts/lumengi/C5/producer-isolation-full-full-20260817-cardowner-v3/`.
The same-process paired equivalence report is `PASS` across all four
checkpoints at the unchanged `1e-4` mean-absolute tolerance for capture,
direct cache, probe, and resolved diffuse outputs. This closes the controlled
producer-drift reproduction; broad cache coverage, tiny-atlas identity, and
release soak/VRAM gates remain independent.

The C6 event-ledger pressure rerun at
`artifacts/lumengi/C6/event-ledger-65536-pressure-20260817/nextframe-gate.json`
passes 208 samples with zero dropped event records, 46 evictions, and 9
stale-owner rejects. The ledger capacity increase is telemetry-only; scheduler
budgets and all strict lifecycle thresholds are unchanged. Long soak and
GPU-wide VRAM remain open.

The A2 dynamic runner now treats `RenderSettingsChanged` as a lighting epoch
transition. Fresh Release runs for static, camera-cut, scene-reload,
lighting-generation, and material/geometry cases are `PASS` with five PNG/EXR
checkpoints each under `artifacts/lumengi/A2/dynamic-*-20260817/`. They prove
runtime capture and reset/generation telemetry; visible low-frequency mottle
remains, so the production no-noise Gate is still open.

The C6 tiny-atlas replay
`artifacts/lumengi/C6/tiny-sphere-drain-replay-20260818/surfacecache-effect.json`
now uses the explicit `surfaceCacheEvents` card/page ledger. It completes with
160 per-frame samples, 48 distinct cards, 16 distinct pages, eviction/stale-owner
transitions, and an observed page-clear sentinel. The strict companion gate
`nextframe-gate.json` is `PASS`; this remains bounded lifecycle evidence rather
than a GPU-wide VRAM or long-soak release result.

The A2 dynamic quality aggregation
`artifacts/lumengi/A2/dynamic-quality-20260818/dynamic-quality-gate.json` is
`PASS` for all five fresh transition manifests. It validates real PNG/EXR
provenance and frozen variance/tail-RMSE rules; multi-scene no-noise and release
soak requirements remain open.

The S2 runtime churn proxy
`artifacts/lumengi/release/churn-telemetry-runtime-20260818/churn.json`
captured 3600 frames with complete canonical Surface Cache telemetry and
authoritative `nvidia-smi` start/end VRAM samples. Renderer/device provenance was
not exposed by Mogwai scripting, so the report is `BLOCKED`; the bounded proxy
was stopped after memory growth and is not a 30-minute/2-hour/8-hour soak.

The C6 strict drain gate was corrected to anchor on the last newly-added
`surfaceCacheEvents.sequence`, rather than the per-frame request readback stamp.
`artifacts/lumengi/C6/tiny-sphere-drain-tail8-budget64-20260818/pressure-drain-gate-sequence-anchor.json`
is `PASS`: sequence 141 has 20 subsequent scheduler samples, zero pending
events, and zero lifecycle violations. The earlier readback-stamp `BLOCKED`
result is superseded without changing any lifecycle threshold.

The A2 dynamic quality runner now requires exact checkpoints `[1, 8, 16, 32,
64]`, capture `PASS`, and explicit scope metadata. Current five-case Cornell
evidence remains bounded `PASS`; multi-scene and production no-noise remain
`OPEN`.

The C10 query-counter readback now exports the exact cache frame at dispatch
submission (`queryCountersSubmittedFrame` and
`levelQueryCountersSubmittedFrame`) instead of inferring the frame from the
next CPU clipmap tick. Release rebuild and the 113-test Lumen CPU suite pass;
this fixes provenance across reset/scene transitions but does not claim broad
Radiance Cache coverage or final-quality completion.

The 2026-08-19 Release two-phase C10 run is now a strict coverage `PASS`:
near-field `z=1.2, focal=10` recorded level-0 in-bounds `4806/8215` with
query-hit fraction `0.9712` and valid hit-distance fraction `0.9717`; the
far-field phase covered levels 1-5. The frozen-threshold phase manifest is
`artifacts/lumengi/C10/two-phase-coverage-20260819/c10-coverage-phase-gate.json`.
This is bounded two-phase coverage evidence, not a claim that all scenes,
camera paths, image quality, or release soak are complete.

The serial C9 Cornell/front mark-off and mark-on attempt is recorded under
`artifacts/lumengi/C9/finalcolor-dual-20260819/`. Producer metadata and finite
composite outputs are present, but strict export equivalence is `FAIL`: mean
absolute error `8.07e-5` and p99 `1.10e-3` exceed the frozen limits. Keep C9
full-color equivalence open; do not loosen the comparison thresholds.

The follow-up same-process endpoint run is under
`artifacts/lumengi/C9/same-process-endpoint-20260820-v5/`. Marking
`ResolvedCompositePreview.out` first fixes Mogwai's post-unmark main-output
lookup: `sameProcessMarkTransition=PASS_BOUNDED` and the endpoint remains
readable after the lazy RenderGraph recompile; runtime shader compilation also
completed without a fatal/device error. Producer metadata and finite composite
output are bounded PASS, but mark-on/off remains materially different
(`meanAbsError=2.339e-3`, `maxAbsError=0.1559`) and no independent export pair
was captured. The strict contract is `PASS_BOUNDED` with
export-equivalence `BLOCKED`; keep C9 full-color equivalence open and preserve
the frozen tolerances.

The evidence harness is now complete offline: `run_screenprobe_dynamic_sidecar.py`
requires explicit linear raw/resolved sidecars for A2, and
`run_release_soak_launcher.py` prepares the authoritative serial 30-minute plus
2-hour run. Both remain strict and do not promote missing runtime evidence.
Remaining production gates are C9 export equivalence, A2 linear no-noise
improvement, GPU-wide VRAM, and the long soak.

The A2 sidecar now has real bounded evidence: both
`artifacts/lumengi/A2/dynamic-cornell-no-noise-20260820/no-noise-linear.json`
and `artifacts/lumengi/A2/dynamic-arcade-no-noise-20260820/no-noise-linear.json`
are `PASS` for all five transitions and checkpoints `[1, 8, 16, 32, 64]`.
This is linear raw/resolved variance evidence; it does not promote C9 export
equivalence or the broader release checklist.

The latest serial GPU continuation is recorded under
`artifacts/lumengi/C9/deterministic-replay-20260820-v1/` and
`artifacts/lumengi/release/soak-launch-20260822-rtx0-v2/`. C9 deterministic
replay completed both same-process phases but remains `FAIL` at the frozen
mean/max limits (`3.5627e-5` / `5.2490e-3`); the final-color contract is also
`FAIL`. The S2 child completed 30 minutes/108000 frames with stable
authoritative GPU-wide VRAM samples, but release evidence is `BLOCKED` because
renderer `Device.info` is unavailable, the child host did not exit cleanly,
and the 2-hour phase was not run. The launcher now records intentional host
termination when a complete child artifact is present, without bypassing any
provenance or duration gate. Do not shut down or claim full completion until
C9 equivalence, renderer provenance, 2-hour/8-hour soak, and remaining
production gates are closed.

The 2026-08-22 follow-up added a read-only live renderer binding at
`Source/Mogwai/MogwaiScripting.cpp`; the 60-second smoke proves
`m.device.info` provenance for RTX 2060 SUPER / D3D12. C9's new same-frame
retained-resource artifact is a truthful `PASS_BOUNDED` only:
`artifacts/lumengi/C9/same-frame-runtime-20260822/same-frame-gate.json`.
It performs zero producer executions and does not close strict recompiled
export equivalence.

S2 v4 completed the 30-minute dynamic child with authoritative provenance, but
the required two-hour churn failed after about 422.6 seconds with
`MemoryError: bad allocation` at `m.renderFrame()` and no soak child JSON.
Keep `artifacts/lumengi/release/soak-launch-20260822-rtx0-v4/release-soak-gate.json`
as `BLOCKED`; diagnose renderer/Surface Cache scene-reload/resize ownership
before any further long run. Do not lower mutation cadence or call this a soak
PASS, and do not shut down until the remaining production gates close.

The 2026-08-30 continuation isolated the material-only path for 1200 logical
seconds / 72,000 frames at
`artifacts/lumengi/release/soak-isolation-20260830-material-1200s/churn.json`.
It recorded 1,201 material mutations, complete canonical stats, live
`m.device` provenance, and no allocation failure. To bound scene replacement
lifetimes, `Mogwai::Renderer::setScene()` now calls `Device::wait()` before
replacing an existing scene and again after graph/pass release, allowing
deferred GPU resources to be reclaimed between reloads. Release `Mogwai` was
rebuilt with `/m:1`, CodeGraph was synchronized, and all offline self-tests
passed. A fresh post-fix two-hour churn is still required; until it produces
complete evidence, S2 remains `BLOCKED` and shutdown is not authorized.

The S2 launcher now writes a strict `RUNNING`/`BLOCKED` manifest at startup and
after each phase. This preserves completed dynamic evidence if the launcher is
interrupted before the soak phase or final aggregation; the offline gate still
requires `READY_FOR_OFFLINE_GATE` and all authoritative duration/provenance
checks.

A fresh C9 replay after the scene-fence change remains strict `FAIL` at
`artifacts/lumengi/C9/deterministic-replay-20260830-v2/c9-export-repro.json`
(mean `5.0122e-5`, p99 `6.1035e-4`, max `2.5635e-3`). Mean/p99 still exceed
the frozen limits; the lifetime guard is not a numerical-equivalence fix. The
first post-fix S2 launcher attempt was interrupted during renderer startup and
left only compile logs under
`artifacts/lumengi/release/soak-launch-20260830-postfix-v5/`; it is not a gate
result. Future runs should use the launcher's new phase checkpoints.

The checkpointed post-fix retry at
`artifacts/lumengi/release/soak-launch-20260830-postfix-v8/` completed the
30-minute dynamic phase (`dynamic=PASS`) and began the required soak. After
about four minutes, system free memory fell below 0.5 GB while the cold soak
Mogwai process approached 8 GB working set, so only that exact child PID was
stopped for host safety. The launcher finalized with `dynamic=PASS`,
`soak=BLOCKED` and no soak child artifact; the offline report is
`.../release-soak-gate.json`. Preserve this as a safety stop, not a soak
verdict, and rerun with sufficient host memory before considering S2 closure.

Strict C9 replay was also isolated to retain only the production-required
`LumenGI.resolvedDiffuseGI` mark during mark-on; normal showcase captures are
unchanged. Fresh v3/v4 artifacts remain `FAIL` only on mean error (v3
`2.6779e-5`, v4 `3.1251e-5`; p99/max are within bounds). A temporary
NoResampling experiment was worse and was not kept as a production setting.
The replay thresholds remain frozen.

The S2 launcher now samples authoritative host available memory in addition to
GPU-wide VRAM. It performs a preflight and applies a default 0.5 GiB safety
threshold (`--min-host-free-gib`); crossing it terminates only the exact child,
records `resource_guard` and host samples, and leaves the phase `BLOCKED` for
the offline gate. This is a safety/diagnostic guard only and does not weaken
the two-hour duration, churn, VRAM, or C9 thresholds. Rerun the checkpointed
soak only when the host has enough memory; do not reinterpret the existing v8
stop as a soak PASS.

The first v10 launch hit the former 1 GiB guard at 0.959 GiB during cold
startup, before dynamic evidence was complete. The default is therefore
calibrated to 0.5 GiB (still above the earlier ~0.43 GiB safety stop) so cold
warm-up can complete while the host guard remains active.

Follow-up hardening (2026-08-30): `run_churn_short.py` now unloads the current
scene before constructing its replacement and fences pre/unload/post boundaries;
the artifact records the complete `resource_sync` event sequence. The 90 s
three-reload probe passed all nine waits but still showed host working-set
growth, so S2 remains BLOCKED pending a complete long run. The launcher waits
for the `CHURN seconds` readiness marker (or 180 s) before enforcing the normal
0.5 GiB guard and keeps a 0.25 GiB startup hard floor. C9 strict replay adds a
post-`removeGraph()` fence and reverse-order diagnostics; v6 off-first remains
strict FAIL on mean error only (`3.3392e-5`).

The v13 launcher run completed its dynamic phase at 1800 s/108000 frames with
one reload and one resize (`resource_sync` 4/4 PASS). The soak phase was safely
stopped at 0.473 GiB after four reloads, producing no child soak artifact and
remaining `BLOCKED`. The launcher now defaults to phase-specific 30-minute
dynamic and 60-minute soak reload/resize cadences (explicit environment values
still override these for bounded diagnostics); no duration, VRAM, or memory
threshold was relaxed.

The v14 rerun closed S2: dynamic 1800 s/108000 frames and soak 7200 s/432000
frames both passed the offline gate with 361/1441 authoritative VRAM samples,
stable `residentBytes`/`allocatedPages`, and complete reload/resize resource
sync. Canonical evidence is
`artifacts/lumengi/release/soak-launch-20260830-postfix-v14/release-soak-gate.json`.

C9 follow-up A/Bs remain diagnostic only: direct-NRD-off mean `4.0274e-5`,
indirect-NRD-off `7.5287e-5`, and default settle-192 `3.3476e-5`; all strict
replays fail the unchanged `2e-5` mean bound while p99/max/relative-max remain
within bounds. The evidence points away from NRD or settle length and toward
mark-output resource lifetime/aliasing/barrier behavior; do not relax the gate.

The next strict replay adds an explicit `unloadScene()` boundary after the
first phase, before `SceneBuilder` constructs the second Scene, and fails closed
if its device fence is unavailable. This lowered the fresh v11 mean error to
`2.4324e-5` (p99 `3.6621e-4`, max `2.1065e-3`), but it is still strict
`FAIL/OPEN`; combining the boundary with settle-192 regressed to `2.6635e-5`.
Keep the lifecycle boundary and frozen thresholds, and use the v11/v12 artifacts
as the baseline for the next resource-lifetime/barrier telemetry pass.
A v13 symmetric-readback experiment was also rejected after regressing to mean
`3.1941e-5`; the strict replay keeps its original intermediate readback behavior.
A v14 FrameCapture-parity experiment was rejected after regressing to mean
`5.3091e-5`; keep the established capture/readback schedule until the compiler
and barrier trace explains its stabilizing effect.

The v16 opt-in RenderGraph resource trace records real texture byte sizes,
object/GFX identities, bind flags, lifetimes, and graph outputs for both fresh
replays. It confirms that mark-on adds only `LumenGI.resolvedDiffuseGI` to the
graph output set (`outputs=4` versus `3`) and extends that resource lifetime to
the external output fence; bind flags and allocation sizes remain identical,
and all native identities are distinct after scene reload. The replay remains
strict `FAIL/OPEN` at mean `2.4343e-5` (p99 `3.6621e-4`, max `2.1065e-3`,
relative max `2.4668e-5`). Keep the telemetry opt-in and investigate queue/barrier
ordering only if a future trace shows a real hazard; do not infer physical aliasing
from the lifetime metadata or relax the frozen C9 thresholds.

The test-only producer-trace path now adds three `BlitPass` sentinels when
`LUMEN_C9_PRODUCER_TRACE_OUT` is set, preserving the source endpoints as
unmarked resources while exporting float32 sidecars. The v20 diagnostic run
measured DirectResolve mean delta `2.1424e-6`, LumenGI `4.6613e-5`, and
Composite `4.7884e-5`, localizing the residual C9 error to LumenGI rather than
the direct producer. Disabling only Lumen temporal filtering (v21) reduced the
Lumen delta to `3.3638e-5` but increased max error to `1.8646e-2`; disabling
both temporal and spatial filters (v22) still measured `2.8192e-5`. These
diagnostic topologies do not close C9 and the production defaults remain
unchanged. The companion offline diff is
`tests/lumengi/run_c9_producer_snapshot_diff.py`.

The v17 readback extension of the same opt-in trace confirms the scheduling
asymmetry directly: mark-on performs four frame-capture readbacks (including
`LumenGI.resolvedDiffuseGI`), while mark-off performs three. Each readback logs
its `CopySource` transition, submit/signal fence, and host wait; the later
`to_numpy()` reads are no-op barriers because the resource is already in
`CopySource`. The run measured mean `4.3308e-5` and therefore did not improve
C9. A telemetry-off v18 control measured `3.3280e-5`, so the trace is retained
as diagnostic evidence only and no behavior or threshold change is justified.

The v24-v26 producer-trace matrix isolates the cache interaction. With both
Surface Cache and Cache Lighting disabled (v24), with Surface Cache disabled
alone (v25), or with Cache Lighting disabled alone (v26), the LumenGI producer
delta is exactly zero in the test-only sentinel topology and the corresponding
offline C9 comparisons pass. The v20 both-on run remains the only matrix cell
with a large LumenGI delta, so the interaction is localized to the combined
cache path; neither cache switch is being disabled in production. A strict
no-cache v29 control independently passed (`mean=2.8565e-6`,
`p99=7.1168e-5`, `max=2.1065e-3`), confirming the direct/non-cache baseline.
The opt-in v27 pre-FrameCapture `m.device.wait()` fence was recorded PASS on
both phases but still failed the frozen mean bound (`3.0024e-5`); the v28
no-wait control was higher (`6.1814e-5`). This is scheduling evidence only,
not a production fix or a threshold change.

The v30 diagnostic disabled only the ScreenProbe Surface Cache lookup while
leaving capture and Cache Lighting enabled; strict C9 passed at the v29
baseline (`mean=2.8565e-6`, `p99=7.1168e-5`, `max=2.1065e-3`). The v31
diagnostic then kept lookup and cache-radiance replacement enabled but disabled
only feedback/request atomics and their next-frame readbacks; strict C9 again
passed (`mean=3.8822e-6`, `p99=1.2207e-4`, `max=2.1065e-3`). A same-build
default-feedback v32 control failed (`mean=2.4613e-5`, `p99=3.6621e-4`,
`max=3.4180e-3`). This localizes the residual to the feedback/request side
effects or their host scheduling interaction. The new
`LUMEN_C9_DISABLE_SURFACE_CACHE_FEEDBACK` switch is test-only and defaults off;
production lookup, feedback, request, and all C9 thresholds remain unchanged.

The v34 rerun includes the stale-pending-readback guard and records host
telemetry in each replay JSON. With lookup still active it observed
`cacheLookupAttempts=58470` and `cacheLookupHits=2905`, while
`surfaceCacheFeedbackHits/Pages=0` and `surfaceCacheRequestRaw/Cards=0`; strict
C9 passed (`mean=2.8565e-6`, `p99=7.1168e-5`, `max=2.1065e-3`,
`relative=2.4669e-5`). This is the canonical diagnostic artifact; it does not
promote the default feedback-enabled path, which remains `FAIL/OPEN` in v32.

The v35 opt-in explicit UAV-barrier A/B (`LUMEN_C9_FORCE_CACHE_READBACK_UAV_BARRIER=1`)
did not close the default path: strict C9 remained `FAIL/OPEN` at
`mean=2.7820e-5` (`p99=4.3321e-4`, `max=2.5635e-3`). It also changed cache
activity (`cacheLookupHits=232`, feedback hits `130291`, request raw
`3100534`), so it is a timing perturbation rather than evidence of a missing
state transition. Keep the switch diagnostic-only; `copyResource()` already
emits the UAV-to-copy transition in the normal path.

The v36-v38 feedback split is the next diagnostic boundary. v36 set
`LUMEN_C9_DISABLE_SURFACE_CACHE_FEEDBACK_ATOMICS=1` while retaining readback;
lookup remained active (`58470` attempts / `2905` hits), host feedback/request
counters were zero, and strict C9 passed. v37 set
`LUMEN_C9_DISABLE_SURFACE_CACHE_FEEDBACK_READBACK=1` while retaining shader
atomics; lookup remained `58470/2905`, but strict C9 failed
(`mean=3.7652e-5`, `p99=6.1035e-4`, `max=3.4180e-3`). v38 restored both split
switches to false and reproduced the default failure (`mean=4.2632e-5`,
`p99=6.1035e-4`, `max=3.6621e-3`) with nonzero feedback/request activity.
The split switches are test-only and the old aggregate
`LUMEN_C9_DISABLE_SURFACE_CACHE_FEEDBACK` remains a compatibility alias.

The offline `tests/lumengi/run_c9_feedback_split_validator.py` checks mode
flags, lookup activity, zero host feedback/request counters, and telemetry
freshness. Self-test plus v36/v37 artifact checks pass. These diagnostics
localize host readback/scheduler timing but do not justify a production change;
the default C9 gate remains `FAIL/OPEN` and thresholds stay frozen.
