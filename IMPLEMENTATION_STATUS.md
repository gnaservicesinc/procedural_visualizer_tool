# Procedural Visualizer implementation ledger

Last updated: 2026-08-08

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
stays stable for that associated bundle. Project display names are snapshotted
per version, so renaming never moves the bundle and old-name diff/revert remains
meaningful; root metadata reflects whichever version is current.

The ZIP/directory boundary is hostile-input hardened: traversal, absolute or
platform-special paths, malformed UTF-8/NUL, collisions, symlinks/special files,
encryption/multidisk/unsupported compression, expansion ratios, counts, sizes,
unexpected records, checksums, UUIDs, enums, and values are bounded and checked.
New directory snapshots and ZIP replacements use sibling staging/atomic rename;
stale or divergent destinations are refused.

The Qt GUI separates Layer Render from global Output, adds a topmost-first Layers
dock, project name/title, blend/opacity/enable/rename/duplicate/reorder controls,
session Solo, version list/diff/current/revert tools, and application-wide
undo/redo. Undo depth, window state, and dialog locations use per-user platform
settings, while a hard 128 MiB history budget bounds full-state snapshots.
Preview/export cancellation now reaches per-frame effects and OBJ rasterization;
stale previews are cancelled and document replacement is blocked during export.
The CLI opens ZIPs, directories, and legacy setups; renders composite
projects; edits the selected layer; manages bounded layers; separates final alpha
from modulation; performs normal bundle saves; and reserves `--save-legacy` for
an explicit one-layer export.

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
| 10 | Qt GUI using the library | Complete | Qt Widgets client with live async preview, draggable wave handles, dynamic stack editors, all configuration fields, timeline, project bundle/legacy import I/O, and background export. |
| 11 | More quantization/swing levels and variations | Complete | 2-65,536 levels, RGB/luminance/hue modes and mix; dynamic sine/triangle/smooth-pulse/bounce swing stacks. |
| 12 | Plane/cube/sphere/cylinder/custom OBJ wrapping | Complete | Analytic built-ins plus bounded cached OBJ parsing and perspective rasterization; authored UV/normal data has safe fallbacks. All mappings are two-sided and transparent closed surfaces composite entry/exit samples. |
| 13 | Configurable PNG compression | Complete | Levels 0-9 are available in the API, setup v3, CLI, and GUI; level 5 is the balanced default and EXR ignores it. |
| 14 | Randomize stack values or composition | Complete | Separate GUI actions preserve existing identity/type structure or create a new bounded mix of waves, swing waveforms, effect types, and enabled items. |
| 15 | Stable paths, dialogs, and playback | Complete | Relative paths are anchored to a stable launch directory, first file dialogs use home then remember their last location, and completed previews advance during Play even under timer/render overlap. |
| 16 | Named projects and default filenames | Complete | Semantic name appears in the title; a portable sanitizer supplies the initial root/default `.zip`. The associated root stays stable while renamed display values are versioned. |
| 17 | Full render-config layers | Complete | Global canvas/export data is split from per-layer render data; layer switches cannot overwrite global output state. Stable UUIDs/file IDs survive names and reorders. |
| 18 | Layer compositing | Complete | Sequential bounded float-RGBA compositing implements all 11 requested modes plus opacity; only one layer frame and one accumulator are retained. |
| 19 | Alpha split | Complete | Per-layer procedural modulation and global final RGB/RGBA selection are independent. Multi-layer creation enables final alpha without changing artwork; validation follows actual final-composite transparency. |
| 20 | Human-readable project bundles | Complete | ZIP/directory bundle tree stores root/version metadata, per-version global output, per-layer `.pvt` data, SHA-256 indexes, and a portable text current pointer. |
| 21 | Automatic immutable save versions | Complete | Changed Save appends; clean Save validates; load fallback, external-change promotion, semantic diff, Make Current, revert-as-new, and advisory newer-program warnings preserve recoverability. |
| 22 | Legacy compatibility without overwrite | Complete | Setup v1/v2 import into a new unsaved one-layer project; setup v3 retains final-alpha state. Only explicit one-layer `--save-legacy` writes `.pvt`. |
| 23 | GUI session undo/redo and preferences | Complete | All editor/structural actions use undo/redo; a configurable step limit plus hard 128 MiB history budget and UI state live in per-user `QSettings`, outside bundles. |
| 24 | Hostile-input bundle handling | Complete | Strict tree/archive/metadata bounds and checks reject traversal, collisions, links/special files, unsupported/encrypted archives, expansion abuse, stale saves, and invalid typed data transactionally. |

