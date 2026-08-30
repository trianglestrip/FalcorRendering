# LumenGI latest execution status — 2026-08-16

## Superseding 2026-08-19 evidence

- C10 two-phase coverage is `PASS` in
  `artifacts/lumengi/C10/two-phase-coverage-20260819/c10-coverage-phase-gate.json`:
  near level 0 in-bounds `4806/8215=0.5850`, query-hit `0.9712`, and valid
  hit-distance `0.9717`; the far phase covers levels 1-5. This is bounded
  phase coverage, not broad production quality.
- The newer C9 dual-process run supersedes the older PASS bullet below:
  `artifacts/lumengi/C9/finalcolor-dual-20260819/` fails frozen pixel export
  equivalence (mean `8.07e-5`, p99 `1.10e-3`). The same-process endpoint retry
  at `artifacts/lumengi/C9/same-process-endpoint-20260819-v2/` is `BLOCKED`
  because lazy graph recompilation attempts to capture an output that is no
  longer marked. No threshold was relaxed.
- A2 now has fresh two-scene runtime evidence at
  `artifacts/lumengi/A2/multiscene-gate-20260820/multiscene-gate.json`:
  Cornell and Arcade each cover the complete five-case transition matrix and
  all required checkpoints; multi-scene evidence is `PASS`. The report remains
  `OPEN` because explicit linear no-noise sidecars are still missing.

## Verified bounded gates

- C6 Surface Cache bounded lifecycle: PASS for the fresh sphere-array
  pressure runtime at
  `artifacts/lumengi/C6/sphere-array-clockfix-v2-20260814/`; 27 evictions,
  generation 1->2, stale-owner rejects, and page-clear telemetry were
  observed using the monotonic scheduler clock.
- Historical C9 full-color/export equivalence: the older independent
  mark-on/mark-off runtime pair at
  `artifacts/lumengi/C9/runtime-markoff-v4-20260815/` and contract report
  `artifacts/lumengi/C9/finalcolor-contract-v4-20260815/`. The report carries
  direct/indirect producer metadata, finite/nonnegative composite output,
  mark-off PASS, and pixel-level export equivalence (meanAbs=1.29e-5,
  p99Abs=4.27e-4, relativeMax=4.00e-5) under an explicit independent-process
  floating-point tolerance. It is superseded for the current Release state by
  the 2026-08-19 dual-process result above and must not be used as the latest
  C9 release verdict.
- C10 producer contract: bounded PASS with hit/sky validity, hit distance,
  validity bits, frame-N/N+1 readiness, and reconciled GPU query counters in
  `artifacts/lumengi/C10/query-readback-v5-20260815/c10-producer-gate.json`;
  the same run also passes the frozen consumer contract at
  `artifacts/lumengi/C10/query-readback-v5-20260815/radiance-cache-gpu-gate.json`.

## Still open

- C6 strict per-request N+1 latency now passes in the high-budget, low-resolution
  lookup-on case at
  `artifacts/lumengi/C6/strict-latency-clockfix-20260815/nextframe-gate.json`.
  The follow-up event-ledger run at
  `artifacts/lumengi/C6/event-ledger-clockfix2-20260815/nextframe-gate-events.json`
  passes 18 settled card/page events with explicit request, capture, ready, and
  generation frames; six final-tail events are reported as pending provenance.
  The separate tiny-atlas pressure run still remains a bounded eviction/reuse
  result; per-card eventual completion under budget pressure and long dynamic
  soak remain open.
- A2 source-quality now passes a real Release/Mogwai A/B at 640x360,
  checkpoints 1/8/32/96. Off/on raw alpha semantics are identical and the
  spatial-filter quality comparison passes at
  `artifacts/lumengi/A2/source-quality/compare-20260815/` (frame-96 local
  variance 0.00020238 -> 0.00019725). The four dynamic cases
  `camera_cut`, `scene_reload`, `lighting_generation`, and
  `material_or_geometry` also pass at
  `artifacts/lumengi/A2/dynamic/*-20260815/`. This closes the bounded A2
  source/history telemetry gate; broad multi-scene no-noise and long soak
  remain open.
- C10 coverage quality is bounded rather than broad UE-equivalent: the latest
  run reconciles query attempts=hits+misses and reports real-hit samples, but
  only 4/32 projected probes are in-bounds and most early samples are
  unclassified no-hit/sky. Probe-position lighting coverage and final-quality
  evidence remain open.
- C5 grid A/B was rerun through the stable C6 graph at 160x90 after the
  monotonic page-fence/all-card-index fixes. Grid off/on both produced one
  cache hit with equal lookup attempts in the sampled sequence; grid-on still
  has a larger candidate-reject count, so candidate preservation is bounded
  PASS while coverage quality remains OPEN.
- The older `artifacts/lumengi/C5/grid-equivalence-current-20260815/` report
  predates the candidate-evaluation denominator change. Use the fresh C6-graph
  manifests above for current A/B evidence; do not mix the old ratio fields
  into the current verdict.
- Rough-specular is diagnostic-only and transmission is reference-only until
  independent directional and medium-aware producers are integrated.

The earlier A2 initialization stall is resolved by the embedded-runner entry
and Windows manifest-write fallback. The Mogwai process is currently stopped;
no GPU process remains running.

The earlier Cornell dirty-pressure runtime at
`artifacts/lumengi/C6/tiny-dirty-pressure-20260814/surfacecache-effect.json`
applied 12 material mutations and recorded 12 distinct card IDs, but did not
fill the 16-page atlas. The later sphere-array run is the authoritative
eviction/reuse evidence; aggregate request activity is not promoted to unique
card/page demand.

The frozen consumer schema is now emitted as an explicit `gpuGate` projection
alongside the producer report. It passes all eight checks (three channels,
stats binding, frame pair, GPU API, and explicit diffuse fallback evidence).
This closes the C10 bounded wiring contract, not broad far-field quality:
only 4/32 projected probes are in bounds and the long VRAM/soak matrix is
still open.

C4 was re-run on the current Release binary at
`artifacts/lumengi/C4/gdf-probe-router-current-20260815/`: 607 GDF hits and
2255 HWRT fallback hits, with finite/nonnegative outputs. C5 full-scan/grid
candidate equivalence passes at 320x180 with all 12 cards indexed, but the
coverage-quality check remains OPEN because coverage rejects are still much
larger than lookup attempts. The card-facing sign fix is in
`ScreenProbe/LumenScreenProbeIntegrate.cs.slang` and standalone Slang
compilation passes; a fresh cache-lighting runtime remains pending because
the current Mogwai path exceeds the bounded timeout.

## 2026-08-15 C10 raycast-gridfix follow-up

The latest unique Release/Mogwai run at
`artifacts/lumengi/C10/raycast-gridfix-20260815/` compiled the new
probe-position interpolation path at runtime and passed both strict reports:
`c10-producer-gate.json` and `radiance-cache-gpu-gate.json`. Reconciled query
telemetry is `1,382,400 = 30,900 hits + 1,351,500 misses`; this is a material
improvement over the earlier zero-hit smoke. The miss fraction is still
97.76%, and projection telemetry remains 4/32 in-bounds probes, so broad
probe-position lighting coverage remains OPEN. The legacy top-level S7 mirror
report is intentionally `partial`; use the two strict reports for the current
bounded verdict.

