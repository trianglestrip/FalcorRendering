# LumenGI UE5.8-aligned next-wave plan

This document is the readable execution checkpoint for the current Falcor
LumenGI work. UE5.8 is used as a contract reference: mark/update caches,
trace, filter, integrate, update history, then resolve material outputs. It is
not a claim that Falcor has UE's full renderer implementation.

## Current checkpoint (2026-08-13)

| Area | Status | Evidence / boundary |
|---|---|---|
| C1 environment importance sampler | PASS | `ParameterBlock<EnvMapSampler>` variant; envSampler=1 runtime artifacts have no `E_INVALIDARG`. |
| C4 route and compose | PASS for bounded route | Screen -> GDF -> HWRT route and compose dispatch run; hit-lighting quality remains scene-dependent. |
| C5 Surface Cache consumer | PASS_NARROW_EFFECT | `material_test` has `gdfHits=42`, `cacheLookupHits=1`; Cornell remains coverage/reject limited. Source-quality A/B runner and normal/depth gather guard are build-verified, while a clean post-fix GPU variance run is still required. |
| C7 validity/history | PASS for implemented telemetry | Dynamic lighting/material/environment generations and reset sidecars are runtime-tested; broader image-quality/age ABI remains a gate. |
| C8 export/resource lifetime | PASS_BOUNDED | Marked endpoint equivalence passes; unmarked direct endpoints are intentionally `BLOCKED` by RenderGraph. Raw-buffer FrameCapture skip is fixed. |
| C9 FinalResolve | PASS | Full-color producer metadata and independent mark-off/export composite hash equivalence pass; finite/non-negative and single `E*albedo/PI` remain covered. |
| Direct reflection + diffuse indirect image | PASS_SMOKE | `convergence_test`, 800x450, 32 frames, frame-32 image shows emissive panels, shadows and metallic/glossy rows. |
| Glass/transmission | UNSUPPORTED | Current RTXDI/LumenGI branch has no transmission producer. Use PathTracer+NRD delta outputs as reference only. |

## Dependency graph for the next implementation waves

```text
A1 validity/epochs ─┬─> A2 temporal reconstruction ─┬─> B trace router quality
                    │                                └─> C cache lifecycle quality
                    └─> D resolve/public auxiliary ──> E rough-specular
                                                      └─> F transmission

B + C + D + image/perf gates ──> close C0-C9 ──> only then C10/C11/C12
```

## Ordered work

### A1/A2: producer validity and UE-style history

1. Keep the existing `LumenProbeHit` ABI stable and expose a typed sidecar per
   probe direction: backend, geometry-valid, radiance-valid, producer frame,
   generation, age, and reset reason.
2. Keep alpha meanings separate: incident confidence, history length, secondary
   hit distance, and resolved confidence must not share a channel.
3. Add previous normal/material guides and reprojected moments to the full-
   resolution screen-radiance history. Reject on depth plane, normal angle,
   material identity, motion/disocclusion, and lighting generation.
4. Tune UE-style coupled parameters together: history frame cap and traced
   directions, three small spatial/bilateral passes, and a max-ray-intensity
   firefly ceiling. Do not enlarge blur radius to hide invalid history.

Gate: Cornell and Arcade at 800x450, frames 1/8/32/96, plus camera pan,
lighting change, material change, geometry move, resize and scene reload.
Require finite/non-negative output, monotonic stable age, explicit reject
reasons, no stale generation reuse, and a measurable reduction in tail
variance without a black-frame fallback.

### B: trace routing and hit-lighting quality

Keep route order explicit: screen-history/cache candidate -> GDF/MeshSDF ->
HWRT fallback. Record selected backend and fallback reason per producer. The
current C5 material-scene pass is the compatibility smoke; next fix is the
Cornell card plane/extent mapping that produced 56,091 coverage rejects. Add a
candidate index or world-space card mapping before relaxing metadata guards.

### C: Surface Cache lifecycle

Complete the UE-style state machine:

```text
Unmapped -> Feedbacked -> Requested -> Capturing -> Captured
         -> LightingPending -> Valid -> LastUsed -> EvictPending
```

GPU lookup may consume only matching owner/card/page generations and a Valid
lighting generation. Add request deduplication, last-used touches, page clear
sentinels, and stale-owner rejection counters. Missing pages must fall back to
HWRT/GDF lighting, never to an implicit black texel.

Gate: lookup on/off, scene reload, one-page-per-frame, dynamic light change,
and a 640x360 -> 800x450 resize. Require generation/state/request/eviction
telemetry and no stale-page ghosting.

### D: resolve and output contract

Keep `resolvedDiffuseGI` as the material-resolved diffuse output. The final
producer must publish source kind, frame/generation, confidence and fallback
reason as auxiliary data; `markOutput()` must remain a capture option, not an
algorithm dependency. The current C9 gate is the minimum contract; the next
step is a production readback/debug API for typed raw buffers, not a relaxed
`getOutput()` check.

### E/F: rough specular then transmission

Follow UE's separate diffuse and rough-specular indirect outputs. Add a
rough-specular producer/denoiser only after D passes; do not feed RTXDI direct
specular into `resolvedDiffuseGI`. For glass/transmission, use the existing
PathTracer+NRD delta reflection/transmission graph as a reference gate first,
then implement a separate transmission producer and material resolve. Arcade
is not a glass scene and must remain a diffuse/emissive/shadow baseline.

### Performance and release closure

After A-D are stable, measure GPU timestamps, p95/max frame time, trace rays,
dispatch count, VRAM, cache bytes and probe/page budgets at 640x360, 800x450,
and 1280x720. Keep image correctness and performance as equal gates. Only
after C0-C9 are closed should C10 Radiance Cache, C11 quality presets, and C12
the Gauntlet-style scene matrix begin.

## Reproducibility and Markdown evidence protocol

