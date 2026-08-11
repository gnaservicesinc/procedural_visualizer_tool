# Procedural Visualizer implementation ledger

Last updated: 2026-08-09

This is the hand-off point for humans and future coding agents. This repository
is the canonical working tree. Any loose C files retained outside it are legacy
snapshots, not inputs to the current build.

## Outcome of this pass

The application is now project-oriented. A named `ProjectConfig` owns global
canvas/loop and export data plus up to 64 independently configurable render
layers. Each layer has a stable UUID and bundle file ID, name, enabled state,
opacity, blend mode, and full `RenderData`. The backward-compatible
`RenderConfig`/`.pvt` surface remains available for one-render library clients
and legacy import.

Composite rendering is float RGBA, linear-light, straight-alpha, and bounded.
It renders one layer image at a time into a single accumulator instead of
retaining all layer frames. Normal (`none`) source-over plus Soft Light, Grain
Merge, Overlay, Color Dodge, Linear Burn, Color Burn, Difference, Subtract,
Multiply, and Add are supported. Per-layer opacity is saved; Solo is deliberately
session-only. Procedural alpha modulation is now per-layer artwork while final
RGB/RGBA channel selection is global output data. Adding a second layer enables
final alpha without changing either layer's art.

Normal Save now creates a versioned ZIP or unpacked project bundle. Each numbered
snapshot owns its `render_output.txt`, layer `.pvt` files, and checksummed
metadata. Root metadata and its separate SHA-256 sidecar index immutable versions;
`current` is a portable checksummed text pointer rather than a filesystem
symlink. Changed saves append, no-change saves fully validate, Make Current only
changes root bookkeeping/pointer state (never a snapshot), and Revert creates a
new highest-numbered snapshot. Loads are transactional/read-only, fall back from
a bad current snapshot in descending numeric order, and promote parseable
external changes only on the next explicit Save. Legacy imports lose their
source association so a normal save cannot overwrite the old `.pvt`.

Full validation accounts for every numeric directory. Noncanonical valid or
malformed raw trees are retained in an exact checksummed preserved-history table,
and lineage aliases keep descendants valid when an ancestor is externally
changed or removed. A hidden sibling OS advisory lock plus an expected whole-tree
digest comparison serializes cooperating writers and closes the stale-save
check/commit window without placing machine-specific lock state inside bundles.

The sanitized archive/directory root is selected on first Save/Save As and then
stays stable for that associated bundle when the user chooses to keep its
filename. Renaming a saved project in the GUI now offers keep-filename, create
and open an independent copy, create a copy while staying in the original, and
Cancel. Independent copies transactionally carry only the current working state
as version 0, regenerate project/layer UUIDs and bundle-local file IDs, and
refuse existing destinations. The stay-here path never mutates the open project
and restores its previous displayed name.

The ZIP/directory boundary is hostile-input hardened: traversal, absolute or
platform-special paths, malformed UTF-8/NUL, collisions, symlinks/special files,
encryption/multidisk/unsupported compression, expansion ratios, counts, sizes,
unexpected records, checksums, UUIDs, enums, and values are bounded and checked.
New directory snapshots and ZIP replacements use sibling staging/atomic rename;
stale or divergent destinations are refused.

All registered files now use a generic readable attachment store. Music, custom
OBJ meshes, images, and future attachment types are copied into a managed cache
at import and saved as `assets/<sha256>/<original-filename.ext>`. The digest
directory prevents collisions while the actual asset retains the exact user
filename and extension. Valid direct replacements or unambiguous renames are
loaded dirty and promoted with fresh identity metadata on Save; directly
replaced music is reanalyzed before acceptance. Legacy version-2 bare-digest
assets remain readable.