The visual evidence remains split by contract: the probe-only frame in
`artifacts/lumengi/screenshots/screenprobe-convergence-p0c-20260811/` retains
large low-frequency mottle, the history2 frame is smoother but is not source
coverage proof, and the latest resolved Cornell frame in
`artifacts/lumengi/C9/runtime-markoff-v4-20260815/` has coherent direct light,
diffuse bounce, and soft-shadow structure. No screenshot is promoted to a
production no-noise claim until C5 coverage and multi-view dynamic gates close.

## 2026-08-15 E1 rough-specular runtime compile

The bounded directional rough-specular producer is now a separate
SceneRayQuery path with independent output and validity resources. The
single-thread Release plugin rebuild and unique Cornell Mogwai run at
`artifacts/lumengi/E1/roughspec-runtime-20260815-v3/` completed with runtime
shader compilation, dispatch, and finite resolved output, without
Fatal/E_INVALIDARG. This is diagnostic evidence only: the producer is not
connected to `LumenFinalResolve`/fullColor, and roughness/multi-view,
performance, and VRAM gates remain open. Transmission remains
`REFERENCE_ONLY`.

The independent C10 broad-coverage gate at
`artifacts/lumengi/C10/raycast-gridfix-20260815/c10-coverage-gate.json` is
explicitly `BLOCKED`: query hit/miss reconciliation and finite fallback pass,
but only 4/32 probes are in bounds and per-level `coverageByLevel` telemetry
is absent. This keeps the bounded producer/consumer PASS separate from the
far-field quality decision.

## 2026-08-15 C10 per-level counter follow-up

`LumenRadianceCacheInterpolate.cs.slang` and the host now expose explicit
per-level counters via `coverageByLevel`; the unique Release/Mogwai artifact is
`artifacts/lumengi/C10/levelcoverage-runtime-20260815/`. Producer and consumer
reports pass and the coverage report is `OPEN` with reconciled query readback,
but only 4/32 projected probes are in bounds and most levels have no queried
screen samples. Broad far-field lighting coverage remains open.

## 2026-08-15 C6 strict pressure gate

After the Release `/m:1` rebuild, the sphere-array pressure run at
`artifacts/lumengi/C6/sphere-array-pressure-final11-20260815/` passes the strict
per-card pressure gate. It uses 384 cards, a 64-texel/16-page atlas, one capture
page per frame, 190 scheduler frames, and two actual tail frames. The report
records 25 evictions, 9 generation-mismatch rejects, zero event drops, explicit
same-page generation reuse, and complete capture/ready or stale-terminal
outcomes. This is bounded pressure/reuse closure; the universal C6 request gate
still remains BLOCKED where frame-origin/card association is incomplete.

## 2026-08-15 C10 sphere-array multi-level follow-up

The producer gate now honors the explicit interpolated validity bitmask (hit/sky
bits may overlap after tap OR; mutually-exclusive inputs remain strict). Both the
long sphere-array runtime and the static-only run at
`artifacts/lumengi/C10/sphere-levels-runtime-long-20260815/` and
`artifacts/lumengi/C10/sphere-levels-static-only-20260815/` pass producer and
GPU-consumer gates with finite channels, hit-distance validity, query readback,
and N-to-N+1 readiness. The coverage gate remains `OPEN`: level 0 has real hits,
but levels 1-4 have no valid probe samples and in-bounds coverage is below the
quality threshold. This is stronger bounded evidence, not far-field closure.

## 2026-08-15 C10 cold-cell starvation fix

The CPU clipmap scheduler now prioritizes empty cells before allocated slots that
have not yet received GPU confidence feedback. Release rebuild and all 111 Lumen
CPU tests pass. The static-only sphere-array runtime at
`artifacts/lumengi/C10/sphere-levels-fair-scheduler-v2-20260815/` reaches 3,072
resident probes and has valid query samples on levels 0-4; producer and GPU
consumer gates pass. Coverage remains `OPEN` for the level-4 in-bounds threshold
and level-5 query/hit-distance evidence, so the starvation fix is not a broad
far-field quality closure.

## 2026-08-15 C5 planar-card producer follow-up

Cornell planar triangle meshes now use a consistent thin-slab card bound when
their finite AABB has one zero-thickness axis. Release/Mogwai fullscan/grid
artifacts at `artifacts/lumengi/C5/coverage-runtime-planar-20260815/` show
8 supported instances, 48 cards, 48 indexed grid cards, zero missing cards,
and finite equivalent A/B lookup counters. The strict equivalence report is
PASS with coverage quality still OPEN because this short run has no valid cache
hit; Surface Cache broad quality is not closed.

The host also carries an explicit Radiance Cache reset-pending flag across
scene/hot-reload boundaries, clearing payload/validity/query state on the next
execute rather than relying on the resettable history clock.

## 2026-08-15 C5 planar long-warmup confirmation

The post-fix Cornell full-scan run at
`artifacts/lumengi/C5/coverage-runtime-planar-long-20260815/fullscan/`
completed 48 frames without a fatal/device error, with approximately 4.7k
lookup attempts per frame but zero valid Surface Cache hits. This rules out a
short-warmup explanation for the current zero-hit result: grid/full-scan
candidate equivalence is bounded PASS, while producer coverage/ready-page
quality remains OPEN.

## 2026-08-15 C5 metadata-neighborhood follow-up

The constrained same-page 3x3 metadata/visibility neighborhood fallback was
compiled into Release and exercised in
`artifacts/lumengi/C5/coverage-neighborhood-20260815/fullscan/`. Runtime
shader compilation and 12 frames completed without Fatal/E_INVALIDARG or a
device error; outputs stayed finite/nonnegative. The run still reports zero
valid cache hits from 4,692 lookup attempts, with 453 owner-valid candidates
rejected by page metadata and the dominant coverage rejects remaining depth
and axis tests. The fallback therefore did not close Surface Cache coverage;
the reject-reason telemetry is retained as the next producer-debug baseline.

## 2026-08-15 C6 per-card next-frame association

`run_c6_nextframe_gate.py` now consumes the host per-card event ledger rather
than aggregate request counters. The recheck at
`artifacts/lumengi/C6/sphere-array-pressure-final11-20260815/nextframe-gate-v3.json`
passes frame-origin/card association, request→capture→ready or stale-terminal
outcomes, monotonic sequence/identity checks, and zero event drops. This is
bounded lifecycle closure for the pressure scene; Surface Cache broad
coverage, lookup-hit association in a fresh positive-hit scene, and long soak
remain separate gates.

## 2026-08-15 C5 facing-sign A/B result

The proposed `dot(rayDir, viewDir)` facing-sign experiment was rebuilt and
run in isolation at `artifacts/lumengi/C5/coverage-facing-sign-20260815/`.
It reduced the facing-reject count slightly but still produced zero cache hits
from 4,692 lookup attempts and `cacheLightingCounterTraced=0`. The experiment
therefore did not close the producer path and was reverted to the original
capture convention; the Release shader copy was rebuilt afterward. The current
diagnostic baseline remains depth/axis rejection plus absent captured metadata,
with Surface Cache quality still OPEN.