Every GPU run uses a unique artifact directory containing the Mogwai log,
manifest, build commit/hash, scene, camera, resolution, frame schedule,
runtime toggles, warnings/errors, and image statistics. The corresponding
step and verdict are appended to `docs/LumenGI_Visual_Debug_Log.md`. A screenshot
is visual evidence only; a PASS requires the typed counters/validity and
finite/non-negative checks described above.

### Latest C5 checkpoint (2026-08-12)

The UE-style card lookup correction is now runtime-tested. A slab/facing
candidate test raised material-scene cache hits from `1` to `23` at 320x180 and
from `1--2` to `179` at 800x450; Cornell changed from `0` to `2` hits at
320x180. All three reports are finite/non-negative and free of runtime or
shader-link errors. This moves C5 from a coverage-mapping blocker to a
quality/performance backlog item. It is not permission to claim full UE Surface
Cache parity: the scan is still O(cards), page/coverage rejects remain high,
and feedback/request/last-used timing plus GPU timestamps are still required.

### Performance checkpoint (2026-08-12)

The benchmark harness now records an explicit JSON pass-property override. The
first production-chain calibration (640x360, 20 warmup + 60 capture) measured
whole-frame GPU P95 `5.416 ms` and LumenGI RenderGraph P95 `4.740 ms` on the
RTX 2060 SUPER. This proves the timestamp path and the enabled route are being
measured; it is not yet a release gate. Repeat at 800x450 and 1280x720, collect
VRAM/budget and three-run p95/max, then run the required long soak before
adding rough-specular or transmission work.

The 800x450 and 1280x720 repeats now report whole-frame GPU P95 `7.095 ms` and
`13.252 ms` (LumenGI P95 `6.687 ms` and `12.648 ms`) with no runtime errors.
The 1280x720 short run fits the 16.67 ms budget, but the UE-style release rule
still requires independent repeats, VRAM/budget telemetry and long soak before
quality branches are expanded.

The first 600-frame/120-warmup sample at 1280x720 is also complete: whole-frame
GPU P95/P99/max `13.378/13.565/14.252 ms`, LumenGI P95/P99/max
`12.628/12.732/12.911 ms`, with no runtime or device-removal errors. Keep the
quality/performance gate open until three independent runs and VRAM/long-soak
evidence are recorded.

Two independent 60-frame repeats at 1280x720 report whole-frame P95/P99/max
`13.481/13.585/13.640 ms` and `13.381/13.550/13.658 ms`; LumenGI reports
`12.717/12.880/12.880 ms` and `12.610/12.664/12.712 ms`. Together with the
600-frame run this gives three timing distributions. VRAM and multi-hour soak
remain explicit release gates.

Resource-side telemetry is now stored in benchmark manifests: page allocation,
capture completion, resident bytes, generation/state rejects, probe GDF/cache
hits, and fallback rates. This makes VRAM/cache-budget review reproducible;
the current 640x360 convergence control has 90 resident pages and zero cache
hits, so material/Cornell cache-consumer evidence must remain separate.

### Exact Surface Cache bytes (2026-08-12)

The benchmark now exports exact byte counters rather than only integer MiB.
The post-rebuild 640x360 production run records 90 allocated/completed pages,
`residentBytes=460800`, `memoryBudgetBytes=536870912`, and zero allocation or
generation/state rejects at
`artifacts/lumengi/benchmark/c5-slab-production-640x360-exactbytes-20260812/`.
Interpret this as Surface Cache residency/budget evidence, not total GPU VRAM;
GPU-wide memory and long soak remain open release gates.

### Multi-view material image checkpoint (2026-08-12)

`convergence_test.pyscene` was captured at 800x450 for 96 sequential frames
from front/left/right with the current direct RTXDI/NRD plus diffuse LumenGI
path. The artifact is
`artifacts/lumengi/screenshots/convergence-test-resolved-20260812/`; all
typed outputs are finite/non-negative and the runtime log is clean. Metal
reflection and direct shadowing are visible. Glass/transmission remains
`UNSUPPORTED/PARTIAL` because no production transmission producer is wired;
use the PathTracer+NRD delta graph only as reference until Wave E adds a
separate transmission contract.

### Validity convergence checkpoint (2026-08-12)

The post-rebuild Cornell/front 800x450 sidecar run completed frames 1/8/32/96
with all checkpoints `PASS` and 182,400 `uint4` records per checkpoint:
`artifacts/lumengi/screenprobe-convergence/postbytes-20260812/`.
Generation/age transitions and Invalid/Screen/HWRT backend records are now
readable and finite. Direction-union identity and complete UE scene-radiance
history (normal/material/moments parity) remain follow-up work.

### Candidate-grid lookup (2026-08-12)

To move the Surface Cache consumer toward UE's spatial page lookup, an opt-in
fixed 16^3 world-space card candidate grid with 32 entries per cell and an
overflow full-scan fallback was added. It is currently disabled by default:
same-scene A/B showed fewer cache hits and a small probe mean shift than the
full-scan control, despite finite outputs and clean runtime logs. The next C5
step is to fix/prove candidate completeness (or remove the experiment), then
measure multi-scene coverage and UE-style feedback/request-dedup/last-used/
eviction telemetry without weakening stale-generation/page-state rejection.

### Surface Cache scene-generation reload gate (2026-08-12)

The allocator keeps page generations scene-local and clears them on scene reload;
reusing the local value across scenes would weaken the stale-page fence. Host
telemetry now exports monotonic `surfaceCacheSceneGeneration` and
`surfaceCacheResetCount` alongside page generation/state counters. The C6 runner
accepts a page-generation transition or an explicit scene/reset-generation
transition, while still requiring page metadata, state, stale-owner,
request-dedup, last-used and eviction fields.

Release post-build material 320x180 full matrix (`lookup_on`, `lookup_off`,
`invalidate`, `low_budget`) is PASS at
`artifacts/lumengi/C6/surfacecache-effect-material-320x180-postepoch-20260812/`.
The invalidate case records scene generation `2 -> 3` and reset count `2 -> 3`,
page generation `1 -> 1` (expected local reset), finite/non-negative outputs,
and zero Fatal/E_INVALIDARG/validation/device-removed matches. This closes the
reload evidence gap, but request prioritization, non-zero last-used/eviction
coverage and GPU timing/VRAM soak remain open C6 release gates.