The Qt GUI separates Synchronization, Layer Render, and global Output, adds a
topmost-first Layers dock, project name/title, blend/opacity/enable/rename/
duplicate/reorder controls, session Solo, version list/diff/current/revert tools,
and application-wide undo/redo. Synchronization owns the global Clock plus the
selected layer's master Swing and Audio Response blocks. Undo depth, window state,
and dialog locations use per-user platform settings, while a hard 128 MiB history
budget bounds full-state snapshots.
Preview/export cancellation now reaches per-frame effects and OBJ rasterization;
stale previews are cancelled and document replacement is blocked during export.
The GUI now also exposes draggable numbered center handles and visible radius
rings for effects and swings, Texture versus Mapped-object effect stages,
custom/built-in per-layer palettes, and directional mirror plus
horizontal/vertical layer transforms. Block Scale remains a whole-image effect
without a center/radius overlay. Version 5 adds a project-wide Default/Frame/
Time/Meter/Music clock, parameter-state Hold/Linear/Smoothstep interpolation,
time-varying music analysis, beat navigation, and layer audio-response routing.
Formats 1-4 load with neutral compatibility defaults.
The CLI exposes the same clock/music/swing/audio-response state plus immediate
portable music and OBJ attachment import.

Music analysis is deliberately not a fixed whole-song BPM estimate. A causal
local beat/tempo observer is reconciled with offline multiband spectral-flux and
local-tempogram evidence, while dense feature samples retain Energy, Bass,
Midrange, Treble, Onset, Beat, Spectral Centroid, Spectral Flatness, Chroma Hue,
and Chroma Strength. These independent routes can modulate waves, effects, and
color at the actual frame time, including between clock pulses. Interpolation
always evaluates procedural parameters; rendered RGBA frames are never
crossfaded.

Music-mode frame count is derived from decoded sample frames/sample rate and
FPS while preserving the stored manual count for later modes. Export remains
the supported PNG/EXR image-sequence workflow.

Sequence export now uses independent-frame CPU workers for rendering and
encoding. Automatic selection is bounded by hardware concurrency, frame count,
256 workers, and a conservative aggregate memory budget computed from the
validated per-frame peak. Output installation and progress callbacks remain in
ascending frame order. On the representative Apple M2 Max workload recorded for
this pass (24 frames, 960x540, block size 1, 64 waves, PNG compression 0), 12
workers reduced wall time from 44.95 seconds to 5.44 seconds (8.3x), with all 24
PNGs byte-identical. This is evidence for that workload, not a general speedup
guarantee.

The CLI opens ZIPs, directories, and legacy setups; renders composite projects;
edits the selected layer; manages bounded layers; separates final alpha from
modulation; performs normal bundle saves; and reserves `--save-legacy` for an
explicit one-layer export.

The earlier C-to-C++ migration, float pipeline, ordered effects, quantization,
dithering, PNG/EXR writing, custom OBJ mapping, strict legacy setup codec, and
portable installed core library remain intact. Bundle code is a separate
non-installed helper with internal portable SHA-256, so core-only/static
consumers do not inherit minizip requirements.

## Requested work

