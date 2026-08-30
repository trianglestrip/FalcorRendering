# LumenGI 视觉调试记录

> 截止：2026-08-11。本文只记录已经落盘的 Mogwai 截图/EXR 和对应的视觉判断；不把截图通过误差阈值或“文件存在”当成 C7/C8 生产链闭环。

## 统一协议

- 所有列出的 `ToneMapperDisplay.dst.*.png` 为 800x450（EXR 同尺寸）；PNG 是固定曝光显示结果，当前脚本默认 `LUMEN_RESOLVED_SHOWCASE_EXPOSURE=0.0`。
- 文件名末尾 `.32`/`.96` 是该次 capture 的 settle frame 数，不是每像素样本数。
- 当前入口为 [`tests/lumengi/run_resolved_showcase.py`](../tests/lumengi/run_resolved_showcase.py)。默认值包括 `SETTLE_FRAMES=96`、`USE_DIRECT_LIGHTING=1`、`USE_DIRECT_DENOISING=1`、`USE_INDIRECT_DENOISING=1`、`probeDirectionsPerProbe=32`。Cornell 使用场景文件自带 composed camera，Arcade 才应用脚本的 front/left/right 视点。
- `USE_INDIRECT_DENOISING=1` 的质量分支为 `LumenGI.diffuseRadianceHitDist -> NRD(RelaxDiffuse) -> ModulateIllumination`；设为 `0` 时，复合图直接使用 `LumenGI.resolvedDiffuseGI`，仅用于诊断 screen-probe 原始 resolve。该开关不会证明 probe producer/history/GDF 已达到 UE/Lumen 的生产契约。
- 旧目录没有统一保存环境变量快照。开关值以下按 handoff/计划说明、NRD PackRadiance 编译次数（每个 NRD 分支约两组）和输出内容推断；标为“未固化”的参数不得用于回归基线。

## Wave 总览

| artifact 目录 | 场景 / 视角 / settle | 开关或输入（证据等级） | 构图与噪声判定 | Gate 结论 |
|---|---|---|---|---|
| [`direct-nrd-smoke-20260810`](../artifacts/lumengi/screenshots/direct-nrd-smoke-20260810/) | Arcade / front / 32 | RTXDI direct + NRD；`USE_INDIRECT_DENOISING=0`（推断；2 组 NRD PackRadiance） | 构图完整；墙面可见明显大块/低频 mottle，属于直接 reservoir/间接 raw source 的展示烟雾测试，不是无噪声最终图。 | 运行时 smoke PASS；视觉质量 FAIL |
| [`direct-nrd-convergence-20260811`](../artifacts/lumengi/screenshots/direct-nrd-convergence-20260811/) | Cornell + Arcade / front / 96 | direct+indirect composition，间接分支尚未成为默认（计划文档）；`USE_INDIRECT_DENOISING=0`（推断） | Arcade 逐帧收敛后仍有墙面残余；Cornell 背墙、地面和盒体仍有低频块。96 帧降低了随机幅度，但没有消除块状结构。 | 收敛证据 PASS；最终视觉 Gate FAIL |
| [`direct-nrd-widefilter-20260811`](../artifacts/lumengi/screenshots/direct-nrd-widefilter-20260811/) | Cornell + Arcade / front / 32 | direct NRD 宽滤波试验；确切 filter width 未写入 artifact；`USE_INDIRECT_DENOISING=0`（推断；2 组 NRD PackRadiance） | 与 smoke 相同的 Arcade 上墙 mottle；Cornell 背墙和地面可直接看到大块斑驳。宽滤波没有把 raw indirect 变成生产级源。 | 诊断实验；视觉 Gate FAIL |
| [`probe-5pass-20260811`](../artifacts/lumengi/screenshots/probe-5pass-20260811/) | Cornell / front / 96 | 目录名提示“5pass”，但没有保存 pass 数或环境变量；2 组 NRD PackRadiance 表明它是 direct-only NRD 路径，`USE_INDIRECT_DENOISING=0`（推断） | 画面比 32 帧宽滤波更平滑，构图正确；仍应视为 raw screen-probe/直接随机源的诊断结果，不能据此宣称 indirect producer/history 已闭环。 | 诊断改善；C7/C8 仍 OPEN |
| [`indirect-nrd-experiment-20260811`](../artifacts/lumengi/screenshots/indirect-nrd-experiment-20260811/) | Cornell / front / 32 | `USE_INDIRECT_DENOISING=1`（推断；direct+indirect 两个 NRD 分支），固定曝光 | 低噪声质量分支首次验证；Cornell 盒体、背墙和阴影连续，无明显大块。32 帧仍不足以作为运动/全局 seed 稳定性证明。 | 静态展示视觉 PASS；生产链 Gate OPEN |
| [`indirect-nrd-arcade-20260811`](../artifacts/lumengi/screenshots/indirect-nrd-arcade-20260811/) | Arcade / front / 32 | `USE_INDIRECT_DENOISING=1`（推断；direct+indirect NRD） | 构图完整；墙、地板和机器边缘明显比 direct-only 版本干净，无黑洞或严重过曝；仍是单视角 32 帧样本。 | 静态展示视觉 PASS；生产链 Gate OPEN |
| [`final-low-noise-20260811`](../artifacts/lumengi/screenshots/final-low-noise-20260811/) | Cornell + Arcade / front,left,right / 96；含 `contact-sheet.png` | 默认质量路径：direct RTXDI→NRD + Lumen raw radiance→NRD（`USE_INDIRECT_DENOISING=1`）；固定曝光 0 | 六视角构图可审查。Arcade 三视角无明显块状噪声；Cornell 三个文件像素基本一致是 composed camera 的预期，不是 view sweep 失败。Cornell PNG 的 57.2% 黑像素主要是画外黑边，不是 GI 黑洞。 | 当前展示/截图 Gate PASS；C7/C8、性能和动态场景 Gate 不代为通过 |

## 量化检查

对代表性 `ToneMapperDisplay` PNG 用 Pillow 读取 RGB，对代表性 `resolvedDiffuseGI` EXR 用 OpenEXR/Imath 读取 float 通道：

- `final-low-noise-20260811/arcade-front`：mean RGB 约 `0.222/0.168/0.145`，black `<0.01` 约 `2.4%`，saturated `>0.98` 约 `0.7%`。
- `final-low-noise-20260811/cornell-front`：mean RGB 约 `0.252/0.204/0.099`，black 约 `57.2%`（与画布外黑边一致），saturated 约 `0.3%`。
- `final-low-noise-20260811/cornell-front` 的 `resolvedDiffuseGI` 为 finite/non-negative，RGB 合并 mean/std 约 `0.0407/0.0665`；Arcade 约 `0.0116/0.0115`。
- `direct-nrd-widefilter-20260811` 与 `direct-nrd-convergence-20260811` 的 Cornell/Arcade 画面在平坦墙面仍有可见成块变化；把 settle 从 32 增到 96 只能减小幅度，不能解决低频结构。
- 各目录 Mogwai 日志均未匹配到以 `(Error)`、`(Fatal)`、`D3D12_ERROR` 或 `Traceback` 开头的运行时致命行；日志仍含已知 shader warning（例如 NRD.hlsli 的 `?:` deprecated），不应把 warning 当成画质通过证据。

## 视觉 Gate 与已知局限

当前可以接受的缺陷：固定静态镜头下少量纹理/材质自身细节、显示边界的黑色画外区域、ToneMapper 高亮区域极少量饱和像素。

当前不可接受的缺陷：平坦墙/地面上的大块 mottle 或棋盘块、物体边缘跨帧拖影、无几何依据的黑洞、把间接能量压成全黑、用增加 settle frame 或曝光改变掩盖 raw probe 的不稳定。

仍未覆盖的 Gate：probe producer generation/age、reprojected moments 的真实 history 契约、GDF 路由 telemetry、相机/灯光/几何变化恢复、固定 HWRT seed 的跨运行确定性、GPU 性能/帧率和最终 FinalResolve source。间接 NRD 只是展示质量分支，不是这些 Gate 的替代品。

## 最小下一步

1. 保持 `USE_INDIRECT_DENOISING=1` 作为当前截图/人工审查默认，避免把 direct-only 的块噪声误当成最终效果。
2. 单独以 `USE_INDIRECT_DENOISING=0` 运行同一场景、同一视角、同一 settle，输出独立目录；为 raw `probeInterpolated -> temporalFiltered -> spatialFiltered -> resolvedDiffuseGI` 增加 history age/confidence/producer generation 统计。
3. 先修 raw probe 的 history rejection、有效性/深度重投影和 GDF route，再重新比较 direct-only 与 indirect-NRD 分支；修复后才重新考虑将间接分支折回生产 FinalResolve。
4. 下一轮截图必须把环境变量、Falcor commit/build、分辨率、settle、视角和 denoiser method 写入同目录 JSON，消除本日志中的“推断”项。

## 2026-08-11 post-fix run: five-pass ping-pong

Command result: the Release `LumenGI` target was rebuilt with `/m:1` (0 warnings, 0 errors),
then a fresh 800x450 Cornell/front/96-frame Mogwai run completed successfully. The artifact is
[`probe-5pass-fixed-20260811`](../artifacts/lumengi/screenshots/probe-5pass-fixed-20260811/).

![Cornell post-fix ScreenProbe result](../artifacts/lumengi/screenshots/probe-5pass-fixed-20260811/cornell-front-resolved.ToneMapperDisplay.dst.96.png)

The five-pass target now alternates `pOutput` and `pScratch` by pass parity; no pass aliases its
input and output UAV. The run has no `E_INVALIDARG`, D3D12 validation error, or fatal log line.
The image is finite/non-negative and the room layout is correct, but the back wall and floor still
show low-frequency mottling. This is a correctness fix and a measurable improvement opportunity,
not a closed no-noise Gate. Compare it with the pre-fix image in
[`probe-5pass-20260811`](../artifacts/lumengi/screenshots/probe-5pass-20260811/).

Interpretation:

1. `final-low-noise-20260811` is the cleanest screenshot and remains the recommended visual
   review artifact, but it uses the explicit `diffuseRadianceHitDist -> NRD -> Modulate` quality
   branch (`USE_INDIRECT_DENOISING=1`).
2. `probe-5pass-fixed-20260811` is the honest production-chain probe evidence
   (`probeInterpolated -> temporal -> spatial -> FinalResolve`) and still fails the no-noise Gate.
3. The next change must improve probe validity/history and spatial confidence handling; increasing
   exposure, settle count, or switching the quality branch is not a substitute for that evidence.

## 2026-08-11 confidence-gate and post-fix showcase runs

