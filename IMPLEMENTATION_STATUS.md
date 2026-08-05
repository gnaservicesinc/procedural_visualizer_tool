# Procedural Visualizer implementation ledger

Last updated: 2026-08-05

This is the hand-off point for humans and future coding agents. This repository
is the canonical working tree. Any loose C files retained outside it are legacy
snapshots, not inputs to the current build.

## Outcome of this pass

The fixed monolithic C renderer has been replaced in the Git project by a
reusable C++17 float-RGBA library, an interactive/scriptable CLI, and an optional
Qt 6 desktop client. The old `render9.c` and vendored `stb_image_write.h` were
removed from this tree and remain recoverable from Git history.

The source migration is indivisible: `CMakeLists.txt`, `app/`, `include/`,
`src/`, and the supporting test/package files must accompany those deletions.
A partial change containing only edits to previously tracked files will not
build.

The follow-up audit aligned every CLI/GUI numeric editor with central validation,
exposed the previously omitted type-specific CLI effect controls, added CLI swing
reordering, and made unknown/self-test option handling unambiguous. GUI preview
and export work now handles allocation/worker exceptions, suppresses stale preview
errors, coalesces progress updates, reports cancellation separately, keeps playback
timing/labels current, and remains visible while a closing export reaches its next
cancellation boundary. The GUI smoke path now round-trips setup state.

The core audit also corrected channel-biased surface lighting, made primitive
curvature a continuous planar-to-mapped transition, preserved exact neutral
effect/surface behavior, hardened public image indexing and render transactionality,
and made peak-memory checks reflect only active work. Setup and image writes now
use checked sibling temporaries, preserve regular-file modes on overwrite, replace
symlink entries without following them, and retain atomic no-clobber behavior under
concurrent renders. POSIX setup output uses direct partial-write/EINTR handling,
followed by permission restoration, `fsync`, and atomic replacement.

This pass added selectable PNG compression (`0` off through `9` maximum, default
`5`), stable relative-path handling, home/last-location file dialogs, and a
playback pipeline that presents completed frames continuously even when rendering
is slower than the playback timer. The GUI now has separate randomizers for
existing stack values and for wave/swing/effect composition. Glow defaults were
tuned to produce a visible restrained bloom, and the new Block Scale effect
animates smooth or quantized pixel grouping in the ordered effect stack.

Surface wrapping now includes bounded custom Wavefront OBJ loading, cached mesh
parsing, perspective-correct CPU rasterization, authored or automatic UVs, and
two-sided lighting. Built-in closed primitives and OBJ meshes composite rear/exit
surface samples through partially transparent front surfaces instead of using an
opaque nearest-hit mask.

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
| 10 | Qt GUI using the library | Complete | Qt Widgets client with live async preview, draggable wave handles, dynamic stack editors, all configuration fields, timeline, setup I/O, and background export. |
| 11 | More quantization/swing levels and variations | Complete | 2-65,536 levels, RGB/luminance/hue modes and mix; dynamic sine/triangle/smooth-pulse/bounce swing stacks. |
| 12 | Plane/cube/sphere/cylinder/custom OBJ wrapping | Complete | Analytic built-ins plus bounded cached OBJ parsing and perspective rasterization; authored UV/normal data has safe fallbacks. All mappings are two-sided and transparent closed surfaces composite entry/exit samples. |
| 13 | Configurable PNG compression | Complete | Levels 0-9 are available in the API, setup v2, CLI, and GUI; level 5 is the balanced default and EXR ignores it. |
| 14 | Randomize stack values or composition | Complete | Separate GUI actions preserve existing identity/type structure or create a new bounded mix of waves, swing waveforms, effect types, and enabled items. |
| 15 | Stable paths, dialogs, and playback | Complete | Relative paths are anchored to a stable launch directory, first file dialogs use home then remember their last location, and completed previews advance during Play even under timer/render overlap. |

## Important implementation map

- `include/procedural_visualizer_tool.h`: public C++ API and complete owned config model.
- `src/core.cpp`: validation, periodic phases, float RGBA renderer, effects,
  quantization, alpha, and analytic surface mappings.
- `src/obj_mesh.cpp` / `src/obj_surface.cpp`: bounded Wavefront parsing/cache and
  perspective, two-sided, layered custom-mesh rasterization.
- `src/image_io.cpp`: PNG/EXR encoding, PNG compression, dithering, collision preflight,
  atomic installation, progress, and cancellation checks.
- `src/config_io.cpp`: `.pvt` serializer/parser and transactional file I/O.
- `app/cli_main.cpp`: menu and command-line client.
- `gui/`: Qt 6 client (Qt 6.5 or newer).
- `tests/test_main.cpp`: core, seam, alpha, format, setup, safety, and I/O tests.

## Guardrails retained

- Frames sample `[0, N)` and omit the duplicated endpoint.
- All animation rates are integer cycles per loop; synchronized and free clocks
  both close exactly.
- IDs are stable, nonzero, and unique after insertion; factory objects start at
  ID zero and must receive `allocate_id(config)`.
- Collection counts, values, decoded strings, file sizes, image dimensions, and
  an estimated 1 GiB peak working set are bounded before large allocations;
  custom OBJ estimates conservatively include the maximum cached mesh and its
  projected position/normal arrays.
- Existing output frames are protected unless overwrite is explicit, including
  an all-frame preflight and atomic no-clobber installation.
- Transparent effect edges and curved 3D-surface exteriors require RGBA export;
  validation rejects configurations that would silently discard their alpha.
- Transparent closed surfaces composite distinct front and rear samples. OBJ
  rasterization never culls by winding and peels at most eight distinct depths.
- The core library has no Qt dependency.

## Validation record

Passed on 2026-08-05:

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

1. Thread cancellation into per-frame rendering and encoding. Cancellation is
   currently checked during preflight and between frames, so one exceptionally
   expensive frame cannot be interrupted midway.
2. For distributable GUI bundles, deploy the matching Qt runtime. On macOS also
   resolve the deployment-target mismatch by packaging an appropriately built
   libpng and exercise the oldest supported macOS version. Until that deployment
   workflow exists, the GUI intentionally remains a build-tree application and is
   not installed by `cmake --install`.
3. Expand GUI automation beyond launch/setup/playback/randomization checks to
   exercise editing/reordering, bit-depth transitions, progress, and cancellation.