| # | Request | Status | Result |
|---|---|---|---|
| 1 | More seamlessly looping effects | Complete | All animated controls use integer cycles over the half-open loop interval. |
| 2 | Optional synchronization per wave/effect | Complete | Each wave and effect has a `synchronized` toggle; free clocks are independent but still periodic. |
| 3 | Toggle/add/remove/reorder any quantity | Complete | Bounded dynamic wave, swing, and effect collections support zero through their safety limits in the API, CLI, and GUI. |
| 4 | Direction from horizontal through radial to vertical | Complete | Continuous `0.0` horizontal, `0.5` radial/default, `1.0` vertical control. |
| 4a-f | Endless zoom, ripple, shake, flag wave, glow, block scale | Complete | Ordered, configurable effects; coordinate effects support alpha/black/white/reflected edges; Glow uses visible straight-alpha-safe HDR bloom; Block Scale animates smooth or stepped pixel grouping at its stack position. |
| 5 | 8/16-bit PNG and 32-bit float EXR | Complete | RGB/RGBA PNG at 8 or 16 bits per channel with compression levels 0-9 (default 5); RGB/RGBA uncompressed scanline EXR with FLOAT channels. |
| 6 | Optional alpha throughout | Complete | Internal images are straight float RGBA, including meaningful RGB at zero alpha; RGB export drops only the fourth channel. |
| 7 | Float processing and lower-depth dithering | Complete | Linear float image pipeline; sRGB conversion plus blue-noise-like, Bayer, or Floyd-Steinberg dithering for PNG; dither is off/ignored for EXR. |
| 8 | Safe setup save/load | Complete | Versioned deterministic `.pvt` files, bounded strict parser, complete validation, transactional load, atomic/durable save, UTF-8 paths, and no NUL truncation. |
| 9 | Library-only build and useful Qt-facing API | Complete | `libProceduralVisualizerTool`, public header, installable CMake package, example consumer, and build switches that omit every `main`. |
| 10 | Qt GUI using the library | Complete | Qt Widgets client with live async preview, draggable wave/swing/effect center handles and visible local-radius rings, dynamic stack editors, all configuration fields, timeline, project bundle/legacy import I/O, and background export. |
| 11 | More quantization/swing levels and variations | Complete | 2-65,536 levels, RGB/luminance/hue modes and mix; dynamic sine/triangle/smooth-pulse/bounce swing stacks. |
| 12 | Plane/cube/sphere/cylinder/custom OBJ wrapping | Complete | Analytic built-ins plus bounded cached OBJ parsing and perspective rasterization; authored UV/normal data has safe fallbacks. All mappings are two-sided and transparent closed surfaces composite entry/exit samples. |
| 13 | Configurable PNG compression | Complete | Levels 0-9 are available in the API, setup v2-v5, CLI, and GUI; level 5 is the balanced default and EXR ignores it. |
| 14 | Randomize stack values or composition | Complete | Separate GUI actions preserve existing identity/type structure or create a new bounded mix of waves, swing waveforms, effect types, and enabled items. |
| 15 | Stable paths, dialogs, and playback | Complete | Relative paths are anchored to a stable launch directory, first file dialogs use home then remember their last location, completed previews advance during Play even under timer/render overlap, and Space owns preview play/pause outside text editors instead of toggling focused boxes. |
| 16 | Named projects and default filenames | Complete | Semantic name appears in the title and a portable sanitizer supplies the initial `.zip`. Saved-project rename choices either preserve that path or create a current-state-only independent project with fresh identities, then open it or stay in the original. |
| 17 | Full render-config layers | Complete | Global canvas/export data is split from per-layer render data; layer switches cannot overwrite global output state. Stable UUIDs/file IDs survive names and reorders. |
| 18 | Layer compositing | Complete | Sequential bounded float-RGBA compositing implements all 11 requested modes plus opacity; only one layer frame and one accumulator are retained. |
| 19 | Alpha split | Complete | Per-layer procedural modulation and global final RGB/RGBA selection are independent. Multi-layer creation enables final alpha without changing artwork; validation follows actual final-composite transparency. |
| 20 | Human-readable project bundles | Complete | ZIP/directory bundle tree stores root/version metadata, per-version global output, per-layer `.pvt` data, SHA-256 indexes, and a portable text current pointer. |
| 21 | Automatic immutable save versions | Complete | Changed Save appends; clean Save validates; load fallback, external-change promotion, semantic diff, Make Current, revert-as-new, and advisory newer-program warnings preserve recoverability. |
| 22 | Legacy compatibility without overwrite | Complete | Setup v1-v4 imports into a new unsaved one-layer project; setup v5 adds clock/music/audio-response and attachment identity data. Only explicit one-layer `--save-legacy` writes `.pvt`. |
| 23 | GUI session undo/redo and preferences | Complete | All editor/structural actions use undo/redo; an extensible Application Settings dialog exposes the step limit and rendering backend from a top-level menu and toolbar, while the hard 128 MiB history budget and all UI preferences live in per-user `QSettings`, outside bundles. |
| 24 | Hostile-input bundle handling | Complete | Strict tree/archive/metadata bounds and checks reject traversal, collisions, links/special files, unsupported/encrypted archives, expansion abuse, stale saves, and invalid typed data transactionally. |
| 25 | Saved-project rename workflow | Complete in Qt GUI | Keep the existing filename, Save As/open an independent version-0 copy, save that copy and stay with the old name restored, or Cancel. Copy creation is no-clobber and the open-document swap is transactional. CLI name edits remain ordinary semantic renames. |
| 26 | Better CPU utilization | Complete | Bounded frame-level render/encode workers, ordered atomic install, serialized progress, `--workers 0..256`, and auto selection in GUI/library. Representative M2 Max measurement: 44.95 s to 5.44 s (8.3x). |
| 27 | Texture versus mapped-surface effects | Complete | Each effect runs before surface mapping or after it; mapped-object coordinate effects move/deform the primitive silhouette. Relative order is retained within each stage. |
| 28 | Draggable/localized effects | Complete | Numbered preview handles edit centers; zero area radius preserves whole-layer behavior and positive radii add feathered local influence. Glow blur and influence radii remain separate. |
| 29 | Localized Swings | Complete | Zero radius retains global clock modulation; positive shorter-edge-relative radius creates a movable feathered source/UV timing region for waves and Texture effects. Mapped-object effects use the global synchronized clock because projection is not uniquely invertible. |
| 30 | Per-layer starting palettes | Complete | 1-256 embedded sRGB source colors, six presets, custom GUI/CLI editing, reliable independent enablement, and once-per-procedural-block linear-light selection. Lighting and effects may create other colors afterward; post-effects quantization remains separate. |
| 31 | Transform layer | Complete (requested scope) | Directional horizontal/vertical/four-way mirrors plus horizontal and vertical flips run after surface mapping and before mapped-object effects and post-effects quantization. This is not a general move/scale/rotate affine transform. |
| 32 | Metal GPU acceleration | Complete | Backend-neutral CPU/CPU+GPU/strict-GPU rendering accelerates live preview and export through cached metal-cpp pipelines, three bounded shared frame buffers per admitted render, analytic surface/effect kernels, transactional cancellation, and CPU/Metal image/straight-alpha/seam parity tests. Hybrid project frames pair CPU and Metal layer lanes with ordered compositing and resilient fallback; custom OBJ depth peeling intentionally remains CPU-only. |
| 33 | Layer starting image | Partially complete - asset foundation | Generic image bytes can already be registered, bounded, checksummed, deduplicated, embedded, and recovered after original deletion. The layer source-mode schema/decoder/rendering/editor remain deferred. |
| 34 | Closed reusable motion paths | Deferred - designed | Store named closed cubic paths separately from per-wave/effect/object bindings; use at least three nodes, handle modes, bounded arc-length LUT sampling, independent sync/phase/direction, and a dedicated editor. |
| 35 | Synchronization tab and clock controls | Complete | Global Default/Frame/Time/Meter/Music clock, Hold/Linear/Smoothstep parameter interpolation, fit/exact spacing, direction/phase/beat offset, and an authoritative per-layer Swing block live in one GUI tab and in the CLI/API. Music no longer adds a redundant swing-suppression control or popup. |
| 36 | Adaptive high-detail music response | Complete | Full-source decoding plus time-varying beat/tempo reconciliation and 8,192-sample multiband/onset/spectral/chroma analysis drive the base clock and independently routable wave/effect/color response. First import enables active-layer response, Energy supplies a visibly dynamic default hue route, and later user overrides remain intact. No fixed whole-song BPM clock is used. |
| 37 | Music-video export | Removed | The broken MP4-with-embedded-music option, FFmpeg helper, and its test target were removed; image-sequence export remains supported. |
| 38 | Portable embedded attachments | Complete | Music, OBJ, image, and generic files retain exact filenames/extensions beneath collision-safe digest directories, accept valid direct replacements as first-class edits, keep managed copies, and retain hostile/oversize rejection plus v2 compatibility. |