### A1 validity transition gate (2026-08-12)

The post-build raw `probeValidity` transition matrix is PASS for both
800x450 and the partial-tile 641x361 resolution, covering `scene_reload` and
`camera_cut` at sequential checkpoints 1/8/32/96. All four runs decode finite
uint4 sidecar records with backend/source validity, generation and age reset
evidence, and the runtime logs contain no Fatal/E_INVALIDARG/validation/device-
removed errors. Artifact:
`artifacts/lumengi/A1/probe-validity-transitions-postepoch-20260812/`.
Direction-union identity and full UE radiance-history parity remain separate
follow-up gates.

### Current production-chain performance checkpoint (2026-08-12)

The post-epoch Release binary was benchmarked on Cornell at 1280x720 with
120 warmup and 600 captured frames, using ScreenProbe, Temporal, Spatial,
Surface Cache and Cache Lighting, with the candidate grid disabled. Whole-frame
GPU p95/p99/max is `10.179/10.437/10.609 ms`; LumenGI GPU p95/p99/max is
`9.533/9.618/9.951 ms`, within the 16.67 ms 60 Hz budget for this run. The
manifest records 14,400 probes, 16 directions, 0.1305 screen-hit rate,
`residentBytes=61440`, and finite/non-negative cache/probe stats:
`artifacts/lumengi/benchmark/release-postepoch-cornell-1280x720-20260812/`.
This is one post-build distribution, not the final performance release gate:
repeat runs, GPU-wide VRAM telemetry and long soak remain required.

### C6.1 resident-page demand feedback (2026-08-12)

The first UE-aligned demand step is now live for resident pages: ScreenProbe
Surface Cache hits atomically write `{hitCount,pageGeneration}` into a per-page
UAV, the host copies it to readback, validates scene epoch/page generation/page
state before `touchPage()`, and performs that touch before the next capture
scheduler budget is selected. The Release 320x180 Cornell matrix is PASS for
`lookup_on`, `lookup_off` (page telemetry not applicable), `invalidate`, and
`low_budget`:
[`feedback-postbuild-320x180-20260812-v3`](../artifacts/lumengi/C6/feedback-postbuild-320x180-20260812-v3).
The active cases report feedback hits/pages/dedup and finite outputs; invalidate
also observes one stale-feedback rejection after the scene epoch changes. The
runtime log has no Fatal/E_INVALIDARG/validation/device-removed errors.

This closes C6.1 hit-to-LRU feedback, not the complete UE lifecycle. The next
step is C6.2: a per-card miss/request ring with deduplication, priority,
capture completion and next-frame-valid evidence, followed by tiny-atlas
eviction/soak. Candidate-grid remains disabled and is not part of this gate.

### C6.2 miss/request-to-capture checkpoint (2026-08-12)

The per-card request path is now runtime exercised. A failed resident-page
lookup emits `{rawMissCount, reasonBits}` into a card-indexed GPU request
buffer; the host reads it before the next capture-scheduler pass, validates
scene/card identity, deduplicates requests through the scheduler worklist, and
reports capture completion separately from requests issued. The Release
320x180 Cornell matrix is PASS at
[`request-postbuild-320x180-20260812-v5`](../artifacts/lumengi/C6/request-postbuild-320x180-20260812-v5).

The active `lookup_on` case reports `requestRaw=5510`, `requestCards=111`,
`requestDedup=5399`, `requestCaptureCompleted=111`, `feedbackHits=28`, and
`feedbackPages=14`; `invalidate` reports `8053/166/7887/166` for the same
request fields; `low_budget` reports `5538/22/5516/14`, demonstrating that
the scheduler budget can defer some requested captures without substituting
black GI. All cases are finite/non-negative and the runtime log has no
Fatal/E_INVALIDARG/validation/device-removed matches.

This closes the bounded C6.2 request-buffer, deduplication, scheduler enqueue,
and capture-completion smoke. It does not yet prove UE-complete request
priority, an explicit next-frame-valid page transition, tiny-atlas eviction,
stale-owner rejection under reuse, or long soak. Those remain release gates;
the C6 label is `PASS_BOUNDED_REQUEST_SMOKE`, not full Surface Cache closure.

### A2 post-C6 convergence runtime check (2026-08-12)

The latest Release binary passed the single-scene UE-style history harness at
Cornell/front 800x450, rendering continuously through checkpoints 1/8/32/96:
[`post-c62-a2-20260812`](../artifacts/lumengi/screenprobe-convergence/post-c62-a2-20260812/).
All required probe, temporal-confidence, spatial, and resolve channels are
finite/non-negative with retained alpha; the Mogwai log has no
Fatal/E_INVALIDARG/validation/device-removed matches. Resolved-GI frame-to-frame
MAE is `0.00770` (1→8), `0.00368` (8→32), and `0.00213` (32→96), while the
spatial-filter MAE is `0.05297`, `0.02173`, and `0.01117` respectively.

This is a runtime-contract and convergence checkpoint, not a no-noise PASS:
source-radiance mottle, lighting-generation transitions, previous-guide
rejection, reprojected moments, and GPU cost versus the current baseline still
need dedicated A/B evidence. Keep A2 open before adding rough-specular or
transmission producers.

### A2 raw-radiance history age sidecar (2026-08-12)

The raw screen-radiance history now has a separate R32Uint ping-pong age
texture. RGBA16F alpha remains the secondary hit distance; accepted
depth/normal/material reprojection increments age, while reset/reject/invalid
samples write zero. The diagnostic output is `screenRadianceHistoryAge`.

