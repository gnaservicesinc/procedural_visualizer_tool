# Performance and correctness audit

Updated: 2026-09-09. Iteration 6, based on commit
`9ff5d1601fba45f65c7d62ae88267e77f34b4663` (iteration 5). Iteration 4 was
committed in 17.10.0, iteration 3 in 17.9.0, iteration 2 as `4eb1dd5`, and
iteration 1 is also in the baseline.

This is the continuation record for the requested performance, memory,
configuration, and latent-defect audit. Iteration 6 checks invariant project
canvas, clock/music, path and export data once per synchronous validation call,
while preserving per-layer checks and direct-edit visibility. The working tree
was clean at the start. This iteration is a local patch with no new version,
commit, tag, or remote CI. Linux and Windows qualification belongs on GitHub;
prior platform evidence below describes its recorded commits only.

## Scope and fidelity contract

Reviewed the CPU frame pipeline, layer composition/admission, LFO
materialization, OBJ projection/rasterization, decoded-source and mesh caches,
Metal/OpenGL surface shading, OpenGL resource ownership, and editor preview
lifecycle. Existing tests additionally exercise persistence, codecs, audio,
Live behavior, localization, and export. This is a broad first pass, not a claim
that every path in those subsystems is exhaustively audited.

Optimizations retain authored configuration, effect order, straight RGB under
transparency, HDR values, animation timing, and two-sided surface behavior.
Defect fixes deliberately restore previously missing coverage. No public
configuration structs, persistence formats, precision, sampling resolution,
or backend comparison tolerances were changed.

## Implemented