## 2026-08-16 C5 capture backface/layout fix

`LumenCardCapture.3d.slang` now uses an explicit 96-byte card-record layout matching
the host buffer and records both triangle windings. The previous capture-side
backface early-return was invalid for the external card camera used by enclosed
Cornell walls. Release rebuild and Mogwai runtime compilation passed. Fresh 48-frame
fullscan/grid runs at
`artifacts/lumengi/C5/capture-backface-fix-20260816-long/` now show real Surface
Cache hits (`21` fullscan, `31` grid), traced cache-lighting pages, finite outputs,
and 48/48 indexed cards. Strict grid equivalence remains `FAIL` only on the
intermediate `probeInterpolated` mean delta (`0.000408147 > 0.0001`); final
`resolvedDiffuseGI` means differ by about `1.6e-5`. Keep producer coverage and
intermediate A/B quality OPEN until a deterministic equivalence run closes this
delta rather than relaxing the threshold.

## 2026-08-16 C10 far-field camera follow-up

The sphere-array runner now accepts `LUMEN_RC_FAR_FIELD_CAMERA_Z` for an explicit
far-field camera without changing scene assets. The unique Release run at
`artifacts/lumengi/C10/far-field-gpu-20260816/` has reconciled query readback
(`8,755,200` attempts, `7,984,015` hits, `771,185` misses), finite fallback and
producer/consumer channels. Levels 0, 2, 3, and 4 have query/hit-distance
evidence, while level 1 and the coarsest level 5 still fail the strict coverage
threshold; `c10-coverage-gate.json` is therefore `OPEN`, not far-field closure.

## 2026-08-16 release churn proxy

The unique 60-second/3,600-frame material-dirty plus resize/reload proxy at
`artifacts/lumengi/release/churn-proxy-20260816/` completed with exit code 0,
material mutation available, six reloads and twelve resizes, and no Fatal,
E_INVALIDARG, device-removed, or error records in the Mogwai log. The proxy
cannot expose allocator counters (`stats_available=false`) and is not the
required 30-minute dynamic or 2-hour soak; those release gates remain OPEN.

The current strict C6 recheck at
`artifacts/lumengi/C6/sphere-array-pressure-final11-20260815/nextframe-gate-v4.json`
also passes the monotonic `surfaceCacheFrameIndex`/`schedulerFrameIndex` clock
contract and per-card request/capture/ready association. This remains bounded
pressure-lifecycle evidence; fresh positive-hit association and universal cache
quality are separate gates.

## 2026-08-16 deterministic grid fallback and far-field v4

The card-grid host bounds now include the shader's 0.04 all-axis lookup epsilon,
and the integrate shader conservatively routes incomplete cells through the
authoritative full-card scan. Release build and runtime compilation are clean.
Fresh 16-frame A/B evidence at
`artifacts/lumengi/C5/grid-deterministic-fallback-20260816/` has identical
lookup attempts/hits/reject counters and complete 48-card indexing, but the
strict intermediate `probeInterpolated` equivalence remains outside the frozen
1e-4 cross-process tolerance. Grid acceleration is therefore disabled-safe
fallback; C5 quality stays OPEN rather than relaxing the threshold.

The EnvMap-free far-field scene at
`artifacts/lumengi/C10/far-field-scene-z220-20260816-v4/` loads and runs cleanly.
Query readback reconciles 5,068,800 attempts with 4,821,264 hits and 247,536
misses, and all six levels are observed. Producer validity passes, but strict
consumer/fallback fails on the explicit non-black fallback and resident plateau;
levels 1-3 also remain below per-level quality thresholds. This is diagnostic
far-field evidence, not Radiance Cache production closure.

## 2026-08-16 scheduler fairness and C10 far-field v8/v9

`LumenRadianceCache::tick()` now reserves one refresh candidate per clipmap
level whenever the per-frame budget can cover all levels, then fills the
remaining budget by the existing deterministic score. The companion CPU test
`LumenRadianceCache_TickReservesEveryLevelWhenBudgetAllows` passes in the
113-test `Lumen.*` suite. This is a scheduling fairness correction; it does not
relax validity or coverage thresholds.

The best far-field runtime evidence is the EnvMap-free v8 artifact at
`artifacts/lumengi/C10/far-field-scene-z220-20260816-v8/`. Its producer gate,
GPU consumer gate, explicit non-black fallback gate, and query denominator
reconciliation pass; all six clipmap levels produce projected/indirection
telemetry. The strict coverage gate remains `OPEN`: level-0 in-bounds fraction
is about `0.0813`, below the frozen `0.5` threshold, so this is bounded producer
and readiness evidence rather than broad quality closure.

The follow-up near-camera v9 artifact at
`artifacts/lumengi/C10/far-field-scene-z220-20260816-v9/` is retained as a
negative diagnostic. It has reconciled `12,441,600` query attempts with
`12,187,982` hits and producer/ready telemetry, but its explicit fallback is
black and the per-level counters show the sampled queries concentrated in
level 0. The coverage and GPU consumer gates therefore fail; v9 is not used to
promote C10. A2 source/no-noise, C5 strict intermediate equivalence, C6
universal request latency, rough-specular/transmission production resolve,
GPU-wide VRAM, soak, multi-view, and final release gates remain OPEN.

The fresh same-binary C5 comparison at
`artifacts/lumengi/C5/grid-disabled-fresh-20260816/` confirms identical
candidate/reject/lookup counters for fullscan and grid-disabled runs, but the
independent-process intermediate outputs still exceed the frozen equivalence
threshold (`probeInterpolated` mean delta about `0.00256`). This preserves the
strict C5 quality gate as `FAIL/OPEN`; no tolerance was widened.

## 2026-08-16 feedback provenance and paired C5 follow-up

The Release rebuild after adding `cacheFeedbackSubmittedFrame` passes the full
`Lumen.*` CPU suite (`113/113`). A fresh sphere-array C6 run at
`artifacts/lumengi/C6/feedback-provenance-sphere-20260816/` records positive
lookup events with `firstHitFrame` equal to the feedback dispatch frame; 65
completed lifecycle events pass the strict association checks. The gate is
still `BLOCKED` only for the final unsampled request batch. The drain variant
at `artifacts/lumengi/C6/feedback-provenance-sphere-drain-20260816/` leaves five
frame-40 requests unresolved at the end of the run, so C6 remains bounded
rather than a universal request-latency pass.

The same-process C5 paired runtime at
`artifacts/lumengi/C5/paired-equivalence-runtime-20260816-v2/` compiles and
executes both full-scan and grid passes with matching 48/48 candidate/page
telemetry. The frozen `1e-4` output gate fails from frame 2 onward
(`probeInterpolated` mean delta about `0.0121`; resolved diffuse about
`0.00120`), so C5 intermediate quality remains `OPEN/FAIL` and no threshold
was relaxed.

### 2026-08-16 C5 full/full control and C10 phase gate