Release runtime evidence at
[`post-age-a2-20260812`](../artifacts/lumengi/screenprobe-convergence/post-age-a2-20260812/)
passes Cornell/front 800x450 checkpoints 1/8/32/96. Age max/mean is
`1/0.313`, `7/2.770`, `31/13.011`, `95/40.398`; all outputs are finite and
non-negative and the log has no runtime validation errors. This closes the
age-sidecar contract and static-scene monotonicity smoke. Dynamic lighting,
moving geometry, rejection-reason distribution, variance quality and repeated
performance remain open A2 gates.

### Final Release rebuild and C6.2 smoke (2026-08-12)

The final single-threaded Release rebuild of `LumenGI` and `Mogwai` completed
successfully. A post-build 320x180 Cornell run exercised `lookup_on`,
`invalidate`, and `low_budget` through checkpoints 1/8/16:
`artifacts/lumengi/C6/request-finalbuild-320x180-20260812-v6`.
The JSON is `complete`; all active cases pass finite/non-negative output and
request/feedback activity checks. The latest values are `lookup_on`
request `5510/111/5399/111`, `invalidate` `8053/166/7887/166`, and
`low_budget` `5538/22/5516/14` for raw/cards/dedup/capture-completed.
The Mogwai log has no Fatal, E_INVALIDARG, validation, device-removed,
Traceback, missing-member, or shader error-30015 matches. The command wrapper
reported a non-zero status only because it tried to read the JSON as a child
file; the runner itself exited 0 and wrote the artifact as the file path above.

### C7 direction identity sidecar (2026-08-12)

The existing `probeValidity` uint4 ABI now packs the six-bit Hammersley sample
slot and an eight-bit octahedral direction fingerprint into previously reserved
bits; the buffer remains 16 bytes per direction. The transition decoder is
`LumenGI.ProbeValiditySidecar.v2`. A final-build 800x450 camera-cut run passed
at checkpoints 1/8/32/96 with `directionIdentityContract.sampled=true` and a
cross-checkpoint union of 161,204 probe/fingerprint identities:
`artifacts/lumengi/A1/direction-v2-finalbuild-800x450-20260812-v2/`.
The runtime log has no Fatal/E_INVALIDARG/validation/device-removed,
Traceback, missing-member, or error-30015 matches. This proves producer-side
direction identity telemetry and a growing quantized union; exact vector
reconstruction and bias/variance image quality remain separate gates.

### Subagent audit: next implementation wave (2026-08-12)

Three bounded read-only audits were executed in parallel over the current
workspace. Their conclusions freeze the next dependency order:

1. **A2 dynamic history is the first implementation wave.** The current
   producer already has raw radiance, depth, normal/material guide, moments,
   lighting generation, and age. The missing production evidence is a typed
   reject-reason counter and paired dynamic image/variance data for light,
   material, geometry, motion, and invalid-history transitions. The minimum
   file boundary is `LumenScreenRadianceHistory.cs.slang` plus
   `LumenGI.cpp/.h` counters/readback and a dynamic runner. Temporal/Spatial
   algorithms remain frozen until this evidence exists. Static age evidence is
   PASS; no-noise quality remains OPEN.
2. **C6.2 remains bounded request smoke.** The current request-completed
   counter is command emission, not GPU capture completion; there is no
   explicit LightingPending/Valid or next-frame fence. The next GPU runner
   must set `surfaceCacheAtlasSize=64/128`, use at least 12 cards, run at least
   72 frames (minimum residency is 60), and record per-frame request,
   generation/state, lighting-valid, eviction, stale-owner, and cache-hit
   transitions. Reused pages must not expose uncleared texels. Request reason
   bits and per-page last-used frame are still not consumed/exported.
3. **C8/C9 and release remain bounded.** Resolve math and the full-color
   `DirectResolve.output + LumenGI.resolvedDiffuseGI` endpoint now pass the
   runtime producer/mark-off/export contract. Three 1280x720 timing distributions are
   below 16.67 ms (`13.378/13.565/14.252`, `13.481/13.585/13.640`,
   `13.381/13.550/13.658` p95/p99/max), but GPU-wide VRAM, 30-minute dynamic
   soak, and 2-hour soak remain open.

The resulting order is **A2 dynamic rejection evidence → C6 next-frame and
eviction lifecycle → C9 finalColor/release contract → C10 GPU Radiance Cache →
separate rough-specular producer → transmission/glass producer → C11/C12**.
Rough-specular and transmission must not be implemented by relabeling RTXDI
direct specular or the PathTracer reference branch.

### A2/C6 implementation and GPU evidence (2026-08-13)

The raw screen-radiance history now exports typed producer counters through the
existing screen-probe counter readback: accepted history, depth/guide/motion/
lighting rejects, current/previous invalid samples, and reset samples. The
counter prefix remains stable and the buffer is 96 bytes. Release build and
Mogwai runtime compilation passed. The 800x450 Cornell convergence run is
finite/non-negative at 1/8/32/96; static history shows roughly 154k accepted
samples and 206k current-invalid samples by the late checkpoints, with no
Fatal/E_INVALIDARG/validation/device-removed errors:
[`A2 reject telemetry`](../artifacts/lumengi/A2/reject-telemetry-20260813/).

The dynamic runner also passed independent static, camera-cut, scene-reload,
lighting-generation (render-settings epoch toggle), and material/geometry
cases, with readable history counters and finite outputs:
[`A2 dynamic`](../artifacts/lumengi/A2/dynamic-20260813/).

The post-reset camera-cut rerun also passes after the 96-byte counter/readback
and reset-active binding update:
[`camera-cut post-reset`](../artifacts/lumengi/A2/dynamic-20260813/camera_cut_post_ready2/).

C6 tiny-atlas mode now runs continuously through 72/96 frames and records the
pressure contract. It remains intentionally BLOCKED: the current camera/scene
combination allocates only one resident page at 16- or 64-texel atlas size, so
no eviction or stale-owner reuse is observed. This is a real missing pressure
fixture, not a pass; the next C6 change must expose enough distinct card/page
requests (or a deterministic page-pressure scene) before enabling the eviction
Gate:
[`C6 tiny-atlas evidence`](../artifacts/lumengi/C6/tiny-atlas-20260813/).