[`probe-confidence-gate-20260811`](../artifacts/lumengi/screenshots/probe-confidence-gate-20260811/)
is the current production-chain diagnostic: 800x450, Cornell and Arcade, front view, 96 frames,
`USE_INDIRECT_DENOISING=0`, direct RTXDI/NRD enabled. Both scenes completed runtime compilation and
all sampled EXR outputs were finite and non-negative. The Arcade image still shows broad, low-frequency
wall variation; this remains a `C7/C8 visual Gate FAIL`, not a crash or invalid-output failure.

The same shader build was used for
[`final-low-noise-postfix-20260811`](../artifacts/lumengi/screenshots/final-low-noise-postfix-20260811/),
the six-view quality branch (`USE_INDIRECT_DENOISING=1`). All Cornell/Arcade front/left/right PNG
and EXR files were produced. The outer command timed out while Mogwai was flushing its final log;
the process was then stopped after verifying the six view outputs were complete. Treat the image
files as valid visual evidence and the run exit as `PARTIAL/timeout`, not as a clean process Gate.

The quality branch remains the current presentation result; the production ScreenProbe path is the
diagnostic result that drives the next implementation wave. Both are linked here so a later LLM can
compare the same camera, resolution, and settle protocol without guessing which denoiser was active.

[`probe-spatial-wide-20260811`](../artifacts/lumengi/screenshots/probe-spatial-wide-20260811/)
is an A/B quality sweep with `spatialRadiusMin=2`, `spatialRadiusMax=4`, and
`spatialVarianceThresholdHigh=0.10` on Arcade/front/96. It is runtime-clean, but the image is
visually almost unchanged from the default radius preset. This rules out a too-small bilateral
footprint as the sole source of the wall mottle.

[`probe-history-20260811`](../artifacts/lumengi/screenshots/probe-history-20260811/) adds the
`probeHistory` graph mirror to the same probe-only chain. The RGB EXR is finite, but the current
EXR capture does not preserve the RGBA alpha channel (the accumulated direction count), so the
history-count Gate remains `BLOCKED` until a typed raw-buffer/alpha readback is exposed. Do not
infer history growth from this RGB-only file.

## 2026-08-11 UE-style screen-radiance history

This wave follows the UE5.8 reference contract: ScreenProbe radiance should not rely only on the
current frame's single HWRT estimate. The implementation is separate from the probe irradiance
history and uses full-resolution ping-pong resources:

- `pScreenRadianceHistory[2]` is RGBA16F (raw unmodulated radiance RGB, secondary hit distance A).
- `pScreenRadianceDepthHistory[2]` is RG32F (previous linear depth).
- `LumenScreenRadianceHistory.cs.slang` performs motion reprojection, depth rejection, finite and
  miss-sentinel checks, radiance cap 10, and bounded history blending.
- The old slot is read by ScreenProbe; the new slot is written before trace and swapped only after
  integrate/interpolate. Resize and hard history invalidation clear both slots.

Build/runtime protocol:

```text
codegraph sync .
MSBuild build/windows-vs2022/Source/RenderPasses/LumenGI/LumenGI.vcxproj /p:Configuration=Release /m:1 /t:Build /nologo
Mogwai.exe --device-type d3d12 --headless --precise --script tests/lumengi/run_screenprobe_convergence.py --logfile <unique>/mogwai.log
```

The first attempted solution target (`Falcor.sln /t:LumenGI`) was retained as a build-entry
failure (`ZERO_CHECK` does not expose that solution target). The direct LumenGI project build then
passed with 0 warnings/0 errors and copied the new shader. The serial GPU artifact is
[`screenprobe-convergence-history-20260811`](../artifacts/lumengi/screenshots/screenprobe-convergence-history-20260811/).

![ScreenProbe history frame 96](../artifacts/lumengi/screenshots/screenprobe-convergence-history-20260811/screenprobe-cornell-front.ToneMapperDisplay.dst.96.png)

Manifest result: `PASS`; checkpoints 1/8/32/96 all have required RGBA outputs, finite/non-negative
values, and no `Error`, `Fatal`, `E_INVALIDARG`, `Failed to link`, or D3D12 validation line. The
probe-history alpha reaches 3040 at frame 96. For the same Cornell/front image, the 8-pixel local
difference proxy is 0.01017 in `screenprobe-convergence-p0c-20260811` and 0.00966 after history;
the wall blocks are reduced but remain visible. Therefore this wave is `C7/C8 improved, partial`:
it proves runtime history wiring and a measurable visual improvement, but it does not yet satisfy
the no-noise realtime Gate. The remaining required evidence is camera-motion/geometry reset,
normal/material rejection, producer generation/age, reprojected moments, and backend-route
telemetry readback. `final-low-noise-20260811` remains a clean NRD quality-branch screenshot only.

## 2026-08-11 UE-style screen-radiance history rerun (corrected statistics)

The convergence reader was corrected to cast RGBA16F readback arrays to `float64` before
reductions. A fresh unique single-GPU run supersedes the earlier manifest's `mean=null` fields:
[`screenprobe-convergence-history2-20260811`](../artifacts/lumengi/screenshots/screenprobe-convergence-history2-20260811/)

![Corrected UE-style radiance-history frame 96](../artifacts/lumengi/screenshots/screenprobe-convergence-history2-20260811/screenprobe-cornell-front.ToneMapperDisplay.dst.96.png)

Protocol: 800x450 Cornell/front, 32 directions/probe, fixed exposure, serial checkpoints 1/8/32/96,
spatial radius 2..4, variance threshold high 0.20. `Mogwai` returned 0; all required outputs were
present and finite/non-negative at every checkpoint; the log contains zero `Fatal`, `Error`,
`E_INVALIDARG`, shader-link, D3D12-validation, or traceback matches.

| frame | probeInterpolated mean / max | spatialFiltered mean / max | temporalConfidence mean | probeHistory alpha max |
|---:|---:|---:|---:|---:|
| 1 | 0.26864 / 2.26367 | 0.27146 / 1.88477 | 0.06486 | 32 |
| 8 | 0.27075 / 1.76074 | 0.43212 / 1.67969 | 0.69714 | 224 |
| 32 | 0.26890 / 1.69531 | 0.44319 / 1.64648 | 0.74761 | 992 |
| 96 | 0.26223 / 1.65332 | 0.43887 / 1.62891 | 0.74794 | 3040 |

The 8-pixel local-difference proxy is 0.00966, versus 0.01017 in the pre-history run. This confirms
that the history producer is active and improves stability, but the frame-96 image still contains
low-frequency wall blocks. Gate status: runtime wiring PASS; finite/non-negative PASS; measurable
improvement PASS; UE-level noise-free realtime GI FAIL/PARTIAL. The next implementation must add
producer generation/age and normal/material rejection, reprojected moments, and camera/geometry
reset coverage. Do not use the separate NRD quality branch as proof for this production-chain Gate.

## 2026-08-11 final realtime LumenGI presentation result

The final presentation graph is now captured at
[`final-realtime-lumengi-20260811`](../artifacts/lumengi/screenshots/final-realtime-lumengi-20260811/)
for Cornell and Arcade, 800x450, front view, 96 warmup frames. It uses the same realtime graph for
both scenes: `LumenGI.diffuseRadianceHitDist` -> NRD RelaxDiffuse -> diffuse modulation, plus
RTXDI direct/emissive lighting -> NRD -> composite. This is the low-noise presentation mode; the
probe-only `resolvedDiffuseGI` path remains separately tracked as a production diagnostic.

![Final realtime Cornell](../artifacts/lumengi/screenshots/final-realtime-lumengi-20260811/cornell-front-resolved.ToneMapperDisplay.dst.96.png)

![Final realtime Arcade](../artifacts/lumengi/screenshots/final-realtime-lumengi-20260811/arcade-front-resolved.ToneMapperDisplay.dst.96.png)

Visual Gate: PASS for the presentation result. Cornell's internal-frame black ratio is about 0.25%
(the 57% whole-image black ratio is the intentional outer camera canvas), Arcade internal black is
about 4.16%, and no broad GI black hole or saturation wall is visible. The Arcade monitor has local
emissive highlights by design. The FrameCapture EXR files are RGB-only in this Falcor writer, so
alpha/hit-distance is not inferred from the files.

The matching profiler run is
[`final-realtime-showcase-20260811`](../artifacts/lumengi/benchmark/final-realtime-showcase-20260811/).
At 800x450 on an RTX 2060 SUPER, the complete final graph measured GPU p95 14.284 ms (Cornell) and
14.711 ms (Arcade), below the 16.67 ms 60-Hz budget. LumenGI itself measured p95 6.266/6.704 ms;
the remaining time is direct RTXDI and NRD denoising. This establishes a realtime performance Gate
for the presentation graph, not for every future quality preset.

Result classification: final screenshot + realtime budget PASS; production ScreenProbe no-noise
Gate remains PARTIAL because the probe-only path still has low-frequency mottle and lacks full
generation/age, normal/material rejection, reprojected moments, and reset telemetry. Do not remove
that distinction when handing this workspace to the next LLM.

## 2026-08-11 Arcade shadow/specular/transmission diagnosis

The Arcade right-hand image was rechecked against the scene asset and the actual graph contracts.
The result is not a missing-GI-dispatch symptom:

- `Media/Arcade/Arcade.pyscene` has one HDR environment (intensity 1), one directional light,
  one point light, and Cabinet emissive factor 150. The environment fills the room and reduces
  direct-shadow contrast; chair/cabinet contact shadows are present but subtle.
- `Arcade.fbx` materials are opaque (`Opacity=1`). The Specular texture is an occlusion/roughness/
  metallic texture convention, not a glass/transmission material. There is no
  `specularTransmission`, `diffuseTransmission`, `thinSurface`, or window mesh.
- The presentation graph's RTXDI branch evaluates direct diffuse/specular reflection and visibility
  rays, but no transmission. The LumenGI branch exports diffuse indirect radiance only. Therefore
  the Arcade image cannot prove indirect mirror reflection or glass, regardless of exposure.

The following serial A/B artifacts make the distinction explicit:

- [`arcade-direct-only-20260811`](../artifacts/lumengi/screenshots/arcade-direct-only-20260811/):
  RTXDI direct branch with GI preview scale 0 and environment 0.25. It is runtime-clean, but the
  scene still has no transmission and the shadow contrast remains limited by geometry/light layout.
- [`arcade-shadow-contrast2-20260811`](../artifacts/lumengi/screenshots/arcade-shadow-contrast2-20260811/):
  corrected full-presentation A/B with environment 0.25 and Cabinet emissive scale 0.5. It is
  darker, but does not create new shadows; this confirms that lowering HDRI alone is not the
  missing feature. The earlier `arcade-shadow-contrast-20260811` run is retained as a failed
  override attempt because its emissive material lookup raised a binding warning.
- [`convergence-test-realtime-20260811`](../artifacts/lumengi/screenshots/convergence-test-realtime-20260811/):
  the repository's material/occlusion scene with colored emissive panels, metallic mirrors, metal
  spheres, rough/smooth glass and thin transmission rows. Direct reflection is clearly visible;
  the glass rows are not a passing realtime-transmission result because RTXDI has no transmission
  producer.