## Important implementation map

- `include/procedural_visualizer_tool.h`: public C++ API and complete owned config model.
- `src/core.cpp`: defaults, periodic phases, float RGBA layer renderer, effects,
  quantization, alpha, and analytic surface mappings.
- `src/composite.cpp`: project/layer validation, UUIDs, linear-light blend modes,
  bounded sequential compositing, and project frame rendering.
- `src/obj_mesh.cpp` / `src/obj_surface.cpp`: bounded Wavefront parsing/cache and
  perspective, two-sided, layered custom-mesh rasterization.
- `src/image_io.cpp`: PNG/EXR encoding, PNG compression, dithering, collision preflight,
  atomic installation, progress, and cancellation checks.
- `src/config_io.cpp` / `src/config_codec.cpp`: setup v1-v3 codec and split
  per-layer/global bundle records with transactional legacy file I/O.
- `src/project_bundle.cpp` / `src/bundle_archive.cpp`: checksummed project/version
  metadata, semantic history operations, bounded ZIP/directory loading, and
  atomic save staging. This helper is intentionally not installed ABI.
- `app/cli_main.cpp`: project/layer-aware menu and command-line client.
- `gui/`: Qt 6 project/layer/version client (Qt 6.5 or newer).
- `tests/test_main.cpp`, `tests/project_composite_test.cpp`, and
  `tests/bundle_test.cpp`: core/seam/format/setup/I/O, layer/blend/project, and
  persistence/archive-safety coverage.

## Guardrails retained

- Frames sample `[0, N)` and omit the duplicated endpoint.
- All animation rates are integer cycles per loop; synchronized and free clocks
  both close exactly.
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
- Custom OBJ paths are configuration references, not embedded bundle assets.
- GUI undo snapshots have a separate 128 MiB hard budget. Oversized or trimmed
  history never clears the document's dirty state, and no-op normalized edits
  do not create commands.

## Validation record

Fresh integrated Release builds passed all 13/13 non-GUI CTests and all 14/14
Qt-enabled CTests on 2026-08-08, including the project composite, bundle, CLI
bundle, GUI smoke, and retained core/OBJ/CLI paths. The workspace-root
`make PVT_BUILD_QT_GUI=ON all` path also rebuilt and linked the macOS GUI
successfully; an offscreen smoke run produced and verified the Layers/Versions
window.

A C++20 Release build passed 13/13 CTests after exercising the standard's
`char8_t` filesystem path behavior. A Debug AddressSanitizer plus
UndefinedBehaviorSanitizer build passed the same 13/13 tests (LeakSanitizer is
not supported by this macOS runtime). A core-only configure built without
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

1. If encoder-level cancellation becomes important, add format-specific abort
   plumbing around libpng/zlib and EXR output. Rendering itself is cancellable
   within expensive frame/effect/OBJ passes; an encoder currently finishes the
   one atomic output file it has already started.
2. For distributable GUI bundles, deploy the matching Qt runtime. On macOS also
   resolve the deployment-target mismatch by packaging an appropriately built
   libpng and exercise the oldest supported macOS version. Until that deployment
   workflow exists, the GUI intentionally remains a build-tree application and is
   not installed by `cmake --install`.
3. Expand GUI automation beyond the bounded smoke paths to exhaustively drive
   every layer editor, undo merge boundary, long semantic diff, bit-depth
   transition, progress path, and cancellation race.
4. If self-contained custom-mesh projects become a priority, add an explicit,
   checksummed asset-import feature with deduplication and strict size/type bounds.
   Current bundles intentionally retain OBJ paths only and never copy or fetch
   mesh/material/texture assets implicitly.