The subsequent 12-partition convergence pressure run reaches the minimum
frame budget and requests more than twelve cards, but still reports only five
resident pages and no eviction or stale-texel owner evidence. It remains
`BLOCKED`, as required by the gate:
[`C6 partition pressure`](../artifacts/lumengi/C6/tiny-partition-20260813/).
The new `pageMetadataPending/pageMetadataReady` counters are readable and
show the intended publish boundary; they do not replace the missing eviction
and stale-texel evidence.

The two-phase pressure run (`60` warmup frames on one partition, then
partitions `1..11`) also reaches all pressure views and requests `183` cards,
but the allocator still reports five resident pages and zero evictions. This
rules out an insufficient frame schedule; the remaining blocker is the lack of
runtime page/card identity and stale-texel instrumentation:
[`C6 phased pressure`](../artifacts/lumengi/C6/tiny-phase-20260813/).

### Current screenshot comparison and interpretation (2026-08-13)

The resolved presentation branch is visibly cleaner than the raw
ScreenProbe-only branch, but the two images exercise different contracts:

- `convergence-test-resolved-20260812/convergence-front-resolved.ToneMapperDisplay.dst.96.png`
  shows colored emissive panels, diffuse bounce, and direct metal highlights.
  This is valid direct+diffuse presentation evidence; it is not evidence that
  LumenGI already produces indirect rough-specular or transmission.
- `final-realtime-lumengi-20260811/arcade-front-resolved.ToneMapperDisplay.dst.96.png`
  shows stable cabinet/seat shadows and emissive screen response. Arcade has
  opaque materials only, so it is not a glass/transmission test.
- The ScreenProbe-only diagnostic remains the quality bottleneck: its wall
  radiance retains low-frequency mottle after 96 frames. The clean final
  branch uses the raw-HWRT diffuse radiance denoising path, while the probe
  incident path still requires dynamic reject/variance evidence.

Release status therefore remains `presentation PASS / production ScreenProbe
PARTIAL`. Do not use exposure or a wider spatial radius to hide probe mottle,
and do not relabel RTXDI direct specular as Lumen indirect specular.

### C6 page-local clear implementation and pressure result (2026-08-13)

The capture path now runs a dedicated page-local compute clear before raster
capture. Each unique page in the command batch clears all 256 texels of the
capture-owned material, radiance, and metadata atlases; this prevents
backface/alpha-test/coverage holes from retaining a previous page owner's
texels. Runtime telemetry exposes `pageClearCommands` and `pageClearTexels`,
and the C6 runner requires both fields for tiny-atlas cases. The Release build
and runtime shader compilation passed. The 320x180 phased pressure run reached
all twelve camera partitions and recorded `pageClearCommands=1..5` with matching
`pageClearTexels=256..1280`, finite/non-negative outputs, and no runtime errors:
[`C6 page-clear pressure`](../artifacts/lumengi/C6/pageclear-phase-20260813-v2/).

The run remains BLOCKED for the separate eviction/stale-owner gate: only five
distinct page owners are actually captured, camera partitions do not dirty
CardScene geometry, and the allocator reports zero evictions/generation reuse.
Aggregate request counters must not be treated as distinct page pressure.
Next C6 work is page/card identity telemetry plus a supported multi-card dirty
fixture, followed by tiny-atlas eviction and stale-texel validation.

The allocator now exports the last allocated/evicted/touched page ID,
generation, and frame, without changing the Free/Allocated/Touched/
EvictedPending state machine. The ten `LumenSurfaceCache_*` CPU tests pass,
including LRU victim/generation assertions:
[`page identity CPU`](../artifacts/lumengi/C6/page-identity-cpu-20260813/).
This telemetry explains the current five-page pressure result but does not
replace a GPU stale-texel/next-frame-valid run.

### C9 runtime final-color endpoint (2026-08-13)

`ResolvedCompositePreview.out` is now sampled in a real 800x450 convergence
run. The endpoint is marked, finite, and non-negative (`mean=0.37224`,
`max=11.1980` at frame 16), and the Mogwai log contains no Fatal,
E_INVALIDARG, validation, or device-removal errors:
[`C9 final-color runtime`](../artifacts/lumengi/C9/runtime-finalcolor-20260813b/).
The contract is now `PASS`: the showcase emits explicit producer metadata
(`DirectResolve.output` + `LumenGI.resolvedDiffuseGI`), and an independent
mark-off execution produced the same composite SHA-256
(`6aab1f720d21f788f1986ecbe4781601584557e665cf2deda87721a7ad2fad9a`). The
runtime contract report is
[`C9 final-color contract`](../artifacts/lumengi/C9/finalcolor-contract-20260813-final/).

### C6 real dirty-pressure rerun (2026-08-13)

The new runner mutation fixture uses actual Falcor material setters
(`Material.baseColor`) and records the following-frame page metadata/identity;
it never treats camera-only partitions as dirty cards. The 320x180,
64-texel-atlas, 144-frame run applied six mutation batches without setter
errors and all outputs remained finite/non-negative:
[`C6 dirty pressure`](../artifacts/lumengi/C6/dirty-pressure-20260813/).

This is useful bounded evidence, but it is not an eviction pass. Only three
page IDs were observed, resident pages stayed at four, page generations stayed
at one, and `evictions=0`, `lastEvictedPageID=0`, and stale-owner evidence was
absent. The next-frame metadata boundary was readable after each mutation
(`PASS_BOUNDED`), while card identity is still unavailable from host stats.
Therefore C6.2 remains OPEN: export card IDs/request reasons, create a
supported multi-card dirty fixture, then rerun tiny-atlas eviction/reuse and
stale-texel validation before enabling the full lifecycle gate.