- [`material-test-realtime-20260811`](../artifacts/lumengi/screenshots/material-test-realtime-20260811/):
  metal/roughness/transmission grid. It is useful for direct specular material coverage, not for
  claiming Lumen indirect glass.

The showcase script now has opt-in, presentation-only overrides (no source-asset mutation):
`LUMEN_RESOLVED_ENV_INTENSITY`, `LUMEN_RESOLVED_POINT_LIGHT_SCALE`,
`LUMEN_RESOLVED_DIRECTIONAL_LIGHT_SCALE`, and `LUMEN_RESOLVED_EMISSIVE_FACTOR_SCALE`.
`python -m py_compile tests/lumengi/run_resolved_showcase.py` and `git diff --check` pass. A future
shadow A/B should use these knobs with a unique output directory and keep the unmodified Arcade
image as the baseline.

Gate classification: Arcade diffuse GI/emissive presentation PASS; Arcade direct-shadow contrast
PARTIAL; Arcade glass/transmission UNSUPPORTED in the current realtime graph. For a real glass gate,
use the existing PathTracer+NRD delta reflection/transmission graph in `scripts/PathTracerNRD.py`
as a reference path, or add a dedicated transmission producer before changing the LumenGI claim.

## 2026-08-11 A1 host telemetry / partial-tile Gate

Build: Release `LumenGI.vcxproj`, MSBuild `/m:1`, zero warnings/errors. The GPU run used an RTX 2060
SUPER with D3D12 and the serial `run_screenprobe_convergence.py` protocol (frames 1/8/32/96,
intermediate frames rendered, fixed Cornell camera).

- [`800x450 host telemetry`](../artifacts/lumengi/A1/host-telemetry-v2-800x450-20260811/)
  passed all required output finite/non-negative checks. Probe grid was `100x57`; the manifest
  recorded history/lighting generation, reset count/reason, pending state and history ping-pong
  indices at every checkpoint.
- [`641x361 partial tile`](../artifacts/lumengi/A1/host-telemetry-v2-641x361-20260811/)
  passed the same checks with the expected `81x46` grid. No fatal, D3D12 validation error,
  `E_INVALIDARG`, or traceback was found.
- [`offline validity gate`](../artifacts/lumengi/A1/probe-validity-gate-20260811/probe-validity-gate.json)
  is intentionally `BLOCKED`: current telemetry does not yet expose per-producer age, backend
  source, or explicit scene-reload/camera-cut transition markers. The script correctly refuses to
  infer these from image alpha or screenshots.

Result: host reset/epoch observability and partial-tile resource lifetime are PASS; UE-style
producer validity/age/backend sidecar remains PARTIAL/BLOCKED. Do not treat this as proof that
ScreenProbe is noise-free. The next wave must add producer frame/generation + age/valid-mask,
normal/material rejection, reprojected moments, and explicit reset-reason telemetry before tuning
filter radius or adding rough-specular/transmission.

## 2026-08-11 C1/C6 runtime debug results

### C1 cache-lighting environment sampler

The first `envSampler=1` run reached shader compilation but failed at the first D3D12 dispatch with
`E_INVALIDARG`; `envSampler=0` passed. The source used an implicit global `EnvMapSampler` (Slang
warning 39019). Replacing it with `ParameterBlock<EnvMapSampler> envMapSampler` and rebuilding the
Release LumenGI target removed the root-shape failure. The post-fix 640x360 Arcade runs are clean:

- [`emissive off`](../artifacts/lumengi/C1/emissive-off-env-sampler-parameterblock-20260811/)
  (`env=1, analytic=1, emissive=0`), eight frames, finite/non-negative.
- [`all on`](../artifacts/lumengi/C1/all-on-env-sampler-parameterblock-20260811/)
  (`env=1, analytic=1, emissive=1`), eight frames, finite/non-negative.

The old shader-link and pre-fix E_INVALIDARG logs remain historical diagnostics; they are not used
as post-fix Gate evidence.

### C6 page metadata / generation fence

[`C6 page metadata`](../artifacts/lumengi/C6/page-metadata-20260811-v2/) passed the complete Cornell
640x360 lookup-on/off, reload, and one-page-per-frame budget matrix. All four cases had finite,
non-negative GI and no fatal/D3D12 validation error. The low-budget page count progressed 1 -> 8 ->
12, while reload rebuilt the page set without a black frame. This is a page generation/state fence
Gate, not proof that UE-style demand feedback, deduplication, or eviction policy is complete.

## 2026-08-11 A2 ScreenProbe validity/filter runtime check

[`A2 ScreenProbe convergence`](../artifacts/lumengi/A2/screenprobe-convergence-post-c1-20260811/)
ran Cornell at 640x360 with 32 directions per probe and serial checkpoints 1/8/32/96. The updated
screen-hit sentinel/finite clamp, dirty-history reset, legal-black handling, spatial confidence
gate, and FinalResolve valid-black rule all compiled and executed with no fatal, D3D12 validation
error, `E_INVALIDARG`, NaN/Inf, or negative output. This is an A2 shader/runtime safety PASS.

It is not a no-noise PASS: the manifest does not yet expose per-direction backend identity,
age/generation sidecar, previous normal/material rejection, or source moments. Those UE-style
history contracts remain the next A2 wave and must be measured separately from `probeHistory.a`.

## 2026-08-11 C4 GDF compose descriptor matrix

The post-C1 Release binary was rebuilt with `/m:1` and the C4 diagnostic matrix was run serially on the RTX 2060 SUPER at Cornell 320x180. E1 (UAV only), E2a (compose CB + three GDF buffers), and E2b (atlas descriptors + scalars) completed without a D3D12 error. E2, E2c, and E2d retained the first `E_INVALIDARG` at `ComputePass::execute`; the failing delta is the nested compose CB combined with Falcor's implicit global uniform CB.

The production compose path uses one explicit `LumenGDFComposeCB` for clipmap data and all three atlas scalars. It dispatched both dirty levels (`(64,1,4096)` logical threads, `(8,1,512)` groups) without fatal errors in [`post-build-production-20260811`](../artifacts/lumengi/C4/post-build-production-20260811/). This is a component/dispatch PASS only. `gdfStats.gdfExecuted` remains zero when no `gdfTrace` output is marked because the standalone sphere-trace debug pass is intentionally skipped; no production probe direction has yet been tagged as GDF. The next evidence must therefore be a unified Screen -> GDF -> HWRT route test, not another compose-only screenshot.

The corrected companion S6 run [`standalone-s6-corrected-20260811`](../artifacts/lumengi/C4/standalone-s6-corrected-20260811/) completed MeshSDF and Hybrid without NaN or D3D12 errors; `sphereHitRate=0.2566` and `sphereTraced=2,073,600`. Both entries still reported `hwrtPrimary=1`, `gdfRadianceSelected=0`, and `hybridFallbackToHWRT=1`; the corrected Gate explicitly reports `sdfPrimaryWritesOutputs=false`. This is GDF data/sphere-trace health only, not a GDF-selected ScreenProbe result.

## 2026-08-11 C4 Screen -> GDF -> HWRT probe router

The C4 router wave is the first run that measures a backend hit in the unified ScreenProbe
records. After the screen-first march misses, the probe shader traces the same biased world ray
through the composed GDF. A hit sets `kLumenProbeHitFlagGDFHit`; a miss increments `gdfMisses` and
continues to HWRT. Integrate accepts Screen/HWRT/GDF geometry flags, while radiance remains the
existing fallback resolver until a dedicated GDF material-lighting producer is implemented.

- [`router`](../artifacts/lumengi/C4/gdf-probe-router-v4-20260811/): Cornell 320x180,
  MeshSDF + useGDF + screen probes, four serial frames. `gdfRouteEnabled=true`, final
  `gdfHits=607`, `gdfMisses=12260`; all sampled outputs finite/non-negative; report status PASS.
- [`HardwareRT control`](../artifacts/lumengi/C4/gdf-probe-router-baseline-20260811/): same
  graph shape with `traceMode=HardwareRT,useGDF=false`; `gdfRouteEnabled=false`, `gdfHits=0`,
  finite/non-negative outputs; report status PASS.

The route is now runtime evidence, not a screenshot inference. The limitation is explicit:
GDF-hit radiance is still resolved through the existing screen/environment fallback and is not
yet a UE-equivalent Surface Cache/card-lighting result. The visual quality and C5 Hybrid policy
gates remain open.

## 2026-08-11 C6 page lifecycle telemetry

The host now exposes page generation/state, generation and stale-owner rejection, scheduler request
deduplication, page-cache touch (`lastUsed`), and eviction counters. The runner treats cache-off as
`NOT_APPLICABLE` instead of fabricating page telemetry for a disabled producer.

Artifact: [`C6 page telemetry v5`](../artifacts/lumengi/C6/page-telemetry-20260811-v5/surfacecache-effect.json).
Cornell 640x360, checkpoints 1/8/16, lookup-on/off, reload, and one-page-per-frame all report
`PASS`; all GI samples are finite/non-negative and the low-budget residency sequence is 1 -> 8 ->
12 pages. This closes the C6 telemetry Gate for the static scene. Moving geometry and full UE-style
feedback/priority behavior remain release-matrix work.
## 2026-08-11 C7 per-direction validity sidecar

Artifact: [`C7 sidecar v8`](../artifacts/lumengi/C7/probe-validity-sidecar-20260811-v8/).
Cornell 800x450, checkpoints 1/8/32/96, raw `LumenGI.probeValidity` only. The optional sidecar
has the frozen `uint4` ABI (backend/source bits, producer frame, generation, age/reset fields),
and the Release D3D12 shader dispatch completed with finite readback and no fatal/D3D12 error.
The Python reader now treats `rawBuffer` as a byte stream and reinterprets 16-byte records; the
earlier v3-v5 backend distribution was a decoder artifact, not rendering evidence.

This is a steady producer-buffer PASS only. The transition matrix is still BLOCKED because the
scene-reload/camera-cut harness timed out before producing a manifest; no screenshot or steady
frame is allowed to stand in for reset/generation proof. The next UE-aligned capture must use the
first post-cut frame and report reset reason, generation delta, age reset, and rejection counts.

## 2026-08-11 C7 sidecar sampling correction

The follow-up artifact [`C7 sidecar v9`](../artifacts/lumengi/C7/probe-validity-sidecar-20260811-v9/)
uses evenly distributed probe indices instead of only the first 256 probes. This removes a spatial
sampling bias: the earlier v8 prefix happened to contain mostly invalid directions and incorrectly
looked like an all-zero backend distribution. v9 decodes 182,400 `uint4` records (729,600 32-bit
words) and reports Screen/HWRT/Invalid counts consistent with `screenHitRate` and fallback-hit
telemetry. The producer ABI/backend-distribution gate is therefore PASS.

