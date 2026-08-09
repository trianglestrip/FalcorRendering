# LumenGI 交接单（2026-08-09）

> 供后续对话直接接续。仓库：`F:\project\FalcorRendering`，分支 `codex/lumen-gi`（远端已同步）。
> 完整执行计划见 `task.md`，历史进度见 `todo.md`。先读本文件再读 `todo.md`/`task.md`。

## 0. 接手必读

- 分支 `codex/lumen-gi`，基线 `eb540f67`；最近提交见文末 git log。
- 所有改动均已提交推送（最后一个 `ce180fab`）。
- **AGENTS.md 规则**：代码分析先用 `codegraph_explore`；`.codegraph/` 不要提交。
- **不要并发跑多个 MSBuild**（C1041 PDB 冲突历史）；仓库级构建用 `--parallel 1`。
- **GPU 只有 1 块**（RTX 2060 SUPER）：GPU 测试串行。
- **新 shader 必须过 Mogwai 实机编译**（文件存在≠已编译）。

## 1. 当前阶段状态

| 阶段 | 状态 | 说明 |
|---|---|---|
| S0 工程骨架 | ✅ Gate 关闭 | 证据 `artifacts/lumengi/S0/` |
| S1 HWRT 基线 | ✅ Gate 关闭 | emissive NEE、MIS/数值防护；证据 `S1/`（reference-compare/metrics2.json） |
| S2 Surface Cache | ✅ Gate 关闭 | Cards/Atlas/调度器/Capture；coverage=1.0、churn 无泄漏；`S2/` |
| S3 Cache Lighting | ✅ Gate 关闭 | direct+多反弹反馈；stability 15/15；`S3/gate/` |
| S4 Screen Trace/Probe | ✅ Gate 关闭 | HZB（独立纹理数组）+screen trace+probe grid+integrate/interpolate；`S4/` |
| S5 时域/空域 | ✅ Gate 关闭 | temporal 14/14、spatial 14/14、ghost 4/4；`S5/` |
| S6 Mesh SDF/GDF | 🔶 集成打通，Gate 未正式关 | builder→cache→volume→atlas→实例表→GDF compose→sphere trace 全链路运行（compose dispatch bug 已修）；缺：软件追踪 vs HWRT 数值对比、S6-C 系列、Screen+SDF 路径完整 gate |
| S7 Radiance Cache | 🔶 组件落盘，GPU 未接 | `LumenRadianceCache.h` + 23 CPU 测试；需 GPU 集成（trace/query/预算） |
| S8 优化/质量档 | 🔶 preset 落盘，GPU 未接 | `LumenQualityPreset.h` + 7 CPU 测试；四档热切换接线未做 |
| S9 发布回归 | 🔶 核心回归已跑 | analytic/dynamic/stability/s2verify/smoke + **110/110 CPU 单测**；缺 image matrix/独占 GPU 性能/soak |

**CPU 单测 110/110**（11 套件：Stats/Sampling/SurfaceCache/Scheduler/HZB/GDF/Atlas/Cache/Scene/Quality/RadianceCache）。

## 2. 剩余待办（按优先级）

1. **Arcade cache lighting dispatch E_INVALIDARG（本轮新发现的 bug）**
   - 现象：Arcade 场景 + `useSurfaceCache+useCacheLighting` 崩（`runCacheLighting` dispatch），Cornell/pointlight 正常。
   - 复现：`tests/lumengi/run_diag_env.py`；日志 `artifacts/lumengi/diag_env*.log`。
   - 线索：Arcade 仅 6 静态 mesh（36 卡，非尺寸）；差异=env+emissive+analytic 全开。排查过 Data.slang sampler 条件化仍正确；疑 envMapSampler/LightBVH 在 Arcade 的绑定或 LightCollection 构建。