The follow-up build now exports the scheduler's last command identity. The
same material-mutation run observes card IDs `18,19` and page pairs
`(18,1,g1)`, `(18,4,g1)`, `(19,3,g1)`, `(19,4,g1)` with no shader/runtime
errors. This closes the previous “no card identity” observability gap, but it
also confirms the fixture still reuses only four resident pages: generation
remains `1`, `lastEvictedPageID=0`, and `evictions=0`:
[`C6 dirty pressure v2`](../artifacts/lumengi/C6/dirty-pressure-20260813-v2/).
The eviction/stale-owner/next-frame-valid gate therefore remains BLOCKED; the
next fixture must dirty enough distinct supported cards (or provide a
deterministic card-demand API) to exceed the four-page atlas and observe a
victim/new-owner generation transition.

### C6.2 starvation fix and multi-instance lifecycle PASS (2026-08-13)

The convergence scene could not exceed twelve supported page owners. The
capture scheduler therefore gained deterministic starvation promotion: a card
waiting eight scheduling passes is promoted ahead of geometric priority. CPU
coverage remains 10/10 scheduler tests green, including deterministic
pending/eviction behavior.

The production pressure fixture now uses `sphere_array.pyscene` (64 supported
instances, 384 cards), mutates real material properties, and runs a 64-texel
atlas for 144 scheduler frames. It passes finite/non-negative output,
following-frame metadata, page-clear fence, and card/page/generation identity;
the run observed 13 evictions, generation 1→2 reuse, and seven generation or
stale-owner rejects:
[`C6 sphere-array lifecycle PASS`](../artifacts/lumengi/C6/sphere-array-pressure-20260813-v4/).

This closes the bounded C6.2 allocator lifecycle gate. The page-clear sentinel
is explicitly a host invariant (`commands * 256 texels`), not an image-derived
claim; a future debug-readback may strengthen it but is not required for this
gate. C6.2 miss-request priority and long-soak behavior remain separate
release checks.

### Next UE-aligned optimization order

1. **Close C8/C9 release contract:** add a production full-scene finalColor
   consumer and mark-off/export equivalence; keep direct RTXDI/NRD, diffuse
   LumenGI, and future specular/transmission channels unit-separated.
2. **Add UE-style rough specular as a separate producer:** directional probe
   representation (SH/oct or reflection-cone samples), independent history and
   roughness/normal/depth rejection, then `roughSpecularGI` material resolve.
   Do not derive it by scaling Lambertian `diffuseGI` or relabel RTXDI direct
   specular.
3. **Add transmission independently:** a dedicated IOR/Fresnel/refraction
   producer with throughput, hit-distance and validity outputs; use
   PathTracer+NRD only as a reference until this producer is in the Lumen
   final-color chain.
4. **Finish release evidence:** GPU-wide VRAM accounting, 30-minute dynamic
   soak and 2-hour soak, 640/800/1280 resolution matrix, and three independent
   p95/p99/max timing distributions. Only after these gates should Radiance
   Cache/C11 presets/C12 publish matrix be enabled.

### A2/C5 source-quality wave (after C9, before rough specular)

The next quality wave must target the producer rather than increasing the
spatial radius. Current screen-probe radiance is still derived from a 1-spp
`diffuseRadianceHitDist` source; Cornell cache coverage is low and coverage
rejects dominate. UE5.8's closer contract is a per-tap history-validity mask,
encoded reprojection coordinates, and fast-update behavior when the current
sample is invalid. The Falcor implementation should therefore:

1. expose a separate raw screen-radiance/history-validity diagnostic (keep
   hit-distance and age/count in separate resources);
2. add per-tap validity and reject-reason counters for depth, normal/material,
   motion, lighting generation, and current/previous invalid samples;
3. improve Surface Cache card coverage/extent and measure cache-hit versus
   coverage-reject before tuning history weights; and
4. capture static 1/8/32/96 plus camera-pan, camera-cut, light, material, and
   geometry transitions with linear EXR and display PNG pairs.

The quality gate is split: final fullColor must remain finite, non-negative,
and hash-equivalent under export policy; raw probe diagnostics must report
source variance and reject telemetry. A smoother probe-only PNG alone is not a
no-noise pass.

The first source-quality GPU attempt after the normal/alpha fix timed out during
Mogwai initialization and produced no manifest; this is intentionally recorded
as missing evidence. The shader-side fix is build-verified only until a clean
single-GPU run supplies the A2/C5 metrics.

The raw screen-radiance history now also carries a separate R32Uint validity
sidecar per ping-pong slot. Trace history taps require both the frozen
RGBA16F hit-distance contract and this validity bit; history age and lighting
generation remain separate resources. This removes implicit validity inference
from hit-distance/age fields without changing the radiance ABI. Release LumenGI
build and offline source-quality self-test pass; a clean Mogwai variance run is
still required before calling the source-noise gate closed.

## 2026-08-13 Radiance Cache CPU usage-feedback preparation

The CPU Radiance Cache contract now carries a separate `lastUsedFrame` usage
timestamp and a `touchProbe()` API. `query()` marks resident corners as used,
while `updateProbe()` seeds both update and usage timestamps. LRU selection uses
last consumption with update-age fallback; radiance payload, generation and
GPU-byte estimates remain unchanged. The Release CPU suite passes 24/24
`LumenRadianceCache_*` tests. This is a preparation step only: the Radiance
Cache checkbox still has no GPU producer, indirection atlas, trace/commit pass,
or production LumenGI output. C10 remains blocked until the Host-to-GPU
mark/allocate/trace/ready-next-frame/query chain is implemented and gated.

## Current visual comparison and next execution order (2026-08-13)

The production full-color branch is currently the clean reference: the
convergence material scene shows diffuse bounce, direct metal highlights, and
contact shadows. The ScreenProbe-only diagnostic remains visibly low-frequency
mottled on the Cornell wall because it still starts from the one-sample
`diffuseRadianceHitDist` producer; a smoother diagnostic image does not by
itself prove a noise-free producer. Arcade is opaque and has no glass or
transmission material, so it is suitable for emissive/color-bleed/shadow
comparison but not for a Lumen glass claim.