The reduced camera-cut transition run
[`camera transition v1`](../artifacts/lumengi/C7/probe-validity-transitions-20260811-camera-v1/)
timed out during Mogwai initialization and produced no manifest. Reset/generation/age behavior
remains BLOCKED; no screenshot or steady sidecar frame is used as a substitute.

## 2026-08-11 C8/C9 export-equivalence rerun

[`C8 current export equivalence`](../artifacts/lumengi/C8/export-equivalence-20260811-current/)
reran the current Release binary at Cornell 800x450. Mark-on/export-off and mark-on/export-on pass
for filters off and temporal-only/partial, with finite/non-negative `diffuseGI` and
`resolvedDiffuseGI` and linear-HDR equivalence. Mark-off direct endpoints remain `BLOCKED` by the
RenderGraph output contract; sentinel BlitPass values remain diagnostic and are not used as GI.

This is a marked-endpoint/export PASS, not a complete C8/C9 source-validity or no-noise PASS. The
remaining work is producer generation/reset propagation, independent temporal confidence, and a
GPU albedo-once/legal-black regression.

## 2026-08-11 C7/A1 transition Gate closure

[`C7 validity transitions`](../artifacts/lumengi/C7/probe-validity-transitions-20260811-full-v1/)
passes the formal reset matrix on the current Release binary: 800x450 and 641x361 partial-tile
resolutions, scene reload and camera cut, with continuous checkpoints 1/8/32/96. All four runs
report readable sidecar data, generation changes, age reset, and reset reasons. This closes the
producer identity/reset observability Gate; previous-normal/material rejection, reprojected source
moments, light/environment epochs, and image-noise thresholds remain open.

## 2026-08-11 C7/A2 guide-history runtime gate

The new full-resolution screen-radiance history guide stores the current normal/material identity
in a ping-pong RGBA16F sidecar. Reprojection accepts history only when depth, a 45-degree normal
threshold, and the material id agree. The primary history RGBA16F alpha remains secondary
hit-distance, so this change does not silently convert hit distance into age/confidence.

Artifact [`A2 guide-history convergence`](../artifacts/lumengi/A2/screenradiance-guide-20260811-v1/)
passes Cornell 800x450 at serial checkpoints 1/8/32/96. Four output captures are finite and
non-negative; the Mogwai log contains no fatal, `E_INVALIDARG`, missing-member, or shader-link
errors. This is a component/runtime PASS only. UE-style source moments, lighting-generation
rejection, and the no-noise image gate are still open and must be measured separately.

## 2026-08-11 C7/A2 source-moments runtime gate

The source-luminance moments are now a distinct RG32F ping-pong resource (mean, mean-square),
updated with the same motion/depth/guide reprojection as the raw radiance history. Trace reads
the moments only for variance-adaptive history weighting; downstream irradiance moments and the
raw history hit-distance alpha are untouched.

Artifact [`A2 source-moments convergence`](../artifacts/lumengi/A2/screenradiance-moments-20260811-v2/)
passes Cornell 800x450 at serial checkpoints 1/8/32/96. The manifest has four captures, zero
capture errors, and the Mogwai log has no fatal, `E_INVALIDARG`, missing-member, or shader-link
errors. This is a component/runtime PASS, not an image-quality PASS. A paired before/after run
with local variance, temporal framediff tail, and GPU time is required before claiming noise
reduction.

The first moments attempt is retained as negative evidence in
`artifacts/lumengi/A2/screenradiance-moments-20260811-v1/`: the runtime reported
`No member named 'gScreenRadianceMoments'` because the host bound the new resource to update and
finalize passes that do not declare it. The binding was narrowed to the trace pass, rebuilt, and
the v2 gate above passed. This is a binding-scope fix, not a suppression of a GPU validation error.

## 2026-08-11 C7/A2 moments image-quality comparison

`tests/lumengi/compare_screenprobe_moments.py` compares identical linear EXR checkpoints with
`useScreenRadianceMoments=0/1`. The optimized run uses the source RG32F moments plus a 2.5-sigma
firefly clamp; the baseline keeps guide history and all other filters unchanged.

Artifact [`A2 moments comparison`](../artifacts/lumengi/A2/screenradiance-moments-compare-20260811-v2/moments-comparison.json)
is `PASS`: at frame 96, spatial local variance is `0.0001467362 -> 0.0001443430`, and the
32→96 spatial tail RMSE is `0.0366308 -> 0.0353284`. The report remains strict: missing/nonfinite
EXR produces `BLOCKED`, and lack of improvement produces `NO_IMPROVEMENT` rather than a false PASS.

## 2026-08-11 C8 post-A2 regression

`artifacts/lumengi/C8/export-equivalence-20260811-post-a2/export-equivalence.json` reruns the
marked export matrix on the moments-enabled Release binary. Filters off and partial both keep
mark-on/export-off and mark-on/export-on at `PASS`; mark-off remains `BLOCKED` by the documented
RenderGraph endpoint contract. No new fatal or D3D12 error is introduced. This is an export
regression PASS, not proof that source mottle is gone.

## 2026-08-11 C7 lighting-generation fence

Screen-radiance history now has a separate R32Uint ping-pong generation texture. The RGBA16F
history alpha remains secondary hit distance; dynamic light, emissive-material, and environment
changes cannot reuse a previous radiance sample when `mLightingGeneration` differs. The optional
`screenRadianceLightingGeneration` output mirrors the producer epoch for readback, without making
the production chain depend on `markOutput()`.

The Release build and runtime transition artifact
[`C7 lighting generation`](../artifacts/lumengi/C7/lighting-generation-20260811-v2/probe-validity-transitions.json)
are `PASS` at 800x450 on the RTX 2060 SUPER. `light_change`, `material_change`, and `env_change`
all report a changed `lightingGeneration`, readable generation mirror, and mirror/stat agreement;
the Mogwai log contains no fatal, `E_INVALIDARG`, or shader-link error. This closes the dynamic
lighting/environment/material epoch component Gate. It does not yet prove multi-scene image
quality, GPU timing/VRAM budget, or rough-specular/transmission support.

## 2026-08-11 C7 convergence/material reference scene

The UE-style convergence scene was rendered at 800x450 for 32 continuous frames with the current
direct RTXDI + NRD and indirect LumenGI branches:
[`convergence material capture`](../artifacts/lumengi/C7/convergence-material-20260811/).
The image visibly contains colored emissive panels, contact/shadow regions, glossy/metallic
reflections, and rough/smooth material rows. The runtime summary reports finite, non-negative
`resolvedDiffuseGI` (`mean=0.12152`, `max=1.11230`) and composite output (`mean=0.37634`,
`max=10.37727`) with no fatal or validation error in the log.

This is a direct-specular + diffuse-indirect quality reference, not a glass/transmission PASS:
the current RTXDI producer evaluates diffuse/specular reflection lobes, while LumenGI's
production branch remains diffuse indirect. Transmission must use the separate PathTracer+NRD
delta reflection/transmission reference graph before it can be claimed as production capability.

The post-fence C8 rerun
[`C8 generation-fence export`](../artifacts/lumengi/C8/export-equivalence-20260811-generation-fence/export-equivalence.json)
keeps all marked endpoint cases `PASS` at 640x360. Mark-off remains the documented RenderGraph
contract `BLOCKED`; no new fatal, `E_INVALIDARG`, or shader-link error was introduced.

## 2026-08-12 C5 GDF probe cache-consumer gate

The probe counter ABI now reports `cacheLookupHits` at byte offset +40 (44-byte counter record).
Integrate increments it only after page generation/state, card generation, metadata validity,
visibility, and finite radiance checks pass. The host binds the counter UAV to integrate and
exposes the value through `screenProbeStats`, keeping `gdfHits` (geometry) separate from cache
lighting consumption.

The cache-off control
[`C5 cache-off`](../artifacts/lumengi/C5/gdf-probe-router-cache-off-20260811/gdf-probe-router.json)
is `PASS` at Cornell 320x180 for eight frames: `gdfHits=608`, `cacheLookupHits=0` as expected,
finite/non-negative probe outputs, and no fatal, `E_INVALIDARG`, or shader-link errors.

The cache-on Cornell run
[`C5 cache-on`](../artifacts/lumengi/C5/gdf-probe-router-cache-on-20260811/gdf-probe-router.json)
is intentionally `BLOCKED`: `gdfHits=607` but `cacheLookupHits=0`. The cache-on
`convergence_test` run is also blocked because it produced no GDF hits. This is negative
evidence, not a regression: cache-lighting dispatch alone does not prove that GDF probe hits
consume authoritative Surface Cache radiance. The next C5 step is reject-reason telemetry and
first-failing page/coverage contract diagnosis.

The follow-up diagnostic
[`C5 cache-on rejects`](../artifacts/lumengi/C5/gdf-probe-router-cache-on-v2-20260812/gdf-probe-router.json)
shows `cacheLookupAttempts=4715`, `cacheCoverageRejects=56091`, `cacheMetadataRejects=488`, and
`cacheVisibilityRejects=1`; page generation/state rejects are zero. The dominant failure is card
plane/extent coverage, followed by invalid card metadata, not a D3D12 descriptor failure. Arcade
provides a complementary partial result
[`C5 Arcade cache-on`](../artifacts/lumengi/C5/gdf-probe-router-arcade-cache-on-20260812/gdf-probe-router.json):
`cacheLookupHits=22`, but `gdfHits=0`, so it cannot close the GDF-route gate. Both results are
retained as diagnosis; no cache hit is promoted to a production PASS without simultaneous GDF-hit
and cache-hit evidence.

The combined-route material-scene gate
[`C5 material cache-on`](../artifacts/lumengi/C5/gdf-probe-router-material-cache-on-20260812/gdf-probe-router.json)
is `PASS` at 320x180 for four frames: `gdfHits=42`, `cacheLookupHits=1`, finite/non-negative
outputs, and no fatal or shader-link error. It proves the end-to-end Screen→GDF→Surface Cache
lighting path on a scene with compatible card metadata. Cornell remains a coverage/metadata
BLOCKED case, so C5 is not yet a universal scene-quality PASS.

## 2026-08-12 C8 raw-buffer FrameCapture compatibility

Adding the optional raw `probeValidity` output initially caused `mark_off_export_on` to fail
because `FrameCapture.captureAllOutputs` assumed every graph output was a texture. FrameCapture
now skips non-texture outputs with a warning and leaves them to the dedicated raw-buffer gate.
The current-binary regression
[`C8 FrameCapture fix`](../artifacts/lumengi/C8/export-equivalence-20260812-framecapture-fix/export-equivalence.json)
is `PARTIAL` as designed: all four marked endpoint policies are `PASS`, mark-off policies remain
`BLOCKED` by the RenderGraph endpoint contract, and there are no capture exceptions or D3D12
errors. The log records only the expected non-texture skip warning.