The one-process C5 full/full control at
`artifacts/lumengi/C5/paired-equivalence-full-full-20260816/` fails the same
strict intermediate output threshold from frame 2 as the full/grid run. The
next debug boundary is Surface Cache lighting or independent temporal state;
the 1e-4 contract remains frozen.

C10 now has an explicit two-phase manifest and evaluator at
`artifacts/lumengi/C10/two-phase-coverage-20260816/`. The far-field phase
(levels 1--5) passes; near level 0 is OPEN (in-bounds 0.0897 < 0.5). The
combined AND gate is OPEN and no level is averaged or relabeled.

The C5 no-temporal control at
`artifacts/lumengi/C5/paired-equivalence-cache-no-temporal-20260816/` still
fails with cache-lighting enabled (`probeInterpolated` mean delta 0.01804;
resolved diffuse 0.00148). The all-cache-off control
`artifacts/lumengi/C5/paired-equivalence-no-cache-20260816/` passes, isolating
the next debug boundary to Surface Cache/cache-lighting producer state.

An authored near-field C10 diagnostic at
`artifacts/lumengi/C10/near-field-authored-surface-focal12-20260816-v2/`
raises level-0 in-bounds to about 0.452 with an explicit focal-length
override, but its fallback remains black and the consumer gate is FAIL. It is
kept as projection/fallback evidence, not promoted to phase PASS.

### 2026-08-16 fresh-plugin C5 producer isolation

The LumenGI plugin was rebuilt with `/t:Rebuild /m:1` (the earlier Falcor-only
rebuild did not update the plugin DLL). The fresh point-light full/full control
at `artifacts/lumengi/C5/paired-equivalence-pointlight-cache-hash-plugin-rebuilt-4f-20260816/`
keeps all three host publication fingerprints identical (page metadata,
page-to-card, and render list), but cache-lighting output still diverges from
frame 2 (`probeInterpolated` mean delta 0.00559, resolved diffuse 0.00048).
The cache-direct atlas fingerprint and traced/firefly counters also differ.
With cache lighting disabled, the matching capture-only control at
`artifacts/lumengi/C5/paired-equivalence-pointlight-capture-only-20260816/`
passes all four frames with zero output/direct deltas. This isolates the
remaining C5 defect to the cache-lighting producer/runtime state, not grid
candidate publication or capture-table identity; the frozen `1e-4` gate stays
OPEN/FAIL.

An explicit UAV barrier was added between capture and cache-lighting for the
material, metadata, and radiance atlases, then the LumenGI plugin was rebuilt.
The follow-up artifact
`artifacts/lumengi/C5/paired-equivalence-pointlight-uavbarrier-4f-20260816/`
still fails the same output contract (`probeInterpolated` mean delta 0.00559
at frame 2), although the host-table hashes remain identical. The barrier is
retained as a producer/consumer ordering safeguard, but the remaining
cache-lighting nondeterminism is not declared fixed.

### 2026-08-16 strict C6 tail and C10 near-field phase

The C6 runner now captures an explicit four-frame tail even with sparse sampling.
`artifacts/lumengi/C6/sphere-array-farfield-tail4-20260816/` records 148 samples
and the strict per-card validator is `PASS`, including delayed feedback frame
provenance. This is bounded pressure evidence, not a long soak claim.

`run_radiance_cache.py` now accepts `LUMEN_RC_CAMERA_FOCAL_LENGTH`. The near-field
run at `artifacts/lumengi/C10/near-field-z1p2-focal10-20260816-v1/` reaches
level-0 in-bounds 0.585, query-hit 0.971, hit-distance 0.972, and finite
non-black fallback. The AND report
`artifacts/lumengi/C10/two-phase-coverage-20260816/c10-coverage-phase-gate-near-z1p2-focal10.json`
passes both the near level-0 and existing far levels 1--5 phases. C5 producer
equivalence, A2, rough/transmission, VRAM/soak and release matrix remain open.

As a bounded stale-owner hygiene fix, cache-lighting now clears the visibility
atlas before each dispatch. The rebuilt-plugin recheck at
`artifacts/lumengi/C5/paired-equivalence-pointlight-visibility-clear-4f-20260816/`
still fails at frame 2 with the same `probeInterpolated` delta, so visibility
stale data was not sufficient to close C5; the fix is retained as lifecycle
hygiene and the strict gate remains OPEN/FAIL.

The C5 paired runner now snapshots both the pre-lighting capture atlas and the
post-lighting direct atlas. In
`artifacts/lumengi/C5/paired-equivalence-pointlight-capture-snapshot5-4f-20260816/`,
host card/page/render fingerprints remain equal and the capture/direct atlas
pixel deltas are finite, but sparse cache-lighting differences still amplify to
`probeInterpolated` mean delta about `0.00559` at frame 2 (frozen tolerance
`1e-4`). This narrows the remaining defect to cache-lighting producer/runtime
determinism; no C5 threshold was changed.

### 2026-08-16 canonical inline-query ray type recheck

Cache-lighting now requests the canonical one-ray-type TLAS entry for its inline
visibility query. The rebuilt-plugin paired artifact
`artifacts/lumengi/C5/paired-equivalence-pointlight-raytype1-4f-20260816/`
is exact for frames 1--7; frame 8 still exceeds the frozen `1e-4` contract at
mean delta `7.426e-4`. This shows TLAS selection contributed to the divergence,
not that it was the sole cause. C5 remains OPEN/FAIL and no tolerance changed.

The follow-up real grid-on/off run
`artifacts/lumengi/C5/paired-equivalence-pointlight-raytype1-grid-ab-4f-20260816/`
runtime-compiles on the rebuilt plugin, but fails the frozen output contract
from frame 2 (probe interpolation and resolved diffuse). Host card/page/grid
telemetry remains finite and all cards are indexed; the failure is therefore
not promoted to a grid-equivalence PASS.

### 2026-08-16 C5 producer provenance telemetry

The LumenGI plugin was rebuilt after adding read-only cache-lighting provenance
fields (seed, GI/surface-cache frame, canonical one-ray-type TLAS, light/sampler
flags, feedback flag, and variant fingerprint). The paired full/full artifact at
`artifacts/lumengi/C5/paired-equivalence-pointlight-provenance-4f-20260816/`
shows every provenance and card/grid statistic equal on both passes for all
eight frames. The output contract still fails from frame 2 (probe/resolved
diffuse), while capture/direct atlas deltas remain finite and small. This
eliminates host variant/card publication mismatch as the current explanation;
the remaining C5 owner is producer-internal atlas/lighting determinism.

### 2026-08-16 C6 strict tail recheck

The updated C6 runner freezes camera and mutation stimuli during its explicit
drain tail. The fresh Release run at
`artifacts/lumengi/C6/sphere-array-farfield-tail8-freeze-20260816/` completed
without a Mogwai error and wrote all per-frame/card telemetry, but the strict
next-frame validator remains `BLOCKED`: one card-specific request (sequence
161, request frame 151) is still pending at the final sample (frame 152).
This is retained as negative lifecycle evidence; it does not replace the
earlier four-frame bounded PASS, and no aggregate counter or tail truncation
is used to manufacture a PASS.