## Important implementation map

- `include/procedural_visualizer_tool.h`: public C++ API and complete owned config model.
- `src/core.cpp`: defaults, validated clock/meter/music-event evaluation,
  parameter interpolation, audio-response routing, periodic/spatial phases,
  staged effects, palettes, transforms, float RGBA layer rendering,
  quantization, alpha, and analytic surface mappings.
- `src/frame_renderer.cpp` / `src/metal_backend.cpp` / `src/metal_kernels.metal`:
  backend-neutral dispatch, CPU fallback, cached metal-cpp resources and compute
  pipelines, bounded admission, transactional cancellation, and GPU kernels for
  base generation, ordered effects, analytic surfaces, transforms, and
  quantization. Non-Apple/disabled builds use `src/metal_backend_stub.cpp`.
- `src/composite.cpp`: project/layer validation, UUIDs, linear-light blend modes,
  bounded ordered compositing, hybrid CPU/Metal layer pairing, and project frame
  rendering.
- `src/obj_mesh.cpp` / `src/obj_surface.cpp`: bounded Wavefront parsing/cache and
  perspective, two-sided, layered custom-mesh rasterization.
- `src/image_io.cpp`: PNG/EXR encoding, bounded frame-worker scheduling, PNG
  compression, dithering, collision preflight, ordered atomic installation,
  serialized progress, and cancellation checks.