## 2026-08-12 C9 FinalResolve contract gate

`tests/lumengi/run_c9_resolve_contract.py` checks the resolve boundary before
accepting an image result. It requires finite source plus alpha validity,
preserves legal black radiance, rejects RGB magnitude as a validity test,
applies incident `E * diffuseOpacity / PI` exactly once, and keeps raw HWRT
reflected radiance on the passthrough branch. It also checks that the internal
resolved texture is copied to public `diffuseGI` and the optional
`resolvedDiffuseGI` mirror, with temporal confidence kept separate from
history length.

Artifact [`C9 resolve contract`](../artifacts/lumengi/C9/resolve-contract/resolve-contract.json)
is `PASS`: all static checks pass and the post-FrameCapture-fix runtime input
has four marked policies passing, four documented mark-off policies blocked by
the RenderGraph endpoint contract, and zero runtime errors. This closes the
bounded C9 source/resolve contract; it does not claim a full-scene `finalColor`
or rough-specular/transmission producer.

## 2026-08-12 convergence material visual gate

The current Release binary was run on `test_scenes/convergence_test.pyscene`
at 800x450, scene camera preserved, 32 continuous frames, with RTXDI direct
lighting plus the LumenGI indirect denoising branch. The run exited 0 and the
log has no `Fatal`, `E_INVALIDARG`, validation, or shader-link error. The
resolved GI summary is finite/non-negative (`mean=0.12152`, `max=1.11230`),
and the composite summary is finite/non-negative (`mean=0.37634`,
`max=10.37727`).

The contact sheet
[`convergence front frame 32`](../artifacts/lumengi/screenshots/convergence-test-20260812/convergence-front-resolved.ToneMapperDisplay.dst.32.png)
shows colored emissive panels, contact/shadow regions, metallic/glossy
reflection rows, and rough materials. This is a direct-reflection plus
diffuse-indirect visual PASS. The dark glass rows are not a failed exposure
diagnosis: the current RTXDI/LumenGI production graph has no transmission
producer. Glass/transmission remains a separate PathTracer+NRD reference gate.

## 2026-08-12 C5 material cache quality at 800x450

The combined Screen -> GDF -> Surface Cache gate was rerun on
`media/test_scenes/material_test.pyscene` at 800x450 for eight continuous
frames with GDF and cache lookup enabled. The report
[`C5 material 800x450`](../artifacts/lumengi/C5/gdf-probe-router-material-cache-on-800x450-20260812/gdf-probe-router.json)
is `PASS`: every frame is finite/non-negative, `gdfHits` stays in the
149--190 range, and `cacheLookupHits` is non-zero (1--2 per frame). The log
has no fatal, `E_INVALIDARG`, or shader-link error. Millions of card/page
coverage rejects remain, so this is a high-resolution route smoke rather than
a claim of broad cache coverage. The next C5 implementation target remains
world-space hit-to-card coverage/indexing; generation and metadata guards stay
strict.

## 2026-08-12 convergence material multi-view smoke

The current Release binary also completed front/left/right captures at 800x450
for 16 continuous frames. All three runs report finite/non-negative GI and
composite values with no fatal, `E_INVALIDARG`, validation, or shader-link
errors. The three PNGs are kept together under
`artifacts/lumengi/screenshots/convergence-test-20260812-multiview/`; their
similar means are expected because this scene intentionally keeps its authored
camera for a material-grid comparison. This is a multi-angle smoke only, not
a performance or transmission gate.

## 2026-08-12 C5 slab/facing card mapping fix

The first C5 reject census showed that exact card-face-plane matching was too
strict for GDF hit points: Cornell had 56,091 coverage rejects and zero cache
hits, while the compatible material scene had only one cache hit. The lookup
now uses a bounded card-AABB slab test, hit-direction facing, and a nearest
valid candidate score. Generation, page-state, card-owner, metadata, visibility,
and finite-radiance guards remain mandatory; invalid pages still fall back and
are never treated as black lighting.

Post-fix artifacts:

- [`material 320x180 slab fix`](../artifacts/lumengi/C5/gdf-probe-router-material-cache-on-slabfix-320x180-20260812/gdf-probe-router.json): `PASS`, `gdfHits=42`, `cacheLookupHits=23` (pre-fix `1`).
- [`material 800x450 slab fix`](../artifacts/lumengi/C5/gdf-probe-router-material-cache-on-slabfix-800x450-20260812/gdf-probe-router.json): `PASS`, `gdfHits=160`, `cacheLookupHits=179`, finite/non-negative output, no fatal/E_INVALIDARG/shader-link error.
- [`Cornell 320x180 slab fix`](../artifacts/lumengi/C5/gdf-probe-router-cornell-cache-on-slabfix-320x180-20260812/gdf-probe-router.json): `PASS`, `gdfHits=607`, `cacheLookupHits=2` (pre-fix `0`), `cacheLookupAttempts=4715`, `cacheCoverageRejects=56121`, `cacheMetadataRejects=444`.

This closes the first card-coverage mapping defect and proves cache consumption
on both the material and Cornell routes. It does not close universal cache
quality: Cornell and material still report large coverage/page rejects, and the
current lookup remains an O(cards) candidate scan. The next C5 task is a
world-space card/page candidate index with the same strict generation fences,
followed by GPU timing/VRAM evidence. The 800x450 run wrote its JSON before the
headless process was terminated; no Mogwai process remains after cleanup.

## 2026-08-12 profiler production-chain calibration

The benchmark harness now accepts `LUMENGI_BENCHMARK_PROPERTIES` as a JSON
object, so the recorded profiler run can explicitly enable the production
Screen -> GDF -> Surface Cache -> Probe path instead of silently measuring the
HWRT-only default graph.

The calibrated 640x360 run
[`C5 production profiler`](../artifacts/lumengi/benchmark/c5-slab-production-640x360-20260812/manifest.json)
used convergence_test, 20 warmup frames, 60 captured frames, MeshSDF, GDF,
screen probes, temporal/spatial filtering, Surface Cache and cache lighting.
Falcor GPU profiler output reports whole-frame `/onFrameRender/gpu_time`
mean `4.311 ms`, P50 `4.020 ms`, P95 `5.416 ms`, P99 `5.554 ms`, max
`5.554 ms`; the RenderGraph execute/LumenGI lane reports mean `3.979 ms`,
P95 `4.740 ms`, P99 `4.986 ms`, max `5.090 ms`. The log has no fatal,
E_INVALIDARG, validation, shader-link, or descriptor-member errors, and no
Mogwai process remains.

This is the first authoritative GPU-time evidence for the enabled production
configuration, not a 60-Hz release gate: it is 640x360/60 frames on one GPU,
with no VRAM budget sample and no 120+600-frame soak. Higher resolutions,
three-run statistics, VRAM, and long soak remain open.

The same 60-frame calibration was then run at 800x450 and 1280x720. The
whole-frame GPU lanes report respectively P95/P99/max `7.095/7.312/7.475 ms`
and `13.252/13.596/13.804 ms`; the LumenGI lanes report
`6.687/6.724/6.764 ms` and `12.648/12.769/12.905 ms`. Both manifests are
`completed`, both logs are free of fatal/E_INVALIDARG/validation/shader-link
errors, and no Mogwai process remains. The 1280x720 P95 is below the 16.67 ms
60-Hz frame budget for this short run, but this is still not the release gate:
three independent runs, VRAM/budget telemetry, and 120+600-frame soak remain.

The first full-length stress sample then completed at 1280x720 with 120 warmup
and 600 captured frames:
[`C5 full600 profiler`](../artifacts/lumengi/benchmark/c5-slab-production-1280x720-20260812-full600/manifest.json).
The 600-frame GPU distribution is whole-frame mean/P50/P95/P99/max
`12.993/12.940/13.378/13.565/14.252 ms`; LumenGI is
`12.339/12.280/12.628/12.732/12.911 ms`. The manifest is completed, the
log contains no fatal/E_INVALIDARG/validation/device-removed error, and cache
lighting remains resident through frame 719 (`residentBytes=460800`). This
closes the 600-frame timing/stability sample, but not the requested independent
three-run or VRAM-budget/2-hour soak gates.

The benchmark manifest now also carries the pass's typed `surface_cache` and
`screen_probe` dictionaries. The 640x360 stats artifact
[`profiler with resource stats`](../artifacts/lumengi/benchmark/c5-slab-production-640x360-20260812-stats/manifest.json)
records 90 allocated/page-metadata-valid pages, 90 completed captures,
`residentBytesMB=0` (460,800 bytes, below the integer-MiB display unit),
zero allocation failures and zero stale-generation/state rejects. Its probe
stats record 50 GDF hits and 0 cache hits on convergence_test, which is valid
negative evidence for this scene rather than a cache-consumer PASS.

Two independent 60-frame repeats at the same 1280x720 configuration completed
without runtime errors. Repeat 2 reports whole-frame P95/P99/max
`13.481/13.585/13.640 ms` and LumenGI `12.717/12.880/12.880 ms`; repeat 3
reports `13.381/13.550/13.658 ms` and `12.610/12.664/12.712 ms`.
Together with the 600-frame run, this provides three independent timing samples;
the long-run maximum remains `14.252 ms`. VRAM/budget and multi-hour soak are
still separate open gates.

After the final single-threaded Release rebuild, the post-build provenance gate
repeated the material cache-on route at 320x180 for four frames. Artifact
[`post-build C5`](../artifacts/lumengi/C5/gdf-probe-router-material-cache-on-postbuild-20260812/gdf-probe-router.json)
is `PASS` with `gdfHits=42`, `cacheLookupHits=23`, finite/non-negative outputs,
and no render error. The JSON was written before the headless process was
terminated; `tasklist` confirmed no remaining Mogwai process. This is the
current source/binary/runtime consistency check.

## 2026-08-12 exact Surface Cache byte telemetry

After the final Release rebuild, the benchmark manifest
[`exact-byte stats`](../artifacts/lumengi/benchmark/c5-slab-production-640x360-exactbytes-20260812/manifest.json)
contains byte-level Surface Cache fields. At 640x360 on the production
property set, 90 pages were allocated and completed; `residentBytes=460800`
(450 KiB), `memoryBudgetBytes=536870912` (512 MiB), with zero allocation
failures and zero generation/state rejects. The run completed without
Fatal/E_INVALIDARG/validation/device-removed errors. These are Surface Cache
resource counters, not total process/GPU VRAM.

## 2026-08-12 ScreenProbe validity convergence