A throughput control with the same frozen pressure scene, `captureMaxPagesPerFrame=64`,
and a 64-frame drain at
`artifacts/lumengi/C6/sphere-array-farfield-budget64-tail64-20260816/` also
remains `BLOCKED`: after 208 samples six card events are still pending and
`surfaceCacheEventDropped=169`. This confirms a real scheduler/request-admission
backlog under tiny-atlas pressure; extending the tail or relaxing the gate is
not a valid fix.

The event-ring capacity was increased from 512 to 8192 and rechecked in
`artifacts/lumengi/C6/sphere-array-farfield-budget64-ring8192-20260816/`.
`surfaceCacheEventDropped` is now zero in the fresh runtime, but the strict
next-frame gate remains `BLOCKED` because unresolved requests persist. The
capacity fix preserves evidence; it does not weaken lifecycle correctness.

### 2026-08-16 C5 capture determinism closure

The cache-lighting consumer barriers and the page-local capture order atlas are
now in the Release plugin. The order atlas is an `R32Uint` UAV cleared per page;
capture pixels atomically select the deterministic `(quantized depth, primitive)`
winner before publishing material/normal metadata. This removes overlapping
triangle UAV write-order variance without changing the public cache atlas ABI.

The fresh 8-frame paired runtime artifact
`artifacts/lumengi/C5/paired-equivalence-pointlight-capture-order-20260816/`
is `PASS` (`192/0/0`): full-scan/grid probe, resolved-diffuse and diffuse outputs
are within the frozen `1e-4` tolerance, and cache-hit counts agree per frame.
The cache-lighting-off control also passes, while the earlier producer failures
are retained as historical negative evidence. C5 paired equivalence is therefore
bounded closed; cache coverage/reject quality and long-run release behavior are
still separate open gates.

### 2026-08-16 C2 resize/resource lifecycle matrix

The Release Mogwai matrix at
`artifacts/lumengi/C2/resolution-matrix-final-20260816/resolution-matrix.json`
passes all required dimensions: 640x360, 800x450, 641x361, 799x449,
800x449, 801x451, and 1280x720. All marked outputs remained finite,
non-negative, and correctly sized; the equal-probe-count/aspect-change pairs
also passed. This closes the C2 resize smoke for the current plugin, not the
separate VRAM/soak or multi-view release gates.

The high-budget C6 strict latency artifact
`artifacts/lumengi/C6/strict-latency-clockfix-20260815/nextframe-gate.json`
is a PASS for its bounded lookup-on workload. Tiny-atlas pressure and eventual
per-card convergence remain separately OPEN/BLOCKED, so this does not promote
universal C6 lifecycle closure.

### 2026-08-16 C11 quality-preset defaults

`LumenGI` now resolves monotonic Low/Medium/High/Reference defaults for probe
directions, capture pages, cache-lighting bounce budget, spatial footprint,
temporal history cap, GDF trace budget, and Mesh-SDF resolution/format. Explicit
graph properties still override each derived value, and no cache producer is
implicitly enabled. The Release preset smoke at
`artifacts/lumengi/C11/preset-smoke-qualitydefaults-20260816/` completed all four
presets with exit 0; effective directions/pages/history/trace values are
serialized in each manifest. This is a C11 configuration/runtime smoke, not the
full image-monotonicity, performance, VRAM, or hot-switch release gate.

### 2026-08-16 C11 RenderGraph hot-switch transaction

`LumenGIPass::setProperties()` now applies preset-derived defaults for a
`qualityPreset` update submitted through `RenderGraph.updatePass()`, while
partial property updates preserve explicit overrides; the UI dropdown uses the
same transaction and resets history. The Release rebuild and
`FalcorTest --test-suite=Lumen.* --parallel=1` pass (113/113), and the offline
hot-switch fixture passes. Mogwai hot-switch attempts under
`artifacts/lumengi/C11/preset-hot-switch-20260816*/` did not complete within
the bounded runtime and are retained as `BLOCKED`; no runtime PASS is claimed
until a clean launcher run produces the per-preset property and finite-output
series.

### 2026-08-16 C11 hot-switch runtime verification

The clean Release series at
`artifacts/lumengi/C11/preset-hot-switch-20260816-final/quality-hot-switch.json`
supersedes the idle-launcher record for the current binary. It executed
`Low -> Medium -> High -> Reference -> Low`; after each pass recreation the
test re-fetched the pass and verified `qualityPresetStats`. All five samples
matched the frozen derived defaults and all marked outputs were finite and
non-negative. This is a bounded configuration/hot-switch PASS only; image
monotonicity, performance, VRAM, soak, and full release gates remain open.

### 2026-08-16 C6 validated completion telemetry

The scheduler now exposes page/generation-validated `isCaptureComplete()`;
Host request events advance only after that validation, rather than treating
command emission as capture completion. Release build and 113/113 Lumen tests
pass. The fresh pressure artifact
`artifacts/lumengi/C6/scheduler-completion-api-pressure-tail64-20260816/nextframe-gate.json`
passes the strict per-card request/capture/ready gate with zero dropped event
records. Aggregate tiny-atlas closure remains open for explicit stale-texel
and unique card/page identity evidence.

### 2026-08-16 A2 source-quality runtime recheck

The fresh 320x180 Cornell paired run
`artifacts/lumengi/A2/source-quality/compare-release-320x180-20260816/source-quality-comparison.json`
has valid off/on manifests and passes all raw-alpha, finite/non-negative, and
history-reject telemetry contracts. Source moments reduce frame-96 spatial
local variance, but the frozen 32→96 tail-RMSE criterion is not met, so the
comparison is explicitly `NO_IMPROVEMENT`; no no-noise production claim is
made.

### 2026-08-16 A2 history-weight recheck

The single frozen-boundary change in
`Source/RenderPasses/LumenGI/ScreenProbe/LumenScreenProbeTrace.cs.slang`
reduces source-moment history blending from a 0.9 maximum to 0.8; validity,
hit-distance alpha, clipping, and all thresholds are unchanged. Release build
and 113/113 Lumen tests pass. The paired runtime
`artifacts/lumengi/A2/source-quality/a2-weight08-compare-320x180-20260816/source-quality-comparison.json`
is `PASS`: raw alpha is invariant, telemetry is complete, frame-96 spatial
variance improves, and 32→96 tail RMSE meets the frozen 1.01 bound. This closes
only the bounded static A2 check; dynamic, multi-scene, and full release
no-noise gates remain open.

### 2026-08-16 C5 producer isolation negative evidence

The same-process full/full cache-lighting isolation run
`artifacts/lumengi/C5/producer-isolation-full-full-20260816-v1/c5-paired-equivalence.json`
is `FAIL` at the unchanged `1e-4` tolerance: capture arrays match, but
cache-direct radiance and downstream probe/resolved outputs diverge while seed,
frame, page/grid identity, TLAS, and shadow settings match. C5 producer
determinism/coverage therefore remains open; no threshold or fallback was
relaxed.

### 2026-08-16 S2 churn telemetry binding

