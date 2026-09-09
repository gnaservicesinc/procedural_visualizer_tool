# Performance and correctness audit

Updated: 2026-09-08. Iteration 1, against commit
`d1e521e2ade3a28237bc33f8eeb8cc4fcf0d7b8e` (17.8.0).

This is the continuation record for the requested performance, memory,
configuration, and latent-defect audit. Changes are local and uncommitted;
this pass does not publish a release. Preserve the pre-existing example-project
edits and `tests/.DS_Store` when committing this work.

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

## Validation

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

These items are deliberately open. Resume by ID, verify current source, and
record changed files, regression evidence, measurement, and final status here.
Choose one bounded group per iteration rather than repeatedly rescanning the
entire repository under a token limit.

| ID | Priority/status | Finding or strategy | Next implementation and acceptance gate |
| --- | --- | --- | --- |
| F01 | P2, confirmed limitation | `obj_surface.cpp` rejects a complete triangle when any projected vertex is behind/too close to the camera. A crossing triangle can lose its visible portion. | Implement camera-plane polygon clipping with interpolated UV/normal/world values and match the OpenGL geometry path. Cover one/two vertices behind, camera-inside views, all windings, and transparent depth peeling. Rendering semantics and backend parity make this a separate geometry change. |
| F02 | P2, confirmed estimate gap | `project_positions` and `transform_normals` allocate per-vertex storage that is not represented explicitly in the mesh working-memory allowance; ordinary imported OBJ topology is inspected only for active construction plans. | Include projected vertices, transformed normals, and backend uploads in checked admission estimates, distinguish shared cache bytes from per-worker bytes, and test very large meshes under a small multiworker budget. |
| F03 | P2, confirmed bottleneck | The OBJ cache has one strong entry. Alternating distinct OBJ layers/files can repeatedly parse and rebuild topology. | Benchmark an alternating two/many-OBJ project; evaluate a byte-bounded multi-entry cache while preserving single-flight loading, generation fences, file replacement, and eviction. Do not retain unbounded meshes. |
| F04 | P2, optimization candidate | `validate_impl` recursively copies a complete render configuration to validate the saved layer clock. Project materialization also copies music tables; repeated validation rebuilds maps/graphs. | Extract clock-only validation and investigate an immutable render plan/shared analysis data with explicit invalidation. Preserve direct API edits, disabled-state validation, LFO dependency order, and live/project revision behavior. Benchmark animated large-analysis projects. |
| F05 | P2, lifecycle extension | Pruning drops current cache owners and fences already-pending loads. A different or stale caller can start a new load after pruning. Independent projects also compete for process-global caches. | Introduce explicit render-session asset leases if concurrent-project isolation is needed; test old-frame late loads, concurrent export/preview, and the last active reader finishing without another frame. Do not force-delete assets needed by another active render. |
| F06 | P2, conservative retention | An enabled LFO currently conservatively retains potentially used surface assets regardless of its target. Multiple decode intents for one retained image path and old mesh variants may also survive until normal LRU eviction. | Track exact asset/decode-intent dependencies and displacement keys in a render plan. Handle LFOs targeting other LFOs, skipped cycles, path bindings, and Live overrides before tightening retention. |
| F07 | P2, strategy constrained by fidelity | General winding-based backface culling is unsafe for the current two-sided OBJ/displacement contract, open meshes, inside-camera views, transparency, and exploded/fragmented surfaces. Exact analytic rear-shading culling is implemented in A02. | Only add triangle culling after proving eligibility per topology/view/material/transform or introducing an explicit authored one-sided setting. Compare complete float outputs for reversed winding, mirrored transforms, concavity, silhouettes, partial curvature, and layered transparency. |
| F08 | P2, further memory reduction | Disabled layer definitions, cached music analysis, and undo/history remain authored state. Deleting those objects would break re-enable, persistence, and undo; A06 releases their render assets instead. | If authoring memory itself must be paged out, design lossless lazy storage with transactional reload, portable paths, recovery, and undo/version tests. This requires a document-lifecycle change, not Boolean packing. |
| F09 | P2, qualification pending | Native Windows/Linux GPU paths, race instrumentation, and allocation-failure injection have not been run for this patch. Scope cleanup addresses identified exceptional paths, but fault injection is still needed to measure driver/decoder resource counts. | Run platform CI, targeted ThreadSanitizer interleavings, and forced failure after PNG/GL allocations. Check resource counts/context state and preserve destinations on error/cancel. Re-run exact hashes after any fixes. |

Other configuration simplifications were evaluated but are not silently
applied: merging floating-point effects, lowering mesh/image resolution,
approximating curves, and flattening layers can alter results. Existing exact
disabled/zero-work effect bypasses remain. Skipping layers beneath opaque
content requires a proof covering blend mode, AlphaUnder, erasers, HDR RGB,
ordered finishing stages, and animated alpha; opacity alone is insufficient.