- `src/config_io.cpp` / `src/config_codec.cpp`: setup v1-v5 codec and split
  per-layer/global bundle records with transactional legacy file I/O.
- `src/project_bundle.cpp` / `src/bundle_archive.cpp`: checksummed project/version
  metadata, semantic history operations, readable/direct-editable attachment storage,
  independent current-state copies, bounded ZIP/directory loading, and atomic
  save staging. This helper is intentionally not installed ABI.
- `src/audio_analysis.cpp`: private full-source decoding, adaptive beat/local-
  tempo reconciliation, and dense multiband/onset/spectral/chroma extraction.
- `third_party/miniaudio` / `third_party/btt`: pinned private decoder and causal
  beat/tempo observer; neither enters the installed core ABI.
- `app/cli_main.cpp`: project/layer-aware interactive client plus scripted
  clock, music, attachment, audio-response, and worker controls.
- `gui/main_window.cpp`: Qt 6 project/layer/version client, Synchronization UI,
  asynchronous analysis, undo/redo, saved-rename workflow, spatial centers,
  palettes/transforms, adaptive unclipped layouts, and cancellable sequence export.
- `tests/test_main.cpp`, `tests/project_composite_test.cpp`, and
  `tests/bundle_test.cpp`: core/seam/format/setup/I/O, layer/blend/project, and
  persistence/archive-safety coverage, including worker determinism, setup-v5
  compatibility, clock/music response, staged/local effects, embedded assets,
  palettes/transforms, readable/direct-edited assets, and rename copies.
  `tests/audio_analysis_test.cpp` covers analysis accuracy and density.

## Guardrails retained

- The Default clock samples `[0, N)` and omits the duplicated endpoint. Frame,
  Time, Meter, and Music clocks evaluate bounded authored/data-derived anchors;
  sequence-fit is explicit instead of silently changing exact intervals.
- Hold/Linear/Smoothstep interpolate the evaluated clock and procedural
  parameters, never finished pixel buffers. Dense audio response uses actual
  frame time independently of sparse beat anchors.
- The active-layer Swing checkbox is authoritative under every clock. Legacy
  music-swing policy values remain readable for setup compatibility but do not
  silently suppress the authored block.
- Sequence workers operate on independent frames, not layers. The requested
  count is capped by frames, hardware/explicit request, 256, and a default 2 GiB
  aggregate estimate. Encoding may overlap, but installation and progress remain
  ordered and the complete collision preflight still happens before frame zero.
- Effects are grouped into two explicit stages: every Texture effect runs before
  surface wrapping and every Mapped-object effect afterward. List order remains
  stable within a stage; the mapping boundary cannot be interleaved ambiguously.
- Effect and Swing radii use zero as the backward-compatible whole-layer mode.
  Positive values are feathered circles measured against the shorter canvas
  edge; Glow's pixel blur radius is not reused as its influence radius.