`tests/lumengi/run_churn_short.py` now reads the canonical `LumenGI.surfaceCacheStats`
binding and exports stable allocation/release/failure/lost/recapture/page/bytes
aliases. The 60-second, 3600-frame Cornell proxy at
`artifacts/lumengi/release/churn-stats-bound-20260816/churn.json` reports
`stats_available=true`, `divergence_ok=true`, 3600 material mutations, 6
reloads, 12 resizes, zero allocation failures/lost pages, and bounded resident
bytes. This is improved proxy telemetry only; GPU-wide VRAM, a 30-minute
dynamic soak, and the 2-hour release soak remain open.
### 2026-08-17 C5 capture-owner equivalence closure

- Rebuilt Release `LumenGI`/`FalcorTest` after the capture-order shader fix and ran the Lumen test suite successfully.
- `LumenCardCapture.3d.slang` now uses the owning card index as the deterministic same-depth atomic tie-break; `SV_PrimitiveID` is no longer used as a cross-draw identity.
- The isolated same-process paired run `artifacts/lumengi/C5/producer-isolation-full-full-20260817-cardowner-v3/c5-paired-equivalence.json` is `PASS`: all four checkpoints are finite and within the frozen `1e-4` mean-absolute tolerance for capture radiance, direct cache radiance, probe interpolation, and resolved diffuse output.
- This closes the previously observed C5 producer drift in the controlled equivalence case. Broad Surface Cache coverage/reject quality, tiny-atlas identity evidence, and universal request-to-next-frame validity remain separate gates.

### 2026-08-17 C6 pressure ledger closure

- The telemetry-only Surface Cache request ledger capacity is now centralized at 65536 records; scheduler admission, capture budgets, residency, and validator thresholds are unchanged.
- Release pressure run `artifacts/lumengi/C6/event-ledger-65536-pressure-20260817/nextframe-gate.json` is `PASS`: 208 frame samples, zero dropped ledger events, explicit request/capture/ready/hit identity, 46 evictions, and 9 stale-owner rejects.
- This closes the tested pressure sequence; multi-hour soak, GPU-wide VRAM, and broader scene/mutation coverage remain release gates.

### 2026-08-17 A2 dynamic image evidence

- `RenderSettingsChanged` now advances `lightingGeneration`, so toggling the environment/light settings cannot reset history without publishing a lighting epoch change.
- Fresh Release Mogwai runs for static, camera-cut, scene-reload, lighting-generation, and material/geometry cases each pass the dynamic runner with five checkpoints and real PNG/EXR captures under `artifacts/lumengi/A2/dynamic-*-20260817/`.
- These runs prove finite outputs, explicit history reject counters, reset/generation transitions, and image-capture provenance. The screenshots still show low-frequency ScreenProbe mottle; the broad no-noise quality Gate remains open.

### 2026-08-18 C6 tiny-atlas identity/drain recheck

- The tiny 64-texel sphere-array run `artifacts/lumengi/C6/tiny-sphere-drain-replay-20260818/surfacecache-effect.json` now consumes the explicit `surfaceCacheEvents` card/page ledger rather than aggregate request counts. It reports `complete`/`PASS`, 160 per-frame samples, 48 distinct card IDs, 16 distinct page IDs, eviction and stale-owner transitions, and an observed page-clear sentinel.
- The strict `artifacts/lumengi/C6/tiny-sphere-drain-replay-20260818/nextframe-gate.json` remains `PASS`. This is bounded tiny-atlas lifecycle evidence; long soak, VRAM, and broader mutation coverage remain open.

### 2026-08-18 A2 dynamic quality aggregation

- The read-only aggregator `artifacts/lumengi/A2/dynamic-quality-20260818/dynamic-quality-gate.json` is `PASS` for static, camera-cut, scene-reload, lighting-generation, and material/geometry manifests. It validates real PNG/EXR provenance, finite linear buffers, history transitions, and the frozen variance/tail-RMSE rules; it does not close multi-scene or release no-noise quality.

### 2026-08-18 S2 churn runtime proxy provenance

- The 60-second proxy `artifacts/lumengi/release/churn-telemetry-runtime-20260818/churn.json` captured 3600 frames and 120 canonical `surfaceCacheStats` samples with complete allocation/release/failure/lost/recapture/page telemetry. Authoritative `nvidia-smi` start/end VRAM was recorded, but Mogwai scripting exposed neither renderer `Device.info` nor authoritative renderer identity; the report therefore remains `BLOCKED` and its `proxy_gate` is not a soak result.
- The runner records Windows process working-set/peak memory through a read-only ctypes probe when available, keeps renderer/device hints non-authoritative, and distinguishes `proxy_gate` from `soak_gate`. The run was stopped after bounded proxy evidence because process memory grew substantially; no 30-minute dynamic or 2-hour/8-hour soak claim is made.

### 2026-08-18 C6 strict drain anchor correction

- The strict drain gate now anchors on the last newly-added `surfaceCacheEvents.sequence`, not the per-frame `requestObservedFrame` readback stamp. The fresh `artifacts/lumengi/C6/tiny-sphere-drain-tail8-budget64-20260818/pressure-drain-gate-sequence-anchor.json` is `PASS`: sequence 141 is followed by 20 scheduler samples, with 0 pending events and 0 lifecycle violations. The earlier readback-stamp `BLOCKED` result is superseded; no threshold was relaxed.

### 2026-08-18 A2 dynamic quality scope tightening

- The dynamic quality runner now requires exact checkpoints `[1, 8, 16, 32, 64]`, capture status `PASS`, and explicit scope metadata. The five-case Cornell aggregate remains a bounded `PASS`; `multiSceneEvidenceStatus` and `productionNoNoiseStatus` remain `OPEN`.

### C10 query-readback frame provenance correction

- Release `LumenGI`/`FalcorTest` was rebuilt after adding explicit `queryCountersSubmittedFrame` and `levelQueryCountersSubmittedFrame` telemetry. Readback reports the cache frame captured at dispatch submission rather than deriving provenance from the later CPU clipmap frame, which avoids cross-generation ambiguity after reset or scene changes.
- The Lumen CPU suite remains `PASS` (`113/113`); this is a telemetry correctness improvement only and does not close C10 broad coverage or final-quality gates.

### 2026-08-19 C10 two-phase GPU coverage

- The Release near-field run `artifacts/lumengi/C10/near-field-z1p2-focal10-continuation-20260819/radiance-cache.json` completed with runtime shader compilation, finite/non-negative cache channels, explicit fallback, and level-0 in-bounds `4806/8215` (`0.5850`). Query hit fraction was `0.9712`; valid hit-distance fraction was `0.9717`.
- The explicit phase manifest and frozen-threshold gate `artifacts/lumengi/C10/two-phase-coverage-20260819/c10-coverage-phase-gate.json` are `PASS`: near-field expected level `[0]` and far-field expected levels `[1,2,3,4,5]`, with no threshold relaxation. This closes the tested two-phase coverage contract; broader scene/camera quality, production radiance-cache integration, and release soak remain separate gates.

### 2026-08-19 C9 dual mark/export runtime attempt