| ID | Finding and change | Evidence |
| --- | --- | --- |
| A01 | CPU OBJ and OpenGL mesh coverage used a byte per Boolean. `PackedMask` uses one bit per pixel in 64-bit words. Each mask has one rendering-thread owner. | Empty masks, word boundaries, odd dimensions, layered/opaque meshes, backend parity. |
| A02 | Analytic Sphere/Cylinder/Cube shaders sampled and lit rear intersections even behind a fully opaque front. Skip rear shading/composition only when sampled front alpha is exactly one, on CPU, Metal, and OpenGL. | All 12 animated CPU/Metal benchmark hashes match the baseline; existing backend parity tests pass. |
| A03 | CPU and OpenGL mesh paths classified nearly opaque input as opaque, discarding representable rear coverage and potentially visible HDR color. Use exact opacity for nearest-only admission and depth-peeling termination. | A closed shell textured at `nextafter(1.0F, 0.0F)` now accumulates its rear coverage; translucent and opaque fixtures pass. |
| A04 | Raster bounds converted potentially enormous finite projected coordinates to `int` before clipping, invoking undefined behavior. Reject offscreen bounds and clip in floating point before conversion. | Huge covering/offscreen geometry, plus UndefinedBehaviorSanitizer and float-cast-overflow instrumentation. |
| A05 | Huge adjacent triangles could round a shared edge outward on both sides, leaving cracks. For edges with extreme endpoints, evaluate a canonical endpoint order and negate for the opposite face. Ordinary-coordinate arithmetic is unchanged. | A quad scaled by `1e12` covers the frame exactly and produces identical pixels with reversed winding. |
| A06 | Disabled/zero-opacity layers and disabled groups left decoded images, height fields, OBJ meshes, and OpenGL mesh uploads resident. Prune cache ownership from contributing-layer references on project render and editor scheduling/completion. Pending loads marked for eviction cannot republish themselves. | Weak-pointer lifetime tests, shared references, disable/re-enable equality, and a deterministic OBJ publication/eviction race. |
| A07 | Validating disabled mesh-construction layers could load their OBJ assets solely for a discarded memory estimate. Retain structural validation but omit that asset inspection for noncontributing layers. | Project validation and rendering suites; enabled-layer validation retains its existing workload checks. |
| A08 | CPU render/backend preparation copied the entire configuration even without active LFOs or path bindings. All-disabled LFO lists also triggered materialization and its memory allowance. Only make the render-time copy when needed. | Disabled LFO output equals the unmodified configuration through both CPU entry points; authored LFO definitions remain intact; existing active/nested LFO tests pass. |
| A09 | PNG color decoding could leak its file/libpng state if vector allocation or error-string construction threw. Add allocation-free scope cleanup. | Existing valid, malformed, cancellation, and sanitizer source-image tests pass; forced allocation failure remains a qualification item below. |
| A10 | Three OpenGL passes relied on explicit cleanup calls that exceptions could bypass. Add idempotent scope cleanup for mesh, coordinate-effect, and generated-source GPU resources. | Real OpenGL suite passes; cleanup also runs during stack unwinding. Forced allocation failure remains a qualification item below. |
| A11 | The mesh working-memory estimate omitted coverage storage. Account for the packed allocation, including its final partial word. | Core/admission tests and overflow-safe size computation. |
| A12 | Cached displacement mesh variants held strong references to decoded height maps already evicted from the source cache. Geometry already contains the sampled heights; retain a weak identity reference instead. | The decoded height field expires while its generated mesh remains cached; source replacement and re-enable rendering tests pass. |
| A13 | CPU mesh and OpenGL displacement triangles crossing the near plane were discarded wholesale. Clip them into at most two triangles, retaining UVs, normals, world coordinates, winding, and depth peeling. Intersections use a canonical endpoint order and stack storage. | Independent CPU ray/triangle oracle for one/two outside vertices, both projections and windings, opaque/translucent textures; camera-inside shell, exact near-plane and entirely behind tests; real OpenGL crossing tests. |
| A14 | Remove unused object coordinates from every projected vertex (88 to 64 bytes on this host). Add checked projection, transformed-normal, retained OBJ geometry, and cold OpenGL staging/buffer allowances to admission. Include OpenGL readback and texture storage. | Overflow and transactional-result tests; ordinary OBJ geometry affects validation without construction enabled; four layers sharing a 20,003-vertex OBJ produce identical pixels with a one-byte worker budget. |
| A15 | Replace the single-entry OBJ cache with an LRU bounded by 16 entries and 512 MiB of retained mesh allocations. Keep single-flight loading, generation fences, file/limit replacement, and pruning. Publication fences are per asset so unrelated concurrent loads can both remain cached. Oversized assets remain renderable without cache retention. | Alternating reuse, entry/byte eviction, oversized no-retention, active-reader lifetime, all-entry pruning, same-path replacement and out-of-order publication tests; 60 alternating requests now parse only three meshes. |
| A16 | Reject entirely behind-camera triangles and triangles wholly outside one viewport edge before face-normal and UV preparation. This view rejection applies to arbitrary OBJ geometry without a winding assumption. | Existing extreme-coordinate tests, reversed/inside-camera coverage tests, and nine matching animated CPU mesh hashes; offscreen-heavy probe below. |
| A17 | Thin displaced triangles exposed an existing OpenGL attribute-interpolation mismatch, including geometry entirely in front of the camera. Use homogeneous determinants at the actual pixel center to interpolate UVs, depth, world positions, and normals without near-zero perspective division. Remove unused smooth varyings. | Before correction, maximum differences reached 0.01327 in orthographic and 0.00583 in perspective fixtures. Eight crossing/noncrossing, opaque/translucent cases now pass the unchanged 0.0035 CPU/GPU tolerance. |
| A18 | Separate immutable OBJ/displacement/image allocations from per-worker projection, frame, and composite storage. Deduplicate by allocation identity, include cached variants, and reserve the shared/composite pool once before admitting layer workers. Standalone validation includes decoded image storage. | One versus four references to the same 20,003-vertex OBJ have equal shared and worker categories; categories sum to the public estimate, budget subtraction and overflow are checked, and constrained/parallel renders match exactly. Public `ValidationResult` layout is unchanged. |
| A19 | Normalize clipped triangle corner order before generating its fan, and recompute the area after orientation normalization rather than negating an evaluation from a different order. These eliminate two sources of winding-dependent floating-point arithmetic relevant to the reported Linux ARM64 camera-inside failure. | Camera-inside shell and all six corner permutations of a textured clipped triangle pass locally, including an explicit contracted-arithmetic build. The prior ARM failure was not reproduced locally; iteration 4 subsequently verified both surface suites passing on main GitHub Linux ARM64 at `d3b2ab2`. Failure output includes the first differing pixel/channel/value and center alpha. |
| A20 | Add conservative depth occlusion for arbitrary CPU meshes rendered with nearest-hit semantics. A triangle is rejected only if complete depth coverage over its pixel bounds is nearer than an outward-rounded lower bound on its rasterized depth. Both windings remain eligible to draw; transparency that requires depth peeling bypasses this optimization. | Exact comparisons with culling disabled cover 48 combinations of projection, winding, clipping/coverage, alpha and compositing policy, plus mixed visible/hidden random geometry and mirrored transforms. Nine animated baseline hashes still match. Hidden-face benchmark below. |
| A21 | Validation now leases immutable assets to its render invocation; worker loaders reuse those exact handles after LRU eviction or oversized-cache bypass. Lease keys include file version, decode intent, OBJ limits, and displacement parameters. This prevents counting one retained copy while rendering reloads another. | Four concurrent workers reuse 17 uncached OBJ assets with no reparses or duplicate handles. Changed files and limits bypass old leases. Color/height/geometry are separate leases; uncached assets expire after the last invocation owner. ThreadSanitizer passes. |
| A22 | Add `pvt_obj_surface_contracted` to normal CTest for GNU/Clang builds (`-O3 -ffp-contract=fast -fno-math-errno`), retaining the existing platform-default surface test. | Both surface suites pass locally and in the subsequently verified main Linux ARM64 run at `d3b2ab2`; all five main/tagged platform jobs pass. MSVC retains its native surface suite. |
| A23 | Replace saved-layer-clock validation's full `RenderConfig` copy and recursive renderer validation with a clock-only check by const reference. Render data, LFO graphs, Live settings, assets, and memory estimates are no longer visited twice by that recursion. Unrelated invalid render data is no longer mislabeled as a saved-clock error. | Twenty-six clock corruptions are rejected for both enabled and disabled saved clocks through public validation, structural validation, and explicit-phase backend preparation. Direct repairs, output-FPS limits, disabled project layers, and transactional render failure pass. |
| A24 | LFO destination discovery copied all `RenderData`, including saved music, for every destination. Probe only the addressed wave, swing, effect, or post effect using the actual destination writer; global targets retain the authored post-effect policy. LFO-to-LFO targets retain their existing stable-ID check. | Reordering/removal, legacy versus stacked post effects, custom paths, malformed IDs, direct edits, nested LFOs, and authored-state tests pass. Static-build CTest now bounds validation allocations below one music feature table for both enabled and disabled LFOs. Animated baseline pixels match exactly. |
| A25 | Validate project canvas/output and layer data through a synchronous const borrowing view, eliminating `apply_global_config` copies for the global probe and every validated layer. Standalone validation uses the same implementation. Live remains checked once per project; per-layer particle, asset, and music-copy accounting accept the borrowed inputs. | Project allocation requests fall from 86,801,520 to 23,744 bytes in the large-analysis probe. Thirty-six invalid-edit cases preserve standalone diagnostic equivalence and transactional failure, including disabled layers/clocks, named streams, Live, paths and nested LFOs; repairs pass. Worker music allowances remain unchanged, and ten animated frame hashes match exactly. |
| A26 | Sequence shutdown stored its atomic stop flag outside the condition-variable mutex, allowing notification between a worker's false predicate check and wait. Publish stop under that mutex before notifying, both in the join guard and worker-scheduler failure path. | The sanitizer core suite hung in callback cancellation; a process sample showed the main thread in `SequenceWorkerJoiner` joining a worker asleep at the ready-slot wait. The corrected suite passes, including 48 repeated callback-cancel, callback-exception, and atomic-cancel cases with one/four workers, ordered output and staging cleanup. |
| A27 | Scope successful canvas validation to one synchronous project invocation, tied to the exact borrowed canvas/output inputs. Layers reuse invariant dimension/FPS, Live, global clock/music, audio-default, path-definition and export checks; saved clocks, LFO graphs, path bindings, alpha policy, assets and workload/admission checks remain per layer. | 132 direct-edit cases preserve standalone diagnostics, disabled-state validation, estimates and transactional failure. Two sequential pairs reduce music-heavy project validation time about 44% and animated CPU frame time about 21%, with the same ten-frame float hash. A path-allocation scaling guard passes after the change and fails against the iteration 5 baseline. |