- The explicit pipeline is procedural base generation, optional starting-palette
  selection, Texture effects, surface mapping, layer mirror/flips,
  Mapped-object effects, then explicit post-effects quantization. A later
  localized mapped effect can intentionally break earlier mirror symmetry;
  neither starting-palette selection nor transforms rewrite alpha.
- Version 4.0.1 intentionally reinterprets the existing v4 `palette.enabled`
  field as source-stage enablement. No schema or ABI layout changed, but 4.0.0
  projects affected by the final-palette bug render differently after correction.
- Project layers are stored bottom-to-top; the GUI reverses that order for a
  conventional topmost-first list. Rendering skips disabled layers and composites
  enabled layers sequentially into one accumulator.
- Layer UUIDs and file IDs are unique and stable. Reordering never renames layer
  files; deletion deliberately leaves gaps. Session Solo is never persisted.
- IDs are stable, nonzero, and unique after insertion; factory objects start at
  ID zero and must receive `allocate_id(config)`.
- Collection counts, values, decoded strings, file sizes, image dimensions, and
  an estimated 1 GiB peak working set are bounded before large allocations;
  custom OBJ estimates conservatively include the maximum cached mesh and its
  projected position/normal arrays.
- Existing output frames are protected unless overwrite is explicit, including
  an all-frame preflight and atomic no-clobber installation.
- Transparent effect edges and curved 3D-surface exteriors can generate alpha;
  validation rejects RGB export only when the final composite can retain
  transparency. An opaque lower stack may make a translucent upper layer safe
  for RGB, although adding a second layer defaults final output to RGBA.
- Transparent closed surfaces composite distinct front and rear samples. OBJ
  rasterization never culls by winding and peels at most eight distinct depths.
- The core library has no Qt dependency.
- Each numbered bundle snapshot owns global output plus every layer. Version
  directories are immutable; `current` is regular checksummed text, and root
  metadata cannot self-checksum (the digest lives in `metadata.sha256`).
- The archive/directory root is a stable bundle identity chosen at association;
  project display name belongs to each snapshot and may change without a move.
- A rename-created independent project has no inherited source/CAS token or
  history, regenerates project/layer identities, and may only target a new path.
  Copy-and-stay leaves the original document and undo/dirty state unchanged.
- A checksum mismatch is treated neutrally as an external change/integrity
  signal, not proof of authorship. Opening is read-only; parseable valid changes
  become dirty and are promoted only by explicit Save.
- Bundle archive/tree entry counts, sizes, paths, types, ratios, metadata records,
  versions, and layer counts are bounded before allocation or extraction.
- Every numeric version directory is either canonical or explicitly preserved
  by its exact raw-tree digest; no clean validation silently skips orphaned,
  malformed, or externally removed history.
- Bundle commits hold a hidden sibling advisory lock and compare the complete
  expected on-disk state while locked; lock sidecars contain no project data.
- New attachments live at `assets/<sha256>/<original-filename.ext>`. The final
  component is always the imported filename, while the parent identity prevents
  collisions. Valid direct replacements are treated as external edits and Save
  gives them a fresh identity; legacy bare-digest assets remain readable.
- Music rendering trusts only cached analysis tied to a valid embedded source
  digest. Relink verifies identity; reanalysis is the explicit content-change
  path. The clock uses event times, never one global BPM estimate.
- GUI undo snapshots have a separate 128 MiB hard budget. Oversized or trimmed
  history never clears the document's dirty state, and no-op normalized edits
  do not create commands.

## Validation record

The 2026-08-10 release-blocker correction passed the clean Qt-enabled Release
suite 15/15 with standard IEEE-safe optimization flags and the workspace's
active-SDK linker path. Focused bundle coverage also passed direct image and
music replacement, automatic music reanalysis, exact filename/extension
storage, directory/ZIP round trips, version-2 asset migration, invalid
replacement rejection, and post-promotion validation. Offscreen GUI smoke plus
captured Effects/Output pages verified full action/tab/form text and the absence
of the removed MP4 target. `git diff --check` passed.