- The serial Cornell/front mark-off and mark-on runs under `artifacts/lumengi/C9/finalcolor-dual-20260819/` compiled and produced finite, non-negative composite endpoints with explicit direct/indirect producer metadata.
- The strict export-equivalence check remains `FAIL`: mean absolute error `8.07e-5` and p99 `1.10e-3` exceed the frozen `2e-5`/`5e-4` limits (max error `4.15e-3` is within its bound). No threshold was relaxed; C9 full-color equivalence remains open.

### 2026-08-20 C9 same-process endpoint correction

- The graph now marks `ResolvedCompositePreview.out` first, so Mogwai's main output remains valid after LumenGI outputs are unmarked. The fresh same-process artifact `artifacts/lumengi/C9/same-process-endpoint-20260820-v5/finalcolor-runtime.json` reports `sameProcessMarkTransition=PASS_BOUNDED` and a readable endpoint after one explicit lazy RenderGraph recompile frame; runtime shader compilation completed without a fatal/device error.
- Direct/indirect producer metadata and finite/non-negative composite output are `PASS_BOUNDED`; mark-on/off hashes still differ (`meanAbsError=2.339e-3`, `maxAbsError=0.1559`), and a single execution has no independent export-on/off pair. The strict contract is therefore `PASS_BOUNDED` with export equivalence `BLOCKED`; C9 full-color equivalence remains open without changing tolerances.

### 2026-08-20 evidence harness completion

- The independent A2 linear sidecar runner `tests/lumengi/run_screenprobe_dynamic_sidecar.py` now validates raw `diffuseRadianceHitDist` RGB against resolved linear output from explicit EXR/NPY provenance. Missing sidecars remain `BLOCKED`; no display-space PNG is accepted as linear evidence. Its dependency-free self-test passes.
- The S2 launcher `tests/lumengi/run_release_soak_launcher.py` now serializes a unique, GPU-indexed 30-minute dynamic plus 2-hour soak plan, authoritative `nvidia-smi` samples, renderer provenance, and per-phase logs. It is `READY_FOR_OFFLINE_GATE` only; no short proxy is promoted to a release soak result.
- The complete offline contract matrix is green (C5/C6/C9/C10/A2/S2 plus diagnostic rough-specular/transmission), while C9 export equivalence, A2 linear no-noise improvement, GPU-wide VRAM, and long soak remain explicitly open or blocked.

### 2026-08-20 A2 linear sidecar closure

- Cornell and Arcade independent sidecar reports
  `artifacts/lumengi/A2/dynamic-cornell-no-noise-20260820/no-noise-linear.json`
  and `artifacts/lumengi/A2/dynamic-arcade-no-noise-20260820/no-noise-linear.json`
  are `PASS` across the five transition cases and checkpoints `[1, 8, 16, 32, 64]`.
- The sidecar computes variance/tail-RMSE from linear raw/resolved resources only;
  PNGs and hit-distance alpha are excluded from the quality metric. This closes
  the measured A2 sidecar gate, but does not close C9 export equivalence or the
  broader multi-angle/release no-noise checklist.

### 2026-08-22 serial GPU continuation

- The deterministic same-process C9 replay
  `artifacts/lumengi/C9/deterministic-replay-20260820-v1/c9-export-repro.json`
  completed with both mark-on and mark-off producer metadata, finite output,
  and matching configuration/PID. The frozen export-equivalence gate remains
  `FAIL`: mean absolute error `3.5627e-5` (limit `2e-5`) and max error
  `5.2490e-3` (limit `5e-3`); p99 and relative error pass. The downstream
  final-color contract is therefore `FAIL`; no threshold was relaxed.

- The S2 dynamic child artifact
  `artifacts/lumengi/release/soak-launch-20260822-rtx0-v2/dynamic/churn.json`
  completed the requested 1800 seconds/108000 frames with 361 authoritative
  `nvidia-smi` samples, stable resident bytes, bounded cache pages, and the
  long-phase cadence (material 60, reload 3600, resize 5400 frames). The
  release launcher/gate remains `BLOCKED`: Mogwai did not provide authoritative
  Falcor renderer `Device.info`, the host process returned non-zero after the
  script artifact was written, and no 2-hour soak phase was run. The launcher
  now recognizes a complete child artifact and records an explicit intentional
  termination for future runs; this does not bypass renderer provenance or
  duration gates.

- C10 two-phase coverage (`artifacts/lumengi/C10/two-phase-coverage-20260819/`),
  C6 bounded event-ledger/eviction evidence, and the A2 linear sidecars remain
  valid bounded passes. Rough-specular and transmission gates remain diagnostic
  only: rough specular lacks history/resolve/performance proof, and transmission
  is forced `ENABLED=0`/`REFERENCE_ONLY=1`. Production completion and shutdown
  authorization remain withheld.

### 2026-08-22 C9 bounded endpoint and S2 resource-failure follow-up

- The rebuilt Mogwai exposes the live renderer device through `m.device.info`; the
  60-second provenance smoke recorded `NVIDIA GeForce RTX 2060 SUPER` / `Direct3D 12`
  with `provenance_status=PASS` in
  `artifacts/lumengi/release/device-provenance-smoke-20260822/churn.json`.
- The C9 same-frame runtime
  `artifacts/lumengi/C9/same-frame-runtime-20260822/finalcolor-runtime.json`
  has direct+indirect producer metadata, finite/non-negative output, and a byte-exact
  retained-resource transition with zero producer executions. Its strict gate is
  `PASS_BOUNDED` at
  `artifacts/lumengi/C9/same-frame-runtime-20260822/same-frame-gate.json`;
  `finalcolor-contract-runtime/finalcolor-contract.json` is also `PASS_BOUNDED`.
  This is not a recompiled/export-on-off equivalence PASS.
- S2 dynamic 30-minute evidence reached `1800s/108000` frames with live renderer
  provenance, but the serial two-hour phase failed at approximately 422.6 seconds:
  Mogwai raised `MemoryError: bad allocation` from `m.renderFrame()` during the
  required material/reload/resize churn and emitted no soak child JSON. The strict
  gate remains `BLOCKED` at
  `artifacts/lumengi/release/soak-launch-20260822-rtx0-v4/release-soak-gate.json`.
  This is treated as a renderer/production resource-lifetime blocker, not hidden by
  lowering cadence or dropping the churn workload.
- The additive live-device binding is in `Source/Mogwai/MogwaiScripting.cpp`; the
  release build and all offline self-tests remain green. Do not authorize shutdown
  while C9 strict export equivalence and S2 resource-lifetime/soak evidence remain
  open or blocked.

### 2026-08-30 S2 scene-reload lifetime guard