Pruning preserves paths, attached source files, saved disabled settings, music
analysis, and undo/history. In-flight readers retain immutable shared handles
until they finish. OpenGL pruning uses a nonblocking mutex attempt so an editor
event cannot deadlock a driver that requires rendering on the GUI thread;
preview completion or the next project frame retries when necessary.

## Measurements

Probe: `tests/performance_audit_probe.cpp`, optional CMake target
`pvt_performance_audit_probe`. Release build, local Apple Silicon host, 512x512,
12 distinct animated frames per case after warm-up, median frame time.
Hashing is outside the timed region and includes every float bit in all 12
frames. These are workload-specific observations, not a universal speedup.

| CPU case | Before, ms | After, ms | Time reduction | Matching hash |
| --- | ---: | ---: | ---: | --- |
| Sphere, opaque | 37.71 | 28.49 | 24.4% | `1ca25ff9d37767bf` |
| Cylinder, opaque | 42.69 | 31.63 | 25.9% | `54e64ad9e0684129` |
| Cube, opaque | 36.80 | 22.14 | 39.8% | `9ef621f0baa00654` |
| Sphere, translucent | 39.67 | 39.93 | -0.7% | `26ccbe780b5a6d9e` |
| Cylinder, translucent | 44.27 | 43.86 | 0.9% | `2535c3ac98967722` |
| Cube, translucent | 38.42 | 34.01 | 11.5% | `9f7eb8e8e1100600` |

| Metal case | Before, ms | After, ms | Matching hash |
| --- | ---: | ---: | --- |
| Sphere, opaque | 1.71 | 1.52 | `8afb83c1bf66bf78` |
| Cylinder, opaque | 1.51 | 1.20 | `11a7d2b113bd96da` |
| Cube, opaque | 1.46 | 1.23 | `009ca5428d4efc9c` |
| Sphere, translucent | 1.49 | 1.52 | `5fad9e9a0336512f` |
| Cylinder, translucent | 1.52 | 1.44 | `fc718f41f250d1b0` |
| Cube, translucent | 1.52 | 1.48 | `313e7c1884bb4443` |

The small translucent timing changes should not be treated as established
improvements or regressions without repeated controlled measurements.

Coverage memory falls by 87.5% at dimensions divisible by 64 pixels:

| Canvas | Byte mask | Packed mask | Saved per live mask |
| --- | ---: | ---: | ---: |
| 3840x2160 | 7.91 MiB | 0.99 MiB | 6.92 MiB |
| 7680x4320 | 31.64 MiB | 3.96 MiB | 27.69 MiB |

These figures describe coverage only, not total renderer memory. Float color,
depth, geometry, authored state, and driver allocations remain separate.

### Iteration 2 measurements

Probe: `tests/obj_performance_audit_probe.cpp`, optional target
`pvt_obj_performance_audit_probe`; pass the repository root as its argument.
The baseline probe was compiled from the committed `obj_surface.cpp` and
`obj_mesh.cpp`; the same probe and other dependencies were used for both.
These local Release observations were repeated sequentially after the first
run. Timing is not a pass/fail gate.

| Workload | Baseline | Iteration 2 | Evidence |
| --- | ---: | ---: | --- |
| Three 10,082-triangle OBJ files, 20 alternating rounds | 370.13 ms, 60 parses | 19.09 ms, 3 parses | 94.8% less elapsed time; immutable reuse rather than reparsing. |
| 100,000 offscreen triangles, median of 12 animated frames | 3.98 ms | 1.44 ms | 63.8% less elapsed time; exact float hash `eeb68bbff0e87f45`. |
| Projected storage for 1,000,000 positions | 83.92 MiB | 61.04 MiB | 22.89 MiB saved; 27.3% reduction in this allocation. |

All nine CPU probe hashes match the committed baseline. The other eight cases
cover both projections, opaque/translucent closed shells, partial curvature,
and mirrored geometry over 12 animated frames each. Their hashes in case order
are `3ffd8bdbaca8d339`, `2dd303b7c62df526`, `14aaef1e834fdb91`,
`499b79461999a543`, `2f903b1caaa32448`, `6a2a723881e8918d`,
`67f31582072d3a3c`, and `c7622f68ecad7c4e`.

Clipping deliberately restores missing coverage; A17 deliberately corrects
GPU interpolation. Those defect cases are validated against independent
geometry/CPU results, not hashes of the defective output. No OpenGL throughput
improvement is claimed from A17; its per-fragment interpolation cost needs
qualification on other drivers alongside correctness.

### Iteration 3 measurements and accounting

The same OBJ probe now also compares culling enabled and disabled in one binary,
checking complete float output outside the timed region. At 129x127, for a visible
front triangle and 10,000 hidden rear triangles, seven Release frames per mode gave
medians of **15.37 ms without culling and 4.16 ms with culling**, a 72.9% reduction.
An earlier run gave 13.98/3.91 ms. These are local workload observations, not a
universal speedup or a timing test gate. All nine animated OBJ hashes above
remain unchanged in this iteration.