2. **800x450 Arcade 基线崩溃**（分辨率相关边缘 case；640x360 正常；Cornell 800x450 正常）。
3. **S6 Gate 正式证据**：software trace 命中距离 vs HWRT 对比（同方向）、Screen+SDF 路径输出、S6-C 系列脚本（`run_sdf_trace.py` 已备，通道名 S6_TODO）。
4. **S7 GPU 接线**：Radiance Cache 接入 LumenGI.cpp（probe trace/query/预算，复用 `LumenRadianceCache.h` 接口）。
5. **S8 质量档接线**：四档参数热切换（`LumenQualityPreset.h` → LumenGI 属性 + reset）。
6. **S9 完整矩阵**：image tests、独占 GPU 性能（3 轮）、30min/2h soak、D3D12+RT validation（Debug 串行）。

## 3. 关键技术坑（避免重踩）

- **D3D12 原生 mip 链是 floor-halving**；LumenGI 的 HZB 用 **ceil-halving**（保守），因此 HZB 是**每级独立纹理数组**（`gHZBMips[16]`），不能用 create2D 的 mipCount 参数。消费端 `hzbMipSize` 也是 ceil。
- **UAV 数组绑定必须 `setUav`**（Assignment 绑 SRV 描述符 → dispatch E_INVALIDARG）。
- **shader 的 Texture3D/Texture2D`<float>`（32 位）与 R16Float/R8Snorm 资源类型化视图不匹配** → dispatch E_INVALIDARG；host 侧统一 R32Float（或 shader 用 half/snorm 类型）。
- **未绑定 sampler/纹理全局参数进 root signature → dispatchE_INVALIDARG**：可选 sampler 必须 `#if HAS_*` 条件声明；`_EMISSIVE_LIGHT_SAMPLER_TYPE` 未绑定时 host 必须 `addDefine("_EMISSIVE_LIGHT_SAMPLER_TYPE","255")`（Null）pin，否则宏残留致重编译 dxc 崩溃。
- **类名是 `LumenGIPass`**（插件 id 仍是 "LumenGI"）。`LumenGI` 已被 `namespace LumenGI::MeshSDF`（MeshSDF 组件）占用，**不要加 `using LumenGI = LumenGIPass`**（重定义冲突）。测试引用用 `LumenGIPass::`。
- **graph 链路中间通道必须 markOutput**：Falcor 未 mark 的可选输出不分配 → `renderData.getTexture` 返回 null → 上游 pass 跳过 → 下游 0（showcase 曾因此全黑）。展示链路时把 probeInterpolated/temporalFiltered/spatialFiltered 都 mark。
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

常用 GPU 脚本：
`run_smoke.py`（Cornell 冒烟）、`run_s2verify.py`（S2 回归）、`run_analytic.py`/`run_dynamic.py`/`run_stability.py`/`run_lightstep.py`（S1/S3 gate）、`run_cards_coverage.py`/`run_churn_short.py`（S2）、`run_screentrace.py`/`run_probe.py`/`run_probe_interp.py`（S4）、`run_temporal_verify.py`/`run_spatial_gate.py`/`run_temporal_ghost.py`（S5）、`run_s6_gdf.py`/`run_sdf_*.py`（S6，通道 S6_TODO 未冻结大多 SKIP）、`run_showcase.py`（效果展示，`artifacts/lumengi/showcase/*.png`）。

## 5. 效果展示（用户可见）

- `artifacts/lumengi/showcase/cornell-gi.ToneMapperDisplay.dst.96.png` — Cornell 全功能 GI（96 帧收敛，tone-mapped）
- `artifacts/lumengi/showcase/cornell-gi-effect.png` — 同图截图
- `artifacts/lumengi/showcase/cornell-gi.*.exr` — probeInterpolated/temporalFiltered/spatialFiltered 中间层
- `artifacts/lumengi/screenshots/panel-lumen-vs-pt.png` — S1 对比面板
- 全功能链 Cornell 数值：spatialFiltered mean≈0.51/max≈8.8；emissive_glow mean≈73/max≈896

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