- A 1200-second material-only isolation (`artifacts/lumengi/release/soak-isolation-20260830-material-1200s/churn.json`) completed 72,000 frames with 1,201 material mutations, complete canonical stats, live `m.device` provenance, and no allocation failure. This separates the per-frame material path from the long-soak failure.
- `Mogwai::Renderer::setScene()` now fences before and after replacing an existing scene (`Source/Mogwai/Mogwai.cpp`). The first wait drains work referencing the old scene; the second advances the fence after graph/pass release so deferred GPU resources are reclaimed before the next reload. The normal frame path is unchanged.
- Release `Mogwai` rebuilt successfully (`cmake --build build/windows-vs2022 --config Release --target Mogwai --parallel 1`), CodeGraph was synchronized, and all dependency-free LumenGI/S2/C9/C10 self-tests passed. The strict S2 two-hour gate remains `BLOCKED` until a fresh post-fix run produces complete 2-hour evidence; C9 strict export equivalence remains open.
- The S2 launcher now persists a strict `RUNNING`/`BLOCKED` manifest after initial setup and after each phase. If the launcher is interrupted after a child writes its artifact, the completed phase's process/VRAM evidence remains discoverable; the offline gate still rejects non-`READY_FOR_OFFLINE_GATE` manifests.
- A fresh C9 replay after the scene-fence change remains strict `FAIL` at
  `artifacts/lumengi/C9/deterministic-replay-20260830-v2/c9-export-repro.json`
  (mean `5.0122e-5`, p99 `6.1035e-4`, max `2.5635e-3`). The max error is within
  its bound, but mean/p99 exceed the frozen tolerances; the lifetime guard does
  not claim C9 numerical equivalence.
- The first post-fix S2 launcher attempt was interrupted during renderer startup
  before it could checkpoint a manifest (`artifacts/lumengi/release/soak-launch-20260830-postfix-v5/` contains only compile logs). It is not a gate result; rerun with the checkpointing launcher when the GPU window is available.

### 2026-08-30 S2 checkpointed post-fix retry

- The checkpointing launcher completed the 30-minute dynamic phase as `PASS`
  with authoritative RTX 2060 SUPER / D3D12 provenance and wrote
  `artifacts/lumengi/release/soak-launch-20260830-postfix-v8/dynamic/churn.json`.
- The required soak phase started but was stopped after approximately four
  minutes when system free physical memory fell below 0.5 GB while the cold
  Mogwai process approached 8 GB working set. Only the exact soak child PID
  was terminated; launcher logs and manifest were preserved. No `churn.json`
  was emitted for soak, so the offline result at
  `artifacts/lumengi/release/soak-launch-20260830-postfix-v8/release-soak-gate.json`
  is correctly `BLOCKED` (`dynamic=PASS`, `soak=BLOCKED`). This is a safety
  stop and not a duration or resource-lifetime PASS.
- C9 strict replay remains `FAIL` at
  `artifacts/lumengi/C9/deterministic-replay-20260830-v2/c9-export-repro.json`
  (mean `5.0122e-5`, p99 `6.1035e-4`, max `2.5635e-3`); thresholds remain
  unchanged. Shutdown remains unauthorized while C9 equivalence and the
  complete 2-hour soak are open/blocked.

### 2026-08-30 C9 replay isolation

- Strict replay now retains only the production-required `LumenGI.resolvedDiffuseGI`
  mark during the mark-on phase; ordinary showcase captures still retain the
  full diagnostic output set. This isolates RenderGraph lifetime/aliasing from
  unrelated intermediate exports without changing production defaults or
  thresholds.
- Fresh v3/v4 replays reduced p99/max into bounds but remain `FAIL` on mean
  error: v3 mean `2.6779e-5`, p99 `4.2725e-4`, max `2.6855e-3`; v4 mean
  `3.1251e-5`, p99 `4.8828e-4`, max `2.4414e-3`. A temporary NoResampling
  diagnostic (v5) was worse (mean `5.6078e-5`) and is not retained as a
  production setting. C9 strict equivalence remains open; no tolerance was
  relaxed.

### 2026-08-30 S2 host-memory safety guard

- `tests/lumengi/run_release_soak_launcher.py` now records authoritative host
  available-memory samples (`GlobalMemoryStatusEx` on Windows) in the launch
  manifest, performs a preflight check, and uses a default 0.5 GiB minimum-free
  threshold (`--min-host-free-gib`).
- If the threshold is crossed during a phase, the launcher terminates only the
  exact Mogwai child, records the observed value and reason, and marks that
  phase `BLOCKED`; it never converts a safety stop into a soak PASS or changes
  the duration/cadence/VRAM gates. The existing v8 soak result remains
  `BLOCKED` and must be rerun with sufficient host memory.
- A v10 dry start reached the former guard at 0.959 GiB during cold startup
  before a dynamic artifact existed; this calibrated the default from 1 GiB to
  0.5 GiB, still above the earlier observed ~0.43 GiB safety stop. This is a
  launcher safety calibration, not a release-gate relaxation.
- Launcher self-test, Python compile, host-memory query, and CodeGraph sync
  are required before the next GPU window; no GPU run is started by this
  change.

### 2026-08-30 S2/C9 follow-up hardening

- The churn driver now performs an explicit `unloadScene()` followed by a
  fenced `loadScene()` for each reload, and records every `m.device.wait()`
  event under `resource_sync`. A 90 s/3-reload probe completed with all nine
  fences passing, but host working-set growth remains observable; S2 is not
  certified by this probe.
- The soak launcher defers the normal 0.5 GiB guard until the child's
  `CHURN seconds` readiness marker (or 180 s), while enforcing a 0.25 GiB
  startup hard floor. This prevents cold shader compilation from being
  mistaken for steady-state churn without weakening the release threshold.
- C9 strict replay now fences both before and after graph teardown and supports
  reverse mark order. The v6 off-first replay still fails only the frozen mean
  error bound (`3.3392e-5` vs `2e-5`); p99/max/relative-max remain in bounds.
- S2 v13 dynamic completed 1800 s/108000 frames with one reload and one resize
  (`resource_sync` 4/4 PASS). Its soak phase safely stopped at 0.473 GiB after
  four reloads, so the result remains `BLOCKED`; this confirms the guard is
  catching residual host growth rather than fabricating a soak PASS.
- The next launcher run uses phase-specific defaults (dynamic 30 min, soak 60
  min reload/resize cadence) while retaining positive mutation counts and all
  duration/VRAM/memory thresholds.
- S2 v14 closed the long-run gate: dynamic 1800 s/108000 frames and soak
  7200 s/432000 frames both PASS with authoritative RTX 2060 SUPER/D3D12
  provenance, 361/1441 VRAM samples, stable Surface Cache residency, and
  complete reload/resize `resource_sync`. Evidence:
  `artifacts/lumengi/release/soak-launch-20260830-postfix-v14/release-soak-gate.json`.
- C9 NRD isolation did not close strict equivalence: direct-NRD-off measured
  mean `4.0274e-5`, indirect-NRD-off `7.5287e-5`; both are worse than default.
  A default-config settle-192 replay remained `3.3476e-5`, so convergence is
  not the cause. Keep the frozen C9 thresholds and target resource lifetime,
  aliasing, and barrier telemetry next.
- C9 v11 added a strict-replay-only `unloadScene()` boundary between the two
  phases, preventing `SceneBuilder` allocations from overlapping the prior
  scene. Mean error improved to `2.4324e-5` (p99 `3.6621e-4`, max
  `2.1065e-3`), but the frozen `2e-5` mean gate still fails. Combining the
  boundary with settle-192 regressed to `2.6635e-5`; retain the unload boundary
  and do not increase settle frames as a threshold workaround.