Depth tiles use eight bytes per 8x8 block on the tested host (about 0.99 MiB at
3840x2160), in addition to the packed coverage mask. They are allocated only
for nearest-hit meshes with at least 64 triangles; small triangles avoid the
bound computation. Partial boundary tiles require coverage of every actual
pixel. Conservative stale tile maxima can miss opportunities but cannot remove
visible samples. No winding-based closed-shell assumption or pixel epsilon is
used, and layered transparency retains the original path.

`src/render_memory.h` separates shared immutable allocations, the worst layer
worker, and the two composite frame buffers. Allocation identities deduplicate
shared assets; per-invocation leases preserve the counted handles across worker
execution, including eviction from process caches. Leases are written only
while validating, then read without mutation by workers. Replaced files and
new modulation-dependent displacement keys retain the existing loader behavior.
There is no public API-layout change. End-of-frame destruction and normal
pruning release the owners, without deleting authored assets or disabled state.

OpenGL's previous allowance remains 76 bytes/pixel plus packed coverage,
excluding caller source/destination: 36 bytes for CPU mapped/layer/depth arrays
and 40 for two RGBA32F and two depth32F textures. Validation can conservatively
include CPU and GPU alternatives together before backend selection. A source
cache snapshot also includes still-retained variants. Parser/build/decode
scratch, lease/hash/control-block overhead, independent concurrent invocation
ledgers, GPU/context heaps, and new asset versions or LFO-generated geometry
after admission are not fully bounded. One oversized worker remains permitted.
The estimate is **not a hard process-memory cap**; F02/F05/F06 record the limits.

### Iteration 4 measurements and next design boundary

Probe: `tests/validation_performance_audit_probe.cpp`, target
`pvt_validation_audit_probe`. Release static build on the local Apple Silicon
host, 64x64, four layers, eight enabled LFOs per layer, project and saved layer
music clocks, and three named frequency streams per analysis. Each full-signal
or named stream contains 60,000 feature samples across ten minutes. Each timing
uses one warm-up and nine measured calls; the rendering calls advance the frame.
Baseline and current binaries ran sequentially twice. The second pair gave:

| Operation | Baseline median, ms | Iteration 4 median, ms | Baseline requested bytes | Iteration 4 requested bytes |
| --- | ---: | ---: | ---: | ---: |
| Single-config validation | 5.97 | 2.92 | 96,469,616 | 4,688 |
| Four-layer project validation | 23.28 | 15.25 | 482,305,320 | 86,801,520 |
| Animated four-layer CPU frame | 44.19 | 31.18 | Not measured across workers | Not measured across workers |

The first pair measured 5.12/3.04 ms, 22.94/15.00 ms, and 44.88/30.28 ms,
respectively. The observed animated improvement is about 29–33% for this
validation-heavy workload, not a universal rendering speedup. Both pairs give
the same ten-frame hash, `20153f5a9b7d0325`; hashing runs outside the timings.
Requested bytes are cumulative ordinary C++ allocations on the validating
thread, not simultaneously retained memory, RSS, or driver allocations.

Run timings with `build-audit/pvt_validation_audit_probe`. Static-build CTest
runs its `--check-allocations` mode without timings: validation must allocate
less than one 2,400,000-byte feature table, for both enabled and disabled LFOs.
Both cases currently request 4,688 bytes. Shared-library allocation interposition
is platform-dependent, so the allocation-only CTest is registered for static
builds; the shared build still exercises the semantic regressions.

At the end of iteration 4, F04 remained open beyond A23/A24.
`apply_global_config` still owned value copies of project and saved-layer
analysis for the global probe, each validated layer, and each rendered layer. LFO materialization also retains its necessary authored
versus evaluated separation. Full music tables and graph dependencies are still
checked anew on each API call; direct edits cannot safely reuse pointer- or
source-digest-based validity. Equal file identity does not prove equal edited
analysis, named ranges, clock policy, LFO targets, or Live state.

The planned next F04 step was an invocation-scoped const validation view over
the canvas/output and `RenderData`, avoiding project-validation adapters before
considering a persistent render plan. Iteration 5 implements that view below
and confines its lifetime to synchronous validation. Persistent sharing needs an owned immutable document snapshot and
explicit invalidation for direct API edits, project/Live revisions, named-stream
processing, paths, and LFO dependency order. No cross-call validity cache or
public by-value ABI change is introduced here. Retain the existing worker
music-copy allowance until those actual copies are removed and measured.

### Iteration 5 measurements and next design boundary

The same large-analysis probe was built at `9c058e9` and after A25. Native
Release static build, AppleClang/Qt 6.11.2 on local Apple Silicon; dimensions,
layer/LFO counts, analyses, frame sequence, and timing protocol are unchanged
from iteration 4. Two sequential baseline/current comparisons gave:

| Operation | 17.10.0 median, ms | Iteration 5 median, ms | 17.10.0 requested bytes | Iteration 5 requested bytes |
| --- | ---: | ---: | ---: | ---: |
| Four-layer project validation, first pair | 15.16 | 13.43 | 86,801,520 | 23,744 |
| Animated four-layer CPU frame, first pair | 30.36 | 29.20 | Not measured across workers | Not measured across workers |
| Four-layer project validation, second pair | 15.12 | 13.92 | 86,801,520 | 23,744 |
| Animated four-layer CPU frame, second pair | 30.84 | 28.60 | Not measured across workers | Not measured across workers |

Project validation requests **99.97% fewer allocation bytes**, with about
8–11% less validation time and 4–7% less animated frame time in these two local
pairs. These remain workload observations, not timing test gates or an RSS
claim. Standalone validation still requests 4,688 bytes; its timings fluctuate
around 3 ms with no intended performance change. All comparisons preserve the
ten-frame float hash `20153f5a9b7d0325`.

`pvt_validation_allocations` now also bounds an entire four-layer project below
one 2,400,000-byte feature table across 12 combinations of enabled/disabled
LFOs, saved clocks, and enabled/disabled/zero-opacity layer contributions.
Neither the global nor the per-layer validation adapter copies music, paths,
Live collections, or render data. The small structural export adapter remains
owned because validation enables its alpha flag temporarily.