The 2026-08-09 starting-palette correction passed the Qt-enabled Release suite
14/14, a fresh C++20 non-GUI suite 13/13, and the AddressSanitizer plus
UndefinedBehaviorSanitizer non-GUI suite 13/13. Regression coverage now locks
the base-palette/effect/quantization order, populated-palette bypass when off,
GUI toggle Undo/Redo and preset state, exact setup/layer/bundle round trips, and
saved-version semantic diffs for enablement and color changes.

Focused `pvt_bundle` and offscreen `pvt_gui_smoke` suites passed on 2026-08-09
after the saved-project rename work, including all four prompt actions,
current-state-only copy identity/history, copy-and-stay restoration, and
transactional open-copy replacement. The representative parallel-export
measurement and its byte-equality result are recorded above; they characterize
that workload rather than promising the same scaling for every project.

The earlier version 5 integrated Release build passed all 16/16 Qt-enabled CTests
on 2026-08-09. Coverage includes core rendering, project compositing, bundle
persistence, adaptive audio analysis, CLI flows, OBJ handling, the then-present
music-video exporter, and GUI smoke paths. The difficult 90 BPM fixture with
accented eighth-note subdivisions and a missing beat remained on its changing
local grid to floating-point precision; a fractional-duration regression also
guards against padded tracker events beyond the final decoded sample. Manual
float-WAV, FLAC, and MP3 imports saved and reloaded from their embedded bundle
assets while every original source was moved away. `git diff --check` passed and
the tree contains no MiniBPM reference.

A fresh C++20 Release build passed 13/13 non-GUI CTests on 2026-08-09 after
exercising the standard's `char8_t` filesystem path behavior. A fresh Debug
AddressSanitizer plus UndefinedBehaviorSanitizer build passed the same 13/13
tests (LeakSanitizer is not supported by this macOS runtime). A core-only configure built without
creating a dependency-fetch directory. Static and shared installed-package
consumers both configured, built, and ran; the installed shared CLI retained its
relative sibling-library runtime path, passed self-test, and still ran after the
whole install tree was relocated.

The final CLI dependency scan contains libpng, zlib, libc++, and the system
runtime only; minizip-ng is embedded statically and bundle SHA-256 is internal,
so no OpenSSL runtime leaked into the executable.

Targeted CLI integration passed on 2026-08-08: warning-clean C++17/C++20 syntax
compilation,
`render9` build/self-test, CLI CTest 8/8, two-layer composite PNG export, first
ZIP and unpacked directory saves, bundle reload/version listing, clean-save full
validation with no added version, and legacy `.pvt` import to a separate default
ZIP without overwriting the source. A root-integrity-mismatch fixture recording
program version `99.0.0` loaded successfully and produced the required nonblocking
newer-version warning; explicit Save preserved it as version 1 with neutral
`external_change` provenance.

Focused bundle CTest 1/1 passed on 2026-08-08, covering SHA-256 vectors,
hostile ZIP/tree guards, directory and Unicode ZIP round trips, stable-root
renames, copied/renamed Save As, append/clean-save/diff/current/revert, legacy
promotion, repeated external-change promotion, missing-current orphan recovery,
stale-writer refusal, commit-time directory/ZIP compare-and-swap, advisory-lock
contention/symlink rejection, complete numeric-history accounting, preserved
valid/malformed orphans, deleted/corrupt ancestor lineage, checksum repair,
timestamp/C1 validation, newer-program detection, and transactional invalid-root
rejection.

Previous baseline passed on 2026-08-05:

- Release C++17 library/CLI build, CTest 10/10, and `render9 --self-test`.
- Release C++20 compatibility build and CTest 10/10.
- Qt-enabled Release build with an explicit Qt prefix, CTest 11/11, offscreen
  smoke launch, setup round-trip, precision, UTF-8 validator, automatic alpha,
  randomization invariants, and slow-preview playback-race verification.
- Release library-only build with CLI/GUI/examples/tests disabled, installation,
  symbol check showing no `main`, and an external `find_package` consumer.
