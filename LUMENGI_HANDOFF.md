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