The view lives only during synchronous validation. Asset leases still own
immutable decoded handles; no configuration reference is retained in those
leases or passed to workers. Render-time adapters and enabled-LFO materialization
still own their copies. Regression checks explicitly preserve both music-copy
allowances, including full-signal and named-stream samples.

The next phase identified after iteration 5 was to validate invariant project
canvas/clock/path data once per invocation, retaining per-layer saved-clock,
LFO graph, asset and workload validation. Iteration 6 implements that phase
below. This must remain invocation-local: direct API edits, named-stream
processing and Live changes must be observed on the next call. Later removal
of render-time analysis copies needs a const runtime context or an owned
immutable snapshot plus separately owned evaluated layer state; reduce worker
allowances only after the actual copies are removed and measured. A persistent
cross-call plan still requires explicit revision and dependency invalidation.

### Iteration 6 measurements and next design boundary

`tests/validation_performance_audit_probe.cpp` retains the music-heavy workload
and adds a separate shared-path workload: 64 paths with 64 nodes each, at 64x64,
with one or four layers. The baseline is iteration 5 at `9ff5d16`; both probes
use Release static builds with the same AppleClang, Qt 6.11.2 and Metal headers.
Two sequential baseline/current pairs after qualification completed gave:

| Operation | Iteration 5 median, ms | Iteration 6 median, ms | Iteration 5 requested bytes | Iteration 6 requested bytes |
| --- | ---: | ---: | ---: | ---: |
| Four-layer project validation, first pair | 13.48 | 7.44 | 23,744 | 22,512 |
| Animated four-layer CPU frame, first pair | 29.10 | 22.92 | Not measured across workers | Not measured across workers |
| Four-layer project validation, second pair | 13.42 | 7.52 | 23,744 | 22,512 |
| Animated four-layer CPU frame, second pair | 28.70 | 22.70 | Not measured across workers | Not measured across workers |
| Shared paths, one layer, first pair | 0.288 | 0.145 | 397,312 | 201,252 |
| Shared paths, four layers, first pair | 0.668 | 0.139 | 987,272 | 203,032 |
| Shared paths, one layer, second pair | 0.278 | 0.143 | 397,312 | 201,252 |
| Shared paths, four layers, second pair | 0.709 | 0.140 | 987,272 | 203,032 |

Music-heavy project validation takes about **44% less time**, and its animated
CPU frame takes about **21% less time** in these pairs. Path-heavy four-layer
validation requests **79.4% fewer bytes** and takes about 79–80% less time.
These are local workload observations, not timing gates, RSS measurements or
universal speedups. Standalone validation still requests 4,688 bytes and has
no intended change. Every music-heavy comparison preserves the ten-frame float
hash `20153f5a9b7d0325`.

The allocation regression also checks that four layers sharing 4,096 path nodes
request less than twice the bytes of one layer. The current ratio is about
1.01; the baseline ratio is about 2.48 and fails this new guard as intended.
This complements the existing large-music-table copy guard without relying on
timing thresholds. Shared-library allocator interposition remains outside this
static-build gate.

`detail::ProjectConfigValidator` owns its validation result and borrows the
canvas/output only until synchronous project validation returns. It exposes no
way to substitute a different canvas for a layer check and rejects layer checks
if the global check failed. The caller keeps these inputs unchanged for that
lifetime. There is no persistent validity cache, new project revision key or
reference passed to render workers. Standalone validation keeps its original
check order. Per-layer saved music tables still require their own scans, and
path bindings still resolve against the current shared path library.

The next bounded F04 phase is to separate immutable runtime canvas/music inputs
from owned, evaluated layer state, starting with project-frame
`apply_global_config` adapters. Preserve saved clocks, named-stream processing,
path resolution, LFO-to-LFO ordering and Live overrides. Enabled-LFO
materialization still copies analysis too. Keep both existing worker-copy
allowances until the corresponding copies are removed and measured. A persistent
cross-call render plan would additionally require explicit revision/dependency
invalidation; this iteration introduces none.

## Validation

Iteration 6 (F04 invariant project validation):

- Native Release/Qt/Metal: **40/40** tests qualified, including allocation,
  core, composition, assets, persistence, export, both surface arithmetic modes
  and all three GUI smoke tests.
- C++20 shared build with Metal disabled and actual OpenGL enabled: all
  **4 focused suites** qualified (core, composition, assets, OpenGL).
- AddressSanitizer + UndefinedBehaviorSanitizer + float-cast-overflow: all
  **4 focused suites** qualified (core, composition, assets, allocations).
  Leak detection remains disabled on this host.
- The first runs in all three builds rejected one new test fixture at the
  global numeric FPS limit before it could exercise the saved-clock frame-count
  limit. The fixture now uses valid FPS with a longer saved music duration.
  Only the affected composition suite needed rebuilding/rerunning, and passed
  in all three builds; the other suites had already passed with the final
  production code. This was a test-data correction, not a product failure.
- The expanded matrix checks **132 direct edits**, including late-layer errors,
  disabled layers, zero opacity, disabled groups, saved clocks, named streams,
  processing changes, Live settings, path/node identities and handles, missing
  wave/effect/motion bindings, LFO graphs and export settings. It checks exact
  standalone diagnostic equivalence, stable estimates, repaired calls,
  independent projects with the same identity and transactional render failure.
  Existing one/two-copy music admission checks remain unchanged in meaning.
- The new path-allocation guard fails against `9ff5d16` and passes against this
  patch; the large-analysis allocation matrix also passes. Two sequential
  baseline/current timing pairs preserve the exact float hash above.
  `git diff --check` passes. Public layouts, persistence, rendering arithmetic
  and backend tolerances are unchanged.