- AddressSanitizer plus UndefinedBehaviorSanitizer build and CTest 10/10.
- Independent FFmpeg decoding of 8/16-bit PNG and FLOAT EXR, plus Blender EXR
  loading; RGB/RGBA channel layouts and varying alpha were confirmed.
- Setup round-trip, malformed-input transactionality, permission preservation,
  Unicode output paths, embedded-NUL rejection, sequence no-clobber behavior,
  progress cancellation/exception handling, and `git diff --check`.
- Workspace-root Makefile delegation and unsafe `BUILD_DIR` clean rejection.
- Static and shared installs containing the CLI, header, CMake package metadata,
  license, and documentation; both installed-package consumers and the installed
  CLI passed. The shared CLI used its relative sibling-library runtime path.
- Warning-enabled client builds and Apple Clang static analysis found no actionable
  project-code issue in the CLI/GUI (one Qt queued-callable ownership false positive).

The validation host's libpng targets a newer macOS version than the Qt kit, so
GUI builds print a deployment-target linker warning there. A redistributable
macOS package should bundle or link a libpng built for its intended deployment
target.

## Remaining work for later passes

1. **Metal backend:** no GPU renderer is implemented. Introduce a backend-neutral
   frame-render interface first; retain the CPU implementation as the reference
   and fallback, then add macOS Metal resource/pipeline caches and backend-aware
   scheduling. Bound GPU allocations and in-flight frames, preserve cooperative
   cancellation, and test CPU/Metal seam, alpha, surface, effect, palette, and
   transform parity. Do not scatter one-off Metal paths through individual
   effects or run CPU and GPU schedulers independently.
2. **Layer starting image:** reuse the implemented bounded readable attachment
   attachment store; do not add another asset container or naked local path.
   Add only the layer source-mode schema, transactional image decoder/cache,
   renderer integration, and GUI. Reference a stable attachment ID/digest and
   share immutable decoded storage across preview, frames, layers, and compact
   undo snapshots. Treat it as an alternative to procedural generation and its
   starting palette, then apply existing periodic effects/surfaces/transforms.
3. **Reusable closed cubic paths:** define named project-level path geometry
   separately from per-consumer bindings. Require at least three nodes, close
   the final cubic segment to the first, give nodes stable IDs, and store Corner,
   Auto Smooth, Smooth, or Symmetric handle modes with explicit in/out handles.
   Because three arbitrary smooth nodes are not an exact ellipse, provide an
   ellipse tool that creates a four-node cubic approximation instead of silently
   promising circle behavior the geometry cannot guarantee.
   Bindings for waves, effect centers, and mapped objects should independently
   own enable, synchronized/free clock, integer cycles, phase, direction, and
   offset, plus optional follow-tangent orientation. Build a bounded arc-length
   lookup table after edits for visually uniform sampling. Add a dedicated GUI
   node/handle editor, strict setup/bundle validation, undo, and semantic diffs.
4. **Projected source-overlay editing:** add an explicit unwrapped source/UV
   preview or inset for Texture effects and localized Swings when a non-plane
   surface is active. Their current dotted overlays are deliberately labelled
   as unprojected; arbitrary OBJ UVs can be discontinuous or one-to-many, so a
   screen-space circle must not pretend to be the projected footprint.
5. If encoder-level cancellation becomes important, add format-specific abort
   plumbing around libpng/zlib and EXR output. Rendering itself is cancellable
   within expensive frame/effect/OBJ passes; an encoder currently finishes the
   one atomic output file it has already started.
6. For distributable GUI bundles, deploy the matching Qt runtime. On macOS also
   resolve the deployment-target mismatch by packaging an appropriately built
   libpng and exercise the oldest supported macOS version. Until that deployment
   workflow exists, the GUI intentionally remains a build-tree application and is
   not installed by `cmake --install`.
7. Expand GUI automation beyond the bounded smoke paths to exhaustively drive
   every layer editor, undo merge boundary, long semantic diff, bit-depth
   transition, progress path, and cancellation race.