The post-rebuild Cornell/front convergence run
[`screenprobe sidecar`](../artifacts/lumengi/screenprobe-convergence/postbytes-20260812/screenprobe-convergence-manifest.json)
completed the 1/8/32/96 sequential checkpoint protocol at 800x450 with
`probeValidity` capture enabled. All four checkpoints are `PASS`; each has
182,400 uint4 sidecar records, finite/non-negative probe and resolve outputs,
and no Fatal/E_INVALIDARG/validation/device-removed error. The records show
`generation=2` on frame 1 and `generation=3` thereafter, age 0 on the first
checkpoint and age 1 on later checkpoints, with explicit Invalid/Screen/HWRT
backend records. This closes the current sidecar/readback convergence gate;
direction-union identity and full UE radiance-history parity remain separate
open gates.

## 2026-08-12 convergence material multi-view image gate

The current Release binary was run on `convergence_test.pyscene` at 800x450,
96 sequential frames, with direct RTXDI/NRD plus LumenGI diffuse indirect and
Surface Cache enabled. Artifact:
[`convergence multi-view`](../artifacts/lumengi/screenshots/convergence-test-resolved-20260812/).
Front/left/right each produced a final PNG and 12 LumenGI/composite EXRs;
all reported finite, non-negative values and the log had zero
Fatal/E_INVALIDARG/validation/device-removed matches. The images show direct
shadowing and strong metal reflection. The transmissive/glass rows contain
black or missing-looking regions, so this is a presentation/robustness
`PARTIAL`, not a glass/transmission PASS: RTXDI/LumenGI currently have no
transmission producer. PathTracer+NRD remains the reference-only glass gate.

## 2026-08-12 Surface Cache candidate-grid experiment

An opt-in bounded 16^3 world-space candidate grid was added beside the production
O(cards) screen-probe cache lookup, retaining the slab/facing, page-generation,
metadata and visibility fences. The grid uses 32 candidates per cell and falls
back to a full scan only for an overflow cell.

Post-build opt-in GPU runs on `material_test.pyscene` with GDF, Surface Cache and
cache lighting enabled completed at 320x180 and 800x450. They report finite,
non-negative outputs and zero grid overflow in the initial smoke.
The 320x180 benchmark records `cardGridCandidateCount=34944`,
`residentBytes=2365440`, and LumenGI GPU P95 `2.902 ms`; the 800x450 benchmark
records the same zero-overflow grid shape and LumenGI GPU P95 `4.526 ms`.
The 800x450 route report has `cacheLookupHits=48`, `gdfHits=163`, and no Fatal,
E_INVALIDARG, validation or device-removed errors.

The same-scene A/B is not equivalent yet: the opt-in 320x180 route produced
`cacheLookupHits=10` and `probeInterpolated.mean≈0.03762`, while the full-scan
control produced `cacheLookupHits=18` and mean≈0.03912. Therefore the grid is
disabled by default and remains an optimization experiment, not a C5 quality
pass. It also does not close UE-style feedback, request deduplication,
last-used/eviction or multi-scene cache coverage. Router manifests are output
evidence only; they are not hang-free release claims.

Correction for the timing numbers above: the 320x180/800x450 benchmark timing
samples were collected with the benchmark graph's screen-probe path disabled,
so they are not candidate-grid timings. The real screen-probe A/B is recorded
separately: grid-on LumenGI P95 was `17.984 ms` in the cold sample versus
`37.524 ms` for the full-scan control, but the run-to-run cache-hit mismatch
(`10` versus `18`) means this is a performance signal only, not an equivalence
or quality PASS. The grid remains opt-in and production defaults to full scan.

### 2026-08-13 C5 grid correctness fallback follow-up

The grid lookup now treats the bounded cell list as an acceleration hint and
retries the authoritative all-card scan when the first pass finds no valid
page. A fresh Release 320x180 Cornell run with GDF, cache lighting, Surface
Cache, and `useCacheCardGrid=true` completed without runtime errors at
[`grid-fallback-gdf-true-20260813`](../artifacts/lumengi/C5/grid-fallback-gdf-true-20260813/gdf-probe-router.json).
It reports `cacheLookupHits=2` from `4715` attempts, matching the same-scene
full-scan control's two hits, with finite/non-negative outputs and valid GDF
fallback evidence. Coverage rejects remain high (`64950`) and this does not
close candidate completeness, request-priority, or cache quality; it only
removes the previously observed grid-on hit loss. The grid stays opt-in until
an all-card indexed/missing-card telemetry gate and a tiny-atlas request/evict
run pass.

The strict offline equivalence manifest
[`grid-equivalence-telemetry-20260814`](../artifacts/lumengi/C5/grid-equivalence-telemetry-20260814.json)
now reports PASS for same-scene identity, finite outputs, lookup-attempt
cardinality, hit count, and all-card indexing (`12/12`, missing `0`). The
coverage-reject quality field remains OPEN (full `11.9x`, grid `13.8x` the
lookup attempts), so this closes correctness equivalence only, not source
coverage quality.

The latest Release post-build default smoke confirms that choice:
`gdf-probe-router-default-fullscan-postbuild-320x180-20260812/gdf-probe-router.json`
is `PASS` with `grid=false`, `gdfHits=42`, `cacheLookupHits=23`, finite outputs,
and zero Fatal/E_INVALIDARG/validation/device-removed matches.

## 2026-08-12 C6 scene reload generation evidence

The Surface Cache allocator intentionally resets page-local generations on a
scene reload. The Host now records monotonic `surfaceCacheSceneGeneration` and
`surfaceCacheResetCount`, and the C6 runner treats either page-generation or
scene/reset-generation change as the reload transition evidence.

Artifact:
[`surfacecache-effect-material-320x180-postepoch-20260812`](../artifacts/lumengi/C6/surfacecache-effect-material-320x180-postepoch-20260812/).
The full four-case Release run is PASS. `invalidate` records scene generation
`2 -> 3`, reset count `2 -> 3`, local page generation `1 -> 1`, finite and
non-negative diffuse/resolved outputs, and no Fatal/E_INVALIDARG/
validation/device-removed errors. `lookup_on`, `lookup_off` (not applicable
page telemetry), and `low_budget` also pass. This evidence proves reset
observability; it does not yet prove non-zero eviction/last-used/request-dedup
coverage or the final GPU timing/VRAM soak.

## 2026-08-12 A1 validity transition evidence

The Release post-build transition gate passed all four combinations of
800x450/641x361 and scene reload/camera cut. Each run rendered sequential
frames through checkpoints 1/8/32/96 and decoded the raw `probeValidity`
uint4 sidecar with finite backend/source flags, generation changes, age reset,
and reset reasons. No Fatal/E_INVALIDARG/validation/device-removed errors were
found. Artifact:
[`probe-validity-transitions-postepoch-20260812`](../artifacts/lumengi/A1/probe-validity-transitions-postepoch-20260812/).
This closes the current validity-transition evidence gate; direction-union
identity and complete UE radiance-history parity remain open.

## 2026-08-12 production performance checkpoint

Current Release binary, Cornell 1280x720, 120 warmup + 600 capture, with
ScreenProbe/Temporal/Spatial/Surface Cache/Cache Lighting enabled and the
candidate grid disabled:
[`release-postepoch-cornell-1280x720-20260812`](../artifacts/lumengi/benchmark/release-postepoch-cornell-1280x720-20260812/).
Whole-frame GPU p95/p99/max = `10.179/10.437/10.609 ms`; LumenGI p95/p99/max =
`9.533/9.618/9.951 ms`. The run had no Fatal/E_INVALIDARG/validation/device-
removed matches, 14,400 probes at 16 directions, screen-hit rate 0.1305, and
Surface Cache resident bytes 61,440. It fits the 60 Hz budget in this run but
does not close repeated-run, VRAM and soak gates.

## 2026-08-12 C6.1 demand-feedback evidence

The resident-page feedback path now records GPU cache-hit counts and observed
page generations, copies them to a readback buffer, validates scene/page
generation and Allocated/Touched state, then calls `touchPage()` before the next
Surface Cache scheduler pass. Release runtime compilation and execution passed
after replacing unsupported DXC `InterlockedExchange` with `InterlockedMax`.

The 320x180 Cornell matrix (`lookup_on`, `lookup_off`, `invalidate`,
`low_budget`) is PASS at
[`feedback-postbuild-320x180-20260812-v3`](../artifacts/lumengi/C6/feedback-postbuild-320x180-20260812-v3).
Active cases report feedback hits/pages/dedup and finite/non-negative outputs;
the reload case records a stale-feedback rejection after the scene epoch
changes. The log contains no Fatal/E_INVALIDARG/validation/device-removed
errors. This is resident-page hit/touch evidence only; C6.2 unmapped-card
miss-to-request-to-capture-to-next-frame-valid and eviction/soak remain open.
The latest post-alias short smoke is also PASS at
[`feedback-alias-smoke-320x180-20260812-v4`](../artifacts/lumengi/C6/feedback-alias-smoke-320x180-20260812-v4),
confirming the lookup-hit mirrors are present in the current binary.

## 2026-08-12 C6.2 miss/request/capture evidence

The bounded miss path now writes a per-card request buffer from failed page,
metadata, or visibility validation. The host consumes the previous frame's
request readback before scheduling capture, validates the scene/card epoch,
deduplicates through `CaptureScheduler::enqueueFeedbackRequest()`, and
exports request and capture-completion counters.

Artifact:
[`request-postbuild-320x180-20260812-v5`](../artifacts/lumengi/C6/request-postbuild-320x180-20260812-v5).

The Release Cornell matrix is PASS for `lookup_on`, `lookup_off`,
`invalidate`, and `low_budget`. The active lookup case records
`requestRaw=5510`, `requestCards=111`, `requestDedup=5399`,
`requestCaptureCompleted=111`, `feedbackHits=28`, and `feedbackPages=14`.
The invalidate case records `8053/166/7887/166`; low-budget records
`5538/22/5516/14`, showing deferred work rather than a silent black
substitution. All outputs are finite/non-negative and the log has no
Fatal/E_INVALIDARG/validation/device-removed matches.

Scope is intentionally bounded: this proves GPU miss→host request→scheduler
enqueue→capture-completion activity, not full UE demand priority, explicit
next-frame page-validity telemetry, tiny-atlas eviction/reuse, or long soak.
Those remain open and must not be inferred from non-zero request counters.

## 2026-08-12 A2 post-C6 history convergence

The latest Release binary ran the Cornell/front 800x450 convergence harness,
rendering every intermediate frame through checkpoints 1/8/32/96. Artifact:
[`post-c62-a2-20260812`](../artifacts/lumengi/screenprobe-convergence/post-c62-a2-20260812/).
The manifest is PASS: all probe, temporal-confidence, spatial, and resolved
outputs are finite/non-negative with alpha present where required, and the log
has no Fatal/E_INVALIDARG/validation/device-removed match.