- ThreadSanitizer was not rerun: this phase adds no shared mutable state or
  worker synchronization changes. Iteration 5's targeted evidence remains
  historical. No GitHub platform run or release is claimed for iteration 6;
  iteration 5's remote status was not rechecked here.

Changed files: `src/core.cpp`, `src/composite.cpp`,
`src/frame_renderer_internal.h`, `tests/project_composite_test.cpp`,
`tests/validation_performance_audit_probe.cpp`, and this tracker.
Reused `/tmp/pvt-audit5-{native,gl,san}` with the recorded runtime/compiler
settings. A separate `/tmp/pvt-audit6-reference` builds the committed baseline
plus only the extended measurement probe from
`/tmp/pvt-audit6-reference-source`. Evidence:
`/tmp/pvt-audit6-{native,gl,san}-tests.log`,
`/tmp/pvt-audit6-{native,gl,san}-composite-tests.log`,
`/tmp/pvt-audit6-allocations.txt`,
`/tmp/pvt-audit6-reference-allocation-check.txt`, and
`/tmp/pvt-audit6-validation-{before,after}-pair{2,3}.txt`.
These logs/builds are disposable; the regression sources and this tracker are
the durable continuation record.

Iteration 5 (F04 project validation and discovered sequence-shutdown defect):

- Native Release/Qt/Metal: **40/40** tests passed after A26, including extended
  allocation/validation/cancellation regressions, assets, both surface arithmetic
  modes, persistence, export and all three GUI smoke tests.
- C++20 shared build with Metal disabled and actual OpenGL enabled: all
  **4 focused suites** passed (core, composition, assets, OpenGL).
- AddressSanitizer + UndefinedBehaviorSanitizer + float-cast-overflow:
  all **4 focused suites** passed (core, composition, assets, allocations)
  after the shutdown fix. Leak detection is disabled on this host. The earlier
  core run was terminated after sampling its deadlock; it is not counted as a
  passing run. Other validation suites had already passed before A26.
- ThreadSanitizer: **3/3 focused suites** passed (core, composition, assets),
  including sequence cancellation/exception shutdown and invocation asset
  leases. This is targeted instrumentation, not exhaustive scheduling coverage.
- The 36 direct-edit cases compare project and standalone validation diagnostics,
  estimates, repairs, and transactional failed rendering. Music accounting
  checks retain the exact sample-byte increase for one/two render-time copies.
  The final probe after A26 also preserves the ten-frame hash above.
  `git diff --check` passes.