The next implementation order is deliberately dependency-driven:

1. **A2/C5 source quality:** run the new validity-sidecar and source-quality
   harness on a clean single-GPU process; add dynamic light/material/geometry
   PNG+EXR pairs and reject-reason telemetry. If cache coverage rejects remain
   dominant, fix card extent/coverage before changing blur radius.
2. **C6.2 release hardening:** keep the bounded sphere-array eviction result;
   add request-reason priority, longer soak, and explicit page-clear sentinel
   readback. Do not call aggregate request counts unique card pressure.
3. **C9 full-color release:** preserve the marked endpoint equivalence;
   the runtime manifest now records direct+indirect producers and a real
   mark-off/export execution. Low-bit GPU digest jitter is accepted only when
   endpoint mean/max agree within `1e-4`.
4. **C10 Radiance Cache GPU producer (Wave-1/2 PARTIAL):** runtime dispatch now
   has separate radianceCache/radianceCacheHitDist outputs and explicit
   producer/interpolator counters. The v14 artifact proved metadata acceptance;
   `artifacts/lumengi/C10/emissive-nee-smoke-20260813.json` now reports finite
   hit-distance coverage plus nonzero hit radiance (`rgbMax≈0.58`, hit fraction
   ≈2.68%) after eight deterministic probe rays and bounded analytic/emissive
   NEE. The `radianceCacheValidity` R32Uint sidecar separates hit/sky/radiance/
   producer bits and runtime shader compilation is clean. The later
   `resolve-wiring-producer-gate-20260814.json` also proves the optional cache
   fallback is bound into final resolve with `finalResolveConnected=true`.
   This remains a bounded fallback integration, not a complete UE cache pass:
   persistent GPU indirection/allocator commit, true query/fallback coverage,
   stale-write rejection under reuse, and multi-view/lighting gates remain
   open.
5. **Separate rough specular, then transmission:** follow UE's independent
   `RoughSpecularIndirect` history/resolve contract; implement IOR/Fresnel
   transmission as a separate producer. RTXDI direct specular and PathTracer
   NRD remain reference paths until those outputs enter Lumen finalColor.
6. **Release matrix:** GPU-wide VRAM, 30-minute dynamic and 2-hour soak,
   640/800/1280 multi-view timing, then C11 presets/C12 publish gates.

## 2026-08-14 execution update

The current evidence is now split explicitly by contract:

- **C5 grid equivalence:** same-scene full-scan versus grid telemetry is
  `PASS` at `artifacts/lumengi/C5/grid-equivalence-telemetry-20260814.json`;
  candidate preservation is measured, while high cache-coverage rejection
  remains an open quality item.
- **C6 bounded lifecycle:** the sphere-array pressure run is the authoritative
  bounded eviction evidence at
  `artifacts/lumengi/C6/sphere-array-pressure-20260813-v4/` (13 evictions,
  generation reuse, stale-owner rejects, page-clear and finite-output checks).
  Cornell tiny-atlas runs without distinct page demand remain `BLOCKED`; this
  is not weakened into a pass.
- **C10 producer/final resolve:** the strict runtime gate passes at
  `artifacts/lumengi/C10/resolve-wiring-producer-gate-20260814.json`.
  It proves producer/interpolator counters, N→N+1 readiness, typed validity /
  hit-distance semantics, and `finalResolveConnected=true`. It is still not a
  full UE Radiance Cache quality/lifecycle pass.
- **E1 rough specular:** the disabled-by-default directional shader and offline
  contract pass at `artifacts/lumengi/E1/rough-specular-gate/`.
  Host history/resource integration is intentionally still pending; RTXDI
  direct specular is not relabeled as Lumen indirect specular.
- **A2 source quality:** the clean source-quality run attempted on 2026-08-14
  timed out in Mogwai before writing a manifest. Therefore A2/C5 no-noise
  remains `BLOCKED`; existing screenshots still show a clean final composite
  versus a visibly mottled probe-only diagnostic. No timeout is treated as a
  quality pass.

The next executable order is therefore A2 source-quality evidence, C6 request
priority/soak, then rough-specular host integration and a separate transmission
producer. GPU-wide VRAM, dynamic soak, multiview performance, and C11/C12
release gates remain open. Do not shut down the machine until those gates are
either completed with artifacts or explicitly handed off as blocked.

C10 producer validity now has an explicit producer-side hit/sky record guard
and safe ray-direction fallback. The 3-frame ping-pong warmup runtime gate
passes at `artifacts/lumengi/C10/producer-compat-long-gate-20260814.json`;
the one-frame run correctly reports no ready samples, so the timing fence is
visible in the evidence.

The host stats now expose `queryAttempts` separately from `queryHits`; the
latter is not incremented without a GPU valid-hit counter. This prevents the
old full-frame dispatch count from being misreported as cache coverage.

## 2026-08-14 request-reason and transmission contracts

The Surface Cache request readback now preserves the GPU reason-bit OR instead
of dropping it. `surfaceCacheStats` exports separate unmapped, stale-owner,
metadata-invalid, and visibility-invalid request counters. The short runtime
smoke at `artifacts/lumengi/C6/reason-telemetry-smoke-20260814/` passes the
page schema and shows stale-owner requests explicitly; this is telemetry and
prioritization groundwork, not yet a long-soak or priority-quality pass.

The transmission wave is now represented by a standalone diagnostic contract
at `artifacts/lumengi/E1/transmission-gate/`. It carries IOR, thickness,
Fresnel, refraction direction, throughput validity, and a separate validity
mask, but is intentionally `REFERENCE_ONLY/UNSUPPORTED`: it is not in the
Host/CMake/finalColor production graph. A real glass claim still requires an
independent path producer and multi-view PathTracer comparison.

## 2026-08-14 C9 closure