OpenEXR RGB comparison gives resolved-GI MAE `0.00770` (1→8), `0.00368`
(8→32), `0.00213` (32→96). Spatial-filter MAE is `0.05297`, `0.02173`,
`0.01117`; convergence improves, but this does not establish a no-noise visual
Gate. The remaining A2 work is source-radiance variance/mottle reduction,
lighting-generation and guide rejection under dynamic changes, and a paired
GPU-cost comparison.

## 2026-08-12 A2 raw-radiance age sidecar

The raw screen-radiance history now exposes a separate R32Uint ping-pong age
sidecar; RGBA16F alpha remains secondary hit distance. A valid reprojection
increments age, while invalid/reset samples clear it. The Release 800x450
Cornell run at [`post-age-a2-20260812`](../artifacts/lumengi/screenprobe-convergence/post-age-a2-20260812/)
passes checkpoints 1/8/32/96 with no Fatal/E_INVALIDARG/validation/device-
removed matches. Age max/mean is `1/0.313`, `7/2.770`, `31/13.011`, and
`95/40.398`, respectively.

This is a static-scene age/clear contract, not proof of noise-free output or
dynamic-scene correctness. Lighting-generation changes, moving-card rejection,
reason counters, variance reduction and performance repeats remain open.

## 2026-08-12 final Release rebuild / C6.2 smoke

`cmake --build build/windows-vs2022 --config Release --target LumenGI Mogwai --parallel 1`
passed. The post-build Cornell 320x180 C6.2 run wrote
`artifacts/lumengi/C6/request-finalbuild-320x180-20260812-v6` and Mogwai
exited with code 0. `lookup_on`, `invalidate`, and `low_budget` all passed the
request, feedback, finite/non-negative, and capture-completion gates. The
runtime log contains no Fatal/E_INVALIDARG/validation/device-removed,
Traceback, missing-member, or error-30015 matches. The outer PowerShell
wrapper's only error was an incorrect attempt to append `.json` to the
artifact file path; no runtime or artifact failure occurred.

## 2026-08-12 C7 direction identity sidecar

`probeValidity` remains a 16-byte uint4 record, with sample slot and an
octahedral direction fingerprint occupying reserved x bits. The v2 decoder
ran on the final Release binary at 800x450 with a camera-cut transition and
continuous checkpoints 1/8/32/96:
`artifacts/lumengi/A1/direction-v2-finalbuild-800x450-20260812-v2/`.
The run is PASS; the cross-checkpoint fingerprint union is 161,204 and the
log has no runtime fatal/E_INVALIDARG/validation/device-removed or shader
error-30015 match. This is identity telemetry, not a claim that the raw
single-sample radiance is already noise-free.

## 2026-08-13 screenshot comparison / C9 endpoint

The current convergence screenshot
[`convergence-front`](../artifacts/lumengi/screenshots/convergence-test-resolved-20260812/convergence-front-resolved.ToneMapperDisplay.dst.96.png)
shows the intended colored-emissive, diffuse-bounce and direct-metal result.
The Arcade screenshot
[`arcade-front`](../artifacts/lumengi/screenshots/final-realtime-lumengi-20260811/arcade-front-resolved.ToneMapperDisplay.dst.96.png)
shows stable cabinet/seat shadows and emissive response. Arcade contains only
opaque materials, so neither image is a glass/transmission acceptance image.

The raw ScreenProbe-only branch still has low-frequency wall mottle; the clean
presentation branch uses raw-HWRT diffuse radiance denoising plus the resolved
LumenGI diffuse term. The correct status is therefore
`presentation PASS / production ScreenProbe PARTIAL`.

The C9 runtime endpoint was sampled from
[`runtime-finalcolor-20260813b`](../artifacts/lumengi/C9/runtime-finalcolor-20260813b/):
`ResolvedCompositePreview.out`, frame 16, mean `0.37224`, max `11.1980`,
finite/non-negative, with no runtime validation errors. Mark-off/export
equivalence remains explicitly `BLOCKED` until a second graph execution is
available.

The C6 two-phase tiny-atlas pressure run
[`tiny-phase-20260813`](../artifacts/lumengi/C6/tiny-phase-20260813/)
reached all 12 camera partitions and requested 183 cards, but still observed
five resident pages and zero evictions. This remains a genuine lifecycle
blocker, not a screenshot-quality pass.

## 2026-08-13 Surface Cache page-local clear

The capture path now clears each unique 16x16 page in the material, radiance,
and metadata atlases before raster capture. This is the UE-style reuse fence
for uncovered/backface/alpha-test texels; it is separate from page generation
and does not claim eviction by itself. The Release runtime reports matching
`pageClearCommands`/`pageClearTexels` counters in
[`pageclear-phase-20260813-v2`](../artifacts/lumengi/C6/pageclear-phase-20260813-v2/).
The phased pressure run is still BLOCKED for eviction and stale-owner evidence:
five resident owners, zero evictions, and no generation transition were
observed. Camera-only partitions do not dirty CardScene cards, so aggregate
request counts are not distinct-page proof.

The allocator-side page identity telemetry (last allocated/evicted/touched
page ID, generation and frame) is now covered by all ten
`LumenSurfaceCache_*` CPU tests. The report is at
[`page identity CPU`](../artifacts/lumengi/C6/page-identity-cpu-20260813/);
GPU eviction and stale-texel evidence remains BLOCKED.

## 2026-08-13 Real dirty-pressure and scheduler identity

The dirty-pressure runner now mutates real material properties and records
following-frame metadata plus scheduler card/page/generation identity. The
144-frame, 64-texel-atlas run applied six mutation batches without runtime
errors and observed card IDs 18/19 mapped to page IDs 1/3/4. It still observed
only four resident pages, generation 1, and zero evictions, so the strict C6.2
eviction/stale-owner gate remains BLOCKED:
[`dirty pressure v2`](../artifacts/lumengi/C6/dirty-pressure-20260813-v2/).

## 2026-08-13 C6.2 multi-card lifecycle PASS

The bounded pressure case was rerun against `test_scenes/sphere_array.pyscene`,
which exposes 64 supported instances and 384 cards. With a 64-texel atlas,
pressure-phase scheduling, real dirty mutations, and 144 scheduler frames, the
run reached the full 16-page capacity and produced 13 evictions. The report
shows page generation `1 -> 2`, seven generation/stale-owner rejects, and
following-frame page metadata pending/ready evidence. All captured outputs were
finite and non-negative; the log has no Fatal, E_INVALIDARG, validation,
device-removed, or traceback errors:
[`sphere-array pressure v4`](../artifacts/lumengi/C6/sphere-array-pressure-20260813-v4/).

The page-clear sentinel is a host lifecycle invariant
(`pageClearTexels == pageClearCommands * 256`), not a raw atlas readback. Thus
C6.2 is closed for bounded request/evict/reuse/next-frame validity, while
long-soak LRU ordering and raw stale-texel readback remain release checks.

## 2026-08-13 C9 producer metadata runtime

The rebuilt full-color Cornell run now records the producer contract explicitly:
`DirectResolve.output` (RTXDI → NRD, including emission) plus
`LumenGI.resolvedDiffuseGI`. At frame 16 the marked composite endpoint was
finite/non-negative (`mean=0.30146`, `max=85.38696`) with no runtime device or
validation failure:
[`runtime-finalcolor-20260813c`](../artifacts/lumengi/C9/runtime-finalcolor-20260813c/).
This closes the metadata ambiguity, but mark-off/export equivalence remains
BLOCKED until an independent second execution compares the same endpoint with
capture/export policy changes.

The follow-up mark-off execution intentionally left Lumen diagnostic outputs
unmarked while retaining the same composite consumer. Its endpoint SHA-256
matched the mark-on run exactly, and both runs were finite/non-negative with no
runtime validation errors:
[`C9 mark-off equivalence`](../artifacts/lumengi/C9/markoff-20260813/mark0/).
The final offline contract is PASS:
[`C9 final contract`](../artifacts/lumengi/C9/finalcolor-contract-20260813-final/).

## 2026-08-13 A2/C5 source-quality preparation

The source-quality runner now validates raw `diffuseRadianceHitDist` at
checkpoints 1/8/32/96, including its independent hit-distance alpha contract,
history age/generation, reject counters, cache hit/coverage-reject telemetry,
and linear-light variance metrics. Its offline self-test passes. A low-
resolution Mogwai attempt did not produce a manifest and was terminated after
initialization timeout; it is recorded as missing GPU evidence, not a quality
pass.

The ScreenProbe source gather also now rejects normal-discontinuous neighbors
and preserves the current hit-distance alpha whenever current RGB is valid;
history alpha is borrowed only for a current miss. Release shader build and
runtime shader copy passed. The next A2/C5 GPU run must show cache coverage and
source-variance evidence before any history-radius tuning is promoted.

The A2 history ABI is now strengthened with a separate R32Uint validity
sidecar for each raw screen-radiance ping-pong slot. Trace taps require both
this bit and the independent RGBA16F hit-distance contract; age and lighting
generation remain separate resources. This is build-verified and covered by
the offline source-quality self-test, but no new GPU screenshot is claimed
until a clean source-variance run completes.

The validity sidecar is now also exposed as optional
`screenRadianceHistoryValidity` (R32Uint), alongside the existing age and
lighting-generation mirrors. This makes the raw-history contract directly
capturable without interpreting hit-distance alpha or image luminance.

The Radiance Cache CPU preparation now reports a live post-render
`radianceCacheStats` snapshot (`contractStatus=1`, 64 scheduled resident slots,
50,200 estimated bytes) in
[`S7 CPU stats smoke v3`](../artifacts/lumengi/S7/cpu-stats-smoke-20260813-v3.json).
GPU producer/interpolation fields remain explicitly zero and the
`radianceCache`/`radianceCacheHitDist` channels are absent, so C10 is not
claimed as a production GPU pass.

The follow-up Release build and single-frame GPU smoke after the scene/hot-reload
reset hardening produced the same bounded CPU snapshot (64 resident slots,
51,200 estimated bytes, `contractStatus=1`) with no Fatal, E_INVALIDARG,
validation, or device-removal errors:
[`S7 CPU stats smoke v4`](../artifacts/lumengi/S7/cpu-stats-smoke-20260813-v4.json).
The smoke still reports `radianceCache` channels unavailable and GPU producer,
interpolation, trace, and ready-next-frame counters at zero; this is expected
and keeps C10 explicitly OPEN.

### Screenshot comparison used for the next-wave decision

- Full-color Arcade presentation: [`arcade front`](../artifacts/lumengi/screenshots/final-realtime-lumengi-20260811/arcade-front-resolved.ToneMapperDisplay.dst.96.png). It has visible chair/cabinet contact shadows and emissive response, but the scene is fully opaque and does not exercise glass/transmission.
- Probe-only diagnostic: [`Cornell probe frame 96`](../artifacts/lumengi/screenshots/screenprobe-convergence-p0c-20260811/screenprobe-cornell-front.ToneMapperDisplay.dst.96.png). It exposes the low-frequency wall mottle from the 1-spp source; this is a producer diagnostic, not the final composite.
- Material/convergence scene: [`convergence front`](../artifacts/lumengi/screenshots/convergence-test-resolved-20260812/convergence-front-resolved.ToneMapperDisplay.dst.96.png). Direct reflection is visible on the dark/metallic rows, while glass/transmission remains unsupported in the current LumenGI branch.