- Rechecked iteration 4 at `9c058e9`: main run
  [34322841352](https://github.com/gnaservicesinc/procedural_visualizer_tool/actions/runs/34322841352)
  and tagged run
  [34323702474](https://github.com/gnaservicesinc/procedural_visualizer_tool/actions/runs/34323702474)
  passed Windows x64/ARM64, Linux x64/ARM64, and macOS ARM64. Tagged publication
  also passed. This closes iteration 4 platform qualification, not iteration 5;
  downloadable 17.10.0 artifacts were not reaudited here.

Changed files: `src/core.cpp`, `src/composite.cpp`,
`src/frame_renderer_internal.h`, `src/image_io.cpp`, `tests/test_main.cpp`,
`tests/project_composite_test.cpp`, `tests/validation_performance_audit_probe.cpp`,
and this tracker. Public configuration/validation layouts, persistence,
rendering arithmetic and backend tolerances are unchanged.

Fresh builds: `/tmp/pvt-audit5-native`, `/tmp/pvt-audit5-gl`,
`/tmp/pvt-audit5-san`, `/tmp/pvt-audit5-tsan`. All use
`-DCMAKE_BUILD_RPATH=/usr/lib` for the host runtime arrangement noted below.
Metal headers use the same pinned commit and verified archive checksum as the
GitHub workflow. Evidence: `/tmp/pvt-audit5-{native,gl,san}-final-tests.log`,
`/tmp/pvt-audit5-tsan-tests.log`, `/tmp/pvt-audit5-san-core.sample.txt`,
`/tmp/pvt-audit5-validation-before-{repeat,final}.txt`, and
`/tmp/pvt-audit5-validation-after{,-repeat}.txt`, and
`/tmp/pvt-audit5-validation-final.txt`. These files are disposable;
the regression sources and this tracker are the durable continuation record.

Iteration 4 (F04 validation phase):

- Native Release/Qt/Metal: **40/40** tests passed, including the new
  `pvt_validation_allocations` regression, core, composition, assets, GUI,
  persistence, and both surface arithmetic modes.
- After synchronizing 17.10.0 metadata, a fresh native rebuild again passed
  **40/40**. The CLI reports 17.10.0 and passes `--self-test`; a separately
  configured macOS 13 distribution build passed embedded-version, dependency,
  and deep-signature verification for its 50 Mach-O files.
- C++20 shared build with Metal disabled and actual OpenGL enabled: all
  **4 focused suites** passed (core, composition, assets, OpenGL).
- AddressSanitizer + UndefinedBehaviorSanitizer + float-cast-overflow:
  all **4 focused suites** passed (core, composition, assets, allocation
  regression). Leak detection remains disabled on this host. The shared and
  sanitizer core suites were rerun after correcting a new test to validate an
  explicit phase instead of requiring diagnostic precedence during timeline
  evaluation; the other three suites passed before that test-only correction.
- Two sequential baseline/current probe comparisons preserve all float bits
  across ten animated project frames. `git diff --check` passes. Public layout,
  persistence formats, rendering arithmetic, and backend tolerances are unchanged.
- Rechecked iteration 3 at `d3b2ab2`: main run
  [34317155111](https://github.com/gnaservicesinc/procedural_visualizer_tool/actions/runs/34317155111)
  and tagged run
  [34317910726](https://github.com/gnaservicesinc/procedural_visualizer_tool/actions/runs/34317910726)
  passed all five platform jobs; tagged publication also passed. The main Linux
  ARM64 log explicitly records both `pvt_obj_surface` and
  `pvt_obj_surface_contracted` passing, with 39/39 total tests. This closes the
  prior ARM64 qualification concern for iteration 3, not platform qualification
  of the iteration 4 changes until their version commit passes GitHub.
  Downloadable 17.9.0 packages were not reaudited here.

Changed files: `src/core.cpp`, `tests/test_main.cpp`,
`tests/project_composite_test.cpp`, `tests/validation_performance_audit_probe.cpp`,
`CMakeLists.txt`, and this tracker. Reused the three `/tmp/pvt-audit2-*` builds.
Evidence: `/tmp/pvt-audit4-native-qualified.log`,
`/tmp/pvt-audit4-gl-tests.log` plus `/tmp/pvt-audit4-gl-core-tests.log`,
`/tmp/pvt-audit4-san-tests.log` plus `/tmp/pvt-audit4-san-core-tests.log`,
and `/tmp/pvt-audit4-validation-{before,after}-repeat.txt`. Logs are disposable;
the regression sources and this record are the durable evidence.

Iteration 3:

- Native Release/Qt/Metal: **39/39** tests passed, including both surface
  arithmetic modes, asset accounting/lifetime, composition, and GUI suites.
- Separate C++20 shared build with Metal disabled and actual OpenGL enabled:
  **4/4** focused suites passed (core, composition, assets, OpenGL).
- AddressSanitizer + UndefinedBehaviorSanitizer + float-cast-overflow:
  **6/6** focused suites passed (core, composition, assets, OBJ loader, both
  surface modes). Leak detection remains disabled on this host.
- ThreadSanitizer: **3/3** focused suites passed (composition, assets, OBJ
  loader), including concurrent lease reuse and existing publication/prune
  races. This is targeted instrumentation, not exhaustive scheduling coverage.
- Nine baseline animated OBJ hashes match; hidden-face probe verifies exact
  culling-on/off pixels. `git diff --check` passes.
- Historical GitHub run [34301953325](https://github.com/gnaservicesinc/procedural_visualizer_tool/actions/runs/34301953325),
  at `1f2ec86`, passed Windows x64/ARM64, Linux x64, and macOS ARM64. Linux
  ARM64 alone failed `pvt_obj_surface` with the reported camera-inside winding
  message. The attached PDF agrees with that log. These results describe the
  **baseline**, not this local patch. Current Linux/Windows qualification,
  especially the ARM64 failure, remains pending a GitHub run of these changes.

Reused build directories: `/tmp/pvt-audit2-native`, `/tmp/pvt-audit2-gl`,
`/tmp/pvt-audit2-san`; new race build: `/tmp/pvt-audit3-tsan`. Current evidence
is in `/tmp/pvt-audit3-native-tests.log`, `/tmp/pvt-audit3-gl-tests.log`,
`/tmp/pvt-audit3-san-tests.log`, `/tmp/pvt-audit3-tsan-tests-final.log`, and
`/tmp/pvt-audit3-probe-final.txt`. These logs are disposable; the
regression sources and this tracker are the durable continuation record.

Iteration 2:

- Native Release/Qt/Metal: 38/38 tests passed, including updated core, OBJ,
  asset/admission regressions and all three localized GUI smoke tests.
- Separate C++20 shared build, Metal disabled, actual OpenGL enabled: 4/4
  focused suites passed (core, composition, asset lifetime, OpenGL).
- AddressSanitizer + UndefinedBehaviorSanitizer + float-cast-overflow: 5/5
  focused suites passed (core, composition, assets, OBJ loader/surface).
  Leak detection remains disabled on this host.
- Nine animated CPU OBJ hashes match the committed baseline; entry/byte bounds,
  file replacement and publication races are deterministic tests.
- `git diff --check` passed. Windows/Linux and other native GPU drivers remain
  unqualified for this iteration; no release or remote CI is claimed.

Current builds and logs are disposable `/tmp/pvt-audit2-native`,
`/tmp/pvt-audit2-gl`, `/tmp/pvt-audit2-san`, `/tmp/pvt-audit2-reference`, and
`/tmp/pvt-audit2-*.log`/`*.txt`. The source probe and this tracker are durable.

Iteration 1 historical evidence:

- Baseline native Release/Qt build: 38/38 tests passed.
- Changed native Release/Qt/Metal build: 39/39 tests passed, including new
  asset-lifetime tests, OBJ race and edge regressions, all three localized GUI
  smoke tests, rendering, bundle, and export tests.
- AddressSanitizer + UndefinedBehaviorSanitizer + float-cast-overflow: 5/5
  focused suites passed (core, composition, asset lifetime, OBJ loader, OBJ
  surface). The subsequent weak-height-reference change was rechecked with
  the core and asset suites. Leak detection was disabled for this local host;
  this is not proof of process-wide leak freedom.
- Separate C++20 **shared** build with Metal disabled: 4/4 focused suites passed
  (core, composition, asset lifetime, real OpenGL). The OpenGL test verifies
  GPU mesh ownership is released and exact output returns after re-upload.
  The weak-height-reference change was rechecked in its affected suites.
- All 12 CPU/Metal animated benchmark hashes match the original build.
- `git diff --check` passed. No Windows/Linux execution or release gate is
  claimed for these local modifications.

Build artifacts/logs are in ignored local directories:
`build-audit-20260908`, `build-audit-sanitized-20260908`, and
`build-audit-opengl-shared-20260908`. The first contains the baseline executable,
benchmark outputs, and native test logs. Sanitizer/OpenGL logs also use
`/tmp/pvt-audit-*`. They are disposable; the source probe and this record are
the durable reproduction instructions.

The local compiler emitted a C++ runtime deployment-version warning. Initially
test binaries could not locate `@rpath/libc++.1.dylib`; configuring the audit
builds with `-DCMAKE_BUILD_RPATH=/usr/lib` resolved execution. This build-local
adjustment does not change product packaging.

### Reproduce the main checks

```sh
cmake -S . -B build-audit -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPVT_BUILD_QT_GUI=ON -DBUILD_TESTING=ON
cmake --build build-audit -j 8
ctest --test-dir build-audit --output-on-failure -j 4
cmake --build build-audit --target pvt_performance_audit_probe
build-audit/pvt_performance_audit_probe
build-audit/pvt_performance_audit_probe gpu
```

Supply the locally installed Qt and metal-cpp paths as appropriate. For the
OpenGL qualification on macOS, use a separate build with
`PVT_ENABLE_METAL=OFF`, `PVT_ENABLE_QT_OPENGL_WITHOUT_GUI=ON`,
`PVT_TEST_OPENGL_SURFACE_ON_APPLE=ON`, `BUILD_SHARED_LIBS=ON`, and
`CMAKE_CXX_STANDARD=20`. For sanitizer checks use
`-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer` and
disable GPU backends. Do not mistake an OpenGL-named test using Metal dispatch
for evidence of OpenGL execution.

## Remaining work and iteration queue

Closed entries retain their IDs for continuity. Resume an open item by ID, verify current source, and
record changed files, regression evidence, measurement, and final status here.
Choose one bounded group per iteration rather than repeatedly rescanning the
entire repository under a token limit.

| ID | Priority/status | Finding or strategy | Next implementation and acceptance gate |
| --- | --- | --- | --- |
| F01 | Closed in iteration 2 | Camera-plane clipping implemented on CPU and OpenGL in A13, with the additional interpolation defect fixed in A17. | Independent geometry, camera-inside, both windings, transparency, and local backend parity checks pass. Other driver qualification remains in F09. |
| F02 | P2, partially resolved in iteration 3 | A18/A21 add shared allocation accounting, decoded assets, and invocation leases, separated from worker/composite storage. A20 includes its tile allowance. | Add cold parser/topology/decode scratch and lease/hash/control-block overhead; bound dynamic LFO-generated geometry and file replacements after admission. Coordinate outer sequence admission and independent concurrent invocation ledgers. Measure aggregate RSS and driver/context heaps under preview/export. One oversized worker is still allowed; this is not a hard process-memory cap. |
| F03 | Closed in iteration 2 | A15 implements a 16-entry/512 MiB OBJ LRU with per-asset publication fencing. | Three alternating assets require 3 parses across 60 requests; generation, entry/byte eviction, oversized bypass, same-path replacement, and pruning regressions pass. |
| F04 | P2, partially resolved in iteration 6 | A23/A24 remove recursive clock and LFO-target copies; A25 removes project-validation value adapters; A27 checks shared canvas/clock/music/path/export invariants once per invocation. Direct edits and exact animated output remain observable, and render-worker allowances are preserved. | Next, separate immutable runtime canvas/music inputs from owned evaluated layer state to remove project-frame adapters, then enabled-LFO analysis copies. Preserve saved clocks, named streams, paths, LFO order and Live overrides. Reduce admission allowances only after actual copies are removed and measured. Persistent cross-call plans require explicit revision/dependency invalidation. |
| F05 | P2, partially resolved in iteration 3 | A21 adds invocation leases and proves last-owner release for uncached assets. Cache eviction no longer forces duplicate loads within an admitted project frame. Process caches still serve independent projects. | Add project/revision-aware cache publication so an obsolete caller cannot repopulate unused entries after a newer prune. Test concurrent projects and late loads without a subsequent frame; preserve assets needed by other active renders. Extend shared ownership across outer sequence workers before claiming process-wide accounting. |
| F06 | P2, conservative retention | An enabled LFO currently conservatively retains potentially used surface assets regardless of its target. Multiple decode intents for one retained image path and old mesh variants may also survive until normal LRU eviction. | Track exact asset/decode-intent dependencies and displacement keys in a render plan. Handle LFOs targeting other LFOs, skipped cycles, path bindings, and Live overrides before tightening retention. |
| F07 | P2, partially resolved in iteration 3; winding strategy constrained | A20 adds depth-proven rejection for arbitrary nearest-hit CPU meshes, alongside A16 view rejection and A02 analytic rear-shading bypass. Winding alone cannot identify hidden faces of existing two-sided/open/translucent surfaces. | Qualify occlusion across platform arithmetic and dense real scenes. Consider tighter interval bounds or safe depth-peeling rejection only with exact-output evidence. Winding-only rejection still requires a proved topology/view/material/transform condition or an explicit authored one-sided setting. |
| F08 | P2, further memory reduction | Disabled layer definitions, cached music analysis, and undo/history remain authored state. Deleting those objects would break re-enable, persistence, and undo; A06 releases their render assets instead. | If authoring memory itself must be paged out, design lossless lazy storage with transactional reload, portable paths, recovery, and undo/version tests. This requires a document-lifecycle change, not Boolean packing. |
| F09 | P2, locally qualified through iteration 6 | Iterations 3 and 4 main/tagged GitHub builds passed all five platforms, including the earlier ARM64 surface concern. Iteration 5 local qualification exposed and fixed A26; iteration 6 native/shared OpenGL and focused allocation/address/undefined sanitizer suites qualify A27. | Require GitHub platform matrices for changes after iteration 4 before claiming Linux/Windows qualification; iteration 5 remote status was not rechecked here and iteration 6 remains local. Continue forced PNG/GL allocation failure and driver/resource/throughput qualification, preserving thin-triangle coverage, transparency, transactional shutdown/failure and exact hashes. Linux/Windows builds belong on GitHub. |

Other configuration simplifications were evaluated but are not silently
applied: merging floating-point effects, lowering mesh/image resolution,
approximating curves, and flattening layers can alter results. Existing exact
disabled/zero-work effect bypasses remain. Skipping layers beneath opaque
content requires a proof covering blend mode, AlphaUnder, erasers, HDR RGB,
ordered finishing stages, and animated alpha; opacity alone is insufficient.