The current full-color composite contract is now runtime-closed. Independent
mark-on and mark-off executions are stored under
`artifacts/lumengi/C9/runtime-finalcolor-current-20260814/` and
`artifacts/lumengi/C9/runtime-finalcolor-markoff-20260814/`. The strict report
`artifacts/lumengi/C9/finalcolor-contract/finalcolor-contract.json` is
`PASS`: direct RTXDI/NRD/DirectResolve (including emission) plus
`LumenGI.resolvedDiffuseGI` is finite, nonnegative, producer-tagged, and
export/mark-policy equivalent. Rough-specular and transmission remain separate
future producers and are not folded into this result.

The 2026-08-14 A2 temporal update replaces the fixed ScreenRadianceHistory
EMA with a bounded 10-frame accumulation weight and 0.10 minimum new-sample
weight. The Release shader and 96-frame image run are clean, and the
1280x720/60-frame production performance smoke records LumenGI GPU
p95/p99/max=9.665/9.692/9.762 ms. Raw source variance and Surface Cache
coverage remain independent quality gates; this temporal improvement does not
close them by itself.

## 2026-08-14 final bounded-gate delta

The C6 strict next-frame validator is now present at
`tests/lumengi/run_c6_nextframe_gate.py`. It accepts only explicit frame-N
request followed by frame-N+1 metadata-ready/capture-complete telemetry, and
rejects same-frame publish/hit, counter regressions, missing request fields,
or image-derived claims. Its fixture passes; the existing reason-telemetry
smoke is intentionally BLOCKED because it predates the frozen top-level
`requestRaw/requestCards/requestCaptureCompleted` schema.

C10 producer/interpolator validity and C9 full-color/export equivalence are
bounded runtime PASSes. A2 source-variance/no-noise remains BLOCKED after the
Mogwai source-quality attempts timed out before producing manifests. C5 grid
correctness passes, but cache coverage rejects remain high. Rough-specular is
disabled-by-default and diagnostic-only; transmission is
`REFERENCE_ONLY/UNSUPPORTED`. GPU-wide VRAM, dynamic/long soak, full
multi-view release matrix, and C11/C12 remain OPEN. These boundaries are
intentional and must not be promoted by a smooth screenshot or a CPU fixture.

The C6 host request path now defers miss-request scheduler insertion by one
host frame and exports `requestObservedFrame` / `requestCaptureFrame` stamps.
This is a provenance hardening step, not a Gate waiver: a fresh runtime report
must still demonstrate explicit N+1 publication and no same-frame cache hit.

The first post-change 320x180 ten-frame run is runtime-clean but remains
strictly BLOCKED: cumulative request/capture deltas and frame stamps are not
yet an unambiguous N-to-N+1 event series. The next fix must separate GPU
request observation, deferred scheduler enqueue, capture completion, and page
ready publication in the exported per-frame schema.

## 2026-08-14 C6 per-frame provenance follow-up

The duplicate `readbackScreenProbeCounters()` call inside
`runScreenProbeTrace()` was removed; Surface Cache feedback is now consumed
once, before scheduler capture. Host stats additionally export per-frame
`requestRawThisFrame`, `requestCardsThisFrame`,
`requestCaptureCompletedThisFrame`, `pageMetadataPendingThisFrame`, and
`pageMetadataReadyThisFrame`. The strict validator prefers these fields and
does not treat the dispatch-local `cacheLookupHits` counter as cumulative.

The Release build and runtime smoke are clean at
`artifacts/lumengi/C6/nextframe-runtime-v5-20260814/`, but the strict report
remains `BLOCKED`: the first request transition has a valid N+1 completion,
while later queued requests are delayed beyond one frame and no requested-page
ready event is observed. This is an honest bounded-budget/lifecycle gap, not a
threshold waiver. A follow-up must either emit card-specific queue/ready
events and prove bounded latency, or change the scheduler contract explicitly;
it must not infer readiness from aggregate resident pages.

For visual context, the adaptive history frame96 image at
`artifacts/lumengi/screenshots/history-adaptive-20260814/` is visibly smoother
than the probe-only mottle baseline in
`artifacts/lumengi/A2/reject-telemetry-20260813/`. This closes neither the raw
source-variance gate nor the dynamic image gate; the comparison is presentation
evidence only.

The E1 diagnostic endpoints are now RenderGraph-integrated. When
`LUMEN_RESOLVED_E1_DIAGNOSTIC_OUTPUTS=1`, the Release Mogwai run at
`artifacts/lumengi/screenshots/e1-diagnostic-runtime-20260814/` compiles and
writes `roughSpecularIndirect`, `roughSpecularValidity`, `transmissionIndirect`,
and `transmissionValidity` without D3D12 errors. Both passes are hard-disabled
by compile-time defines and remain outside `resolvedDiffuseGI`/finalColor;
runtime compilation is therefore closed, but the actual directional rough-
specular and medium-aware transmission producers are still not implemented.

The 64x36 A2 source-quality retry (`diag-off-20260814`) again timed out before
the script emitted its progress manifest, even with one probe direction and
two checkpoints. The Mogwai process was terminated explicitly and no GPU
process remains. This confirms an execution/initialization blocker, not a
quality pass; raw source variance and dynamic image gates stay BLOCKED.

The bounded C10 consumer-validity smoke is recorded at
`artifacts/lumengi/C10/final-validity-consumer-gate-20260814.json`: the
hit/sky validity sidecar, hit-distance semantics, frame-N to frame-N+1 ready
pair, and final-resolve wiring all pass without using RGB-nonzero as a
validity proxy. This is a producer-contract smoke, not proof of broad
Radiance Cache coverage or UE-equivalent lighting quality.

A controlled three-frame C6 run at
`artifacts/lumengi/C6/nextframe-runtime-smoke-3f-20260814/nextframe-gate.json`
also passes the strict first request-to-next-frame publication check. The
longer ten-plus-frame reports remain BLOCKED because later queued cards do
not yet provide an unambiguous bounded-latency ready series. The short smoke
must therefore remain labelled bounded, not full lifecycle closure.