### C10 Wave-1 GPU smoke (2026-08-13)

The source-backed Radiance Cache producer now compiles and dispatches in the
Release Mogwai process. The binding-name failure (`No member named 'CB'`) was
fixed, and the smoke reached both compute entries without Fatal/E_INVALIDARG or
device removal. The latest evidence is
[`wave1 smoke v10`](../artifacts/lumengi/C10/wave1-smoke-20260813-v10.json).

The report records `gpuProducerEnabled=1`, `gpuInterpolationEnabled=1`,
`traceCount=8192`, `commitCount=8192`, and a ready-next-frame fence, while raw
GBuffer/source channels are finite and non-negative. However both
`radianceCache` and `radianceCacheHitDist` remain RGB/alpha zero at every
checkpoint. This is an honest **C10 PARTIAL**: dispatch/ABI is proven, but
world-to-clipmap coverage or payload projection is not yet producing valid cache
samples. The next implementation step is persistent indirection plus a typed
non-zero payload/coverage gate; no final GI image has been replaced by this
diagnostic output.
## 2026-08-13 C10 Wave-2 scene-backed producer and C5 grid preservation

Release/Mogwai runtime compilation succeeded after adding the scene-backed
Radiance Cache probe producer. `artifacts/lumengi/C10/wave2-smoke-20260813-v18.json`
shows finite cache outputs and nonzero valid hit-distance coverage; Cornell's
default environment is black, so RGB-zero cache samples are legal at this
stage. The strict GPU gate remains BLOCKED because the legacy runner does not
yet emit the required v1 request/ready/fallback schema, and the producer still
lacks UE-style secondary-hit lighting plus an explicit bHit/sky-valid sidecar.

The card-grid candidate index now includes all active cards, including
non-resident pages, and cache lookup is disabled when no page publication exists
for the current frame. A same-scene full-scan/grid GPU equivalence run remains
required.

### 2026-08-13 C9 runtime closure and C10 validity sidecar

The C9 full-color runtime was executed twice at 800x450 Cornell/front with
direct RTXDI/NRD enabled: once with Lumen diagnostic outputs marked and once
with them unmarked. Both manifests record the explicit producer pair
`DirectResolve.output + LumenGI.resolvedDiffuseGI`, finite/nonnegative output,
and a passing mark-off/export comparison. Independent process readbacks had
different low-order bytes but identical mean/max within `1e-4`; the contract
accepts that bounded GPU jitter and reports `PASS`.

The C10 v19 smoke now includes `radianceCacheValidity` (R32Uint). Its bits
separate hit, sky, radiance, and producer validity from the RGBA16F hit-distance
sentinel; warmup produces nonzero sky-valid coverage without treating black
Cornell environment radiance as an invalid sample. C10 remains **PARTIAL**:
secondary-hit lighting, persistent indirection/allocator commit, stale-write
and non-black fallback gates are still open, and the cache is not connected to
`resolvedDiffuseGI`.

### 2026-08-13 C10 bounded hit-lighting follow-up

The producer now uses eight deterministic probe directions and the existing
Lumen analytic/emissive next-event adapter for each scene hit. The Release
Mogwai smoke [`emissive-nee-smoke-20260813`](../artifacts/lumengi/C10/emissive-nee-smoke-20260813.json)
compiled and dispatched without fatal/D3D12 errors; static Cornell samples show
finite hit-distance coverage, validity hit fraction about 2.68%, and nonzero
cache radiance (`rgbMax≈0.58`). This closes the first hit-lighting diagnostic
gap, but does not promote C10: the cache is still not connected to
`resolvedDiffuseGI`, lacks persistent UE-style allocator/commit/query lifecycle,
and has no multi-view dynamic-light quality or soak evidence.

The short-frame follow-up
[`emissive-nee-stats-20260813`](../artifacts/lumengi/C10/emissive-nee-stats-20260813)
records `traceCount=128`, `probeDirectionCount=8`, `probeRayCount=1024`,
`commitCount=128`, producer/interpolator enabled, and `readyNextFrame=1` with
clean runtime logs. The legacy S7 trajectory still reports GPU VRAM/scroll/
dynamic-light checks as `SKIP`; the strict C10 v1 manifest and final-resolve
consumption remain required before any production claim.

The historical pre-wiring strict producer report
[`producer-gate3-20260814`](../artifacts/lumengi/C10/producer-gate3-20260814.json)
passes schema, producer counters, frame-N to frame-N+1 readiness, hit/sky
validity, and hit-distance sentinel checks. It is intentionally **PARTIAL**
because `finalResolveConnected=false`; the later resolve-wiring smoke updates
only that connectivity field, while C10 lifecycle and quality gates remain open.

### 2026-08-14 resolve wiring and rough-specular boundary

The final resolve shader now accepts an optional C10 cache fallback. It checks
the cache validity bitmask and confidence independently from RGB magnitude,
then preserves the existing reconstructed/HWRT fallback for invalid samples.
The Release LumenGI target builds cleanly; runtime proof of
`finalResolveConnected` and a fresh image comparison are still pending.

E1 now has a disabled-by-default directional rough-specular producer and an
offline contract gate. It intentionally has no host binding yet and cannot be
called indirect Lumen specular until directional probe data, independent
history, and roughness/cone rejection are wired and measured.
The offline contract artifact is
[`rough-specular-gate.json`](../artifacts/lumengi/E1/rough-specular-gate/rough-specular-gate.json).

The short runtime follow-up
[`resolve-wiring-producer-gate-20260814`](../artifacts/lumengi/C10/resolve-wiring-producer-gate-20260814.json)
now passes the strict producer gate, including `finalResolveConnected=true`.
This is a bounded fallback integration proof, not a full C10 release pass:
VRAM/scroll/dynamic-light checks are still skipped, and allocator/eviction,
stale-owner, multi-view quality, and soak evidence remain open.

The tiny dirty-pressure C6 run
[`tiny-dirty-pressure-20260814`](../artifacts/lumengi/C6/tiny-dirty-pressure-20260814/surfacecache-effect.json)
applied five material mutation batches and observed next-frame metadata
pending/ready transitions across 12 scheduled partitions. It remains BLOCKED
for strict lifecycle closure because no eviction/generation reuse occurred and
mutation card identity/stale-owner evidence is incomplete.

The stricter one-page rerun
[`tiny-onepage-dirty-20260814`](../artifacts/lumengi/C6/tiny-onepage-dirty-20260814/surfacecache-effect.json)
also produced no eviction/generation-reuse event despite applied mutations;
the remaining blocker is distinct page-owner demand in the scheduler, not
atlas capacity or a relaxed threshold.

The supported `sphere_array.pyscene` pressure artifact
[`sphere-array-pressure-20260813-v4`](../artifacts/lumengi/C6/sphere-array-pressure-20260813-v4/)
is the stronger bounded C6.2 evidence: 144 frames, 64px atlas, 13 evictions,
generation 1→2 reuse, seven generation/stale-owner rejects, page-clear and
finite-output checks. Miss-request priority and long-soak remain open.

The short C6 post-completion smoke
[`postcompletion-smoke-20260814`](../artifacts/lumengi/C6/postcompletion-smoke-20260814/surfacecache-effect.json)
is clean at 320x180 for lookup-on/off: outputs are finite/nonnegative, page
clear/metadata and request/capture completion telemetry are readable, and
validated feedback touches are observed. It does not exercise tiny-atlas
eviction, stale-owner reuse, dirty-card mutation, or next-frame validity.

## 2026-08-30 C9 producer-trace localization

The strict C9 replay remains unchanged and `FAIL/OPEN`; the new test-only
`LUMEN_C9_PRODUCER_TRACE_OUT` mode adds three marked `BlitPass` sentinels so
DirectResolve, LumenGI, and Composite can be read back as float32 sidecars.
The v20 run measured mean deltas of `2.1424e-6` (DirectResolve), `4.6613e-5`
(LumenGI), and `4.7884e-5` (Composite), showing that the residual is on the
LumenGI producer path rather than direct lighting. V21 disabled only Lumen
temporal filtering (`3.3638e-5`, max `1.8646e-2`); v22 disabled temporal and
spatial filters (`2.8192e-5`). Neither diagnostic topology is a strict gate
candidate. The default v23 regression left both filters enabled and sentinels
off, and measured `4.3798e-5`. Keep the production path and frozen C9 limits;
the companion offline tool is
[`run_c9_producer_snapshot_diff.py`](../tests/lumengi/run_c9_producer_snapshot_diff.py).

## 2026-08-30 C9 cache-path isolation

The v24-v26 producer-trace matrix is diagnostic-only because its three
`BlitPass` sentinels change graph topology. It nevertheless provides a clean
source split: only Surface Cache + Cache Lighting enabled together produced a
non-zero LumenGI delta; disabling either switch made that producer delta
exactly zero. The v29 strict no-cache control passed the unchanged C9 limits
(`mean=2.8565e-6`, `p99=7.1168e-5`, `max=2.1065e-3`). The v27 explicit
pre-FrameCapture wait was recorded PASS but remained strict FAIL at
`mean=3.0024e-5`, while v28 without the wait measured `6.1814e-5`. Keep both
cache features enabled in production and treat this as the next barrier/
resource-ordering investigation boundary.

## 2026-08-30 C9 feedback/request isolation

The v30 lookup-off diagnostic passed strict C9 while Surface Cache capture and
Cache Lighting remained enabled. The v31 test-only switch
`LUMEN_C9_DISABLE_SURFACE_CACHE_FEEDBACK=1` retained cache lookup and cache
radiance replacement but removed feedback/request UAV atomics and their
next-frame host readbacks; it passed with mean `3.8822e-6`, p99 `1.2207e-4`,
and max `2.1065e-3`. A same-build v32 default-feedback control failed with
mean `2.4613e-5`, p99 `3.6621e-4`, and max `3.4180e-3`. This is diagnostic
localization only: the switch defaults off, production lookup/feedback/request
behavior is unchanged, and the strict thresholds remain frozen.

The v34 rerun includes a fail-closed stale-pending-readback guard and records
`screenProbeStats`/`surfaceCacheStats` in the replay JSON. Lookup is proven
active (`58470` attempts, `2905` hits), while feedback/request counters are all
zero; strict C9 passes at mean `2.8565e-6`, p99 `7.1168e-5`, max `2.1065e-3`,
relative `2.4669e-5`. This remains diagnostic-only; v32 with default feedback
enabled is still the production `FAIL/OPEN` control.
