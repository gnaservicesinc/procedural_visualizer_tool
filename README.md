# Procedural Visualizer Tool

A seamless-loop procedural image renderer with a reusable C++ library, command-line
editor, and optional Qt 6 desktop GUI. A named project can contain a stack of
independently configurable fire layers; each frame is rendered and blended in
linear-light 32-bit floating-point RGBA, then exported as 8/16-bit PNG or full
32-bit FLOAT EXR.

## Requirements

- CMake 3.20 or newer and a C++17 compiler.
- libpng development files for the library, CLI, and tests.
- Bundle support uses an internal portable SHA-256 implementation. CMake uses a
  compatible installed minizip-ng when available and otherwise fetches its
  pinned, minimal zlib-only configuration.
- Qt 6.5 or newer with Widgets and Concurrent components for the optional GUI.

## Quick start

Build the library and CLI, then open the interactive menu:

```sh
make
make run
```

Render the defaults without opening the menu:

```sh
make render
```

Build and open the Qt GUI. CMake discovers Qt from the standard search paths:

```sh
make gui
```

For a Qt installation outside those paths, pass
`QT_PREFIX=/path/to/Qt/6.x/<kit>`. The launch target handles both the macOS app
bundle and native non-macOS executable names.

## What is configurable

- A semantic UTF-8 project name used in the window title. Display-safe characters
  such as `:` are allowed; the first Save/Save As derives a portable sanitized
  root and `<project-name>.zip` filename without changing the displayed name.
- Up to 64 full render layers. Layers have stable UUID/file identities and can
  be named, enabled, duplicated, removed, selected, and reordered. Paint order
  is bottom-to-top; the GUI presents the topmost layer first.
- Per-layer opacity and Normal (`none`), Soft Light, Grain Merge, Overlay,
  Color Dodge, Linear Burn, Color Burn, Difference, Subtract, Multiply, and Add
  blend modes. Normal is ordinary Porter-Duff source-over compositing.
- Any number of waves from zero through the validated safety limit, with add,
  duplicate, remove, enable, and reorder controls.
- Per-wave synchronization, placement, amplitude, spatial frequency, phase,
  cycles per loop, and propagation direction.
- An ordered dynamic effect stack with endless zoom, ripple, shake, flag wave,
  glow, and animated block scaling. Every effect can be enabled, synchronized,
  duplicated, removed, and reordered. Each effect explicitly runs either in
  **Texture** space before surface wrapping or on the **Mapped object** after
  wrapping and the layer mirror/flip; the latter moves or deforms the rendered
  silhouette of a cylinder, sphere, cube, or OBJ in final canvas coordinates
  rather than editing its source 3D geometry. Controls
  include type-specific centers, edges, frequencies, harmonics/attenuation,
  glow bloom parameters, and block scale
  range/mix/quantization steps.
- Draggable numbered effect centers in the preview. A local area radius of zero
  preserves whole-layer behavior; a positive radius creates a smoothly
  feathered circle around the center for zoom, ripple, shake, flag wave, and
  glow. Glow's blur radius remains a separate control. Texture-effect and Swing
  overlays are explicitly marked as unprojected source/UV coordinates; mapped-
  object overlays are final screen coordinates.
- Transparent, black, white, or reflected out-of-frame handling for coordinate
  effects.
- Multiple dynamic swing modulators with sine, triangle, smooth-pulse, and
  bounce variations, including add, remove, edit, and reorder controls. A swing
  radius of zero modulates the whole layer; a positive radius localizes its
  clock influence to a feathered circle whose numbered center handle is
  draggable in the preview. Localized Swing timing drives source waves and
  Texture effects; Mapped-object effects use the global synchronized clock
  because an arbitrary projected screen point does not map to one unique UV.
- An optional starting palette per layer with 1-256 authored sRGB colors, custom
  add/edit/remove controls, and six presets: Ember, Deep Ocean, Vaporwave,
  Forest Biolume, Arcade, and Moonlight. The palette chooses exact procedural
  source colors in linear light without changing alpha; lighting and effects
  may create other colors afterward. Presets never silently change
  whether the starting palette is enabled.
- Per-layer horizontal/vertical flips and directional mirror symmetry
  (left-to-right, right-to-left, top-to-bottom, bottom-to-top, or four-way).
- Independent feature toggles for displacement, slope lighting, spiral, and
  wall reflection.
- Post-effects RGB, luminance, or hue quantization with 2-65,536 levels and
  adjustable mix. This is the explicit final color-reduction control and is
  independent of the starting palette.
- Plane, cylinder, sphere, ray-cast cube, and custom Wavefront OBJ mappings.
  OBJ files may provide texture coordinates and normals; automatic box UVs and
  geometric normals cover meshes that omit them.
- Independent procedural alpha modulation with minimum/maximum alpha, spatial
  frequency, phase, and cycles per loop.
- 8/16-bit RGB or RGBA PNG and 32-bit FLOAT RGB or RGBA EXR output.
- PNG compression from 0 (off/fastest) through 9 (maximum), with a balanced
  default of 5. EXR output is unaffected.
- Optional deterministic blue-noise-like, ordered Bayer, or Floyd-Steinberg
  dithering for integer PNG output. Dithering is never applied to float EXR.

The GUI includes a topmost-first Layers dock, project naming, per-layer blend and
opacity controls, a session-only **Solo** preview, draggable center handles for
waves, swings, and centered effects with visible radius rings, ordered
wave/swing/effect editors, palette and transform controls, type-aware effect
controls, a live
checkerboard alpha preview, a continuously updating loop timeline, and
background composite export with cooperative cancellation inside expensive
frame/effect/OBJ passes. The Output tab is
global while the Layer Render tab always edits the selected layer. **Randomize
values** keeps the current layer's stack structure and types while varying its
settings; **Randomize mix** creates a new bounded mix. File dialogs remember
their last usable folder and otherwise begin in the home folder.

Every GUI field edit and structural move participates in session undo/redo. The
step limit, window layout, and dialog locations are stored with the platform's
normal per-user settings service (`QSettings`); these preferences are not placed
inside a portable project. A separate hard 128 MiB snapshot budget prevents a
large, high-layer document from turning a generous step limit into unbounded
memory growth; if history must be trimmed, the document remains correctly dirty.
Saved-version history is separate from session undo.

## Synchronization and seamless loops

The renderer samples `N` frames over the half-open interval `[0, 1)`. It does not
write the duplicated endpoint, so the last frame advances to frame zero by one
normal frame step.

All animation rates are whole cycles per loop:

- **Synchronized (optional) on:** the item uses the shared master phase after
  swing and phrase modulation.
- **Synchronized off:** the item uses its own linear periodic clock, cycle count,
  and phase.

Both choices remain seamless. Unsynchronized means independently timed, not
wall-clock-driven or random.

Wave propagation direction is continuous:

- `0.0`: horizontal propagation
- `0.5`: radial/all-directions behavior (the default)
- `1.0`: vertical propagation

Intermediate values blend between radial and the selected axis.

## Parallel sequence export

Sequence exports use a bounded CPU worker pool. Each worker renders and encodes
an independent frame; completed files are still installed atomically in
ascending frame order, and progress callbacks run serially on the calling
thread. This keeps collision protection, deterministic filenames, cancellation,
and callback behavior compatible with the former sequential exporter.

The GUI and the default library overload select workers automatically. The CLI
can choose the upper bound explicitly:

```sh
# Hardware-concurrency auto selection (also the default)
./build/render9 --render --workers 0

# Reproducible sequential reference, or an explicit bounded pool
./build/render9 --render --workers 1
./build/render9 --render --workers 12
```

The requested value is capped by the frame count, the reported hardware
concurrency when automatic, a hard 256-worker limit, and a conservative
aggregate memory budget derived from the validated per-frame peak estimate. A
request is therefore an upper bound, not permission to exhaust RAM.

On one representative Apple M2 Max workload (12 CPU cores, 24 frames at
960x540, block size 1, 64 waves, PNG compression 0), `--workers 1` took 44.95
seconds and `--workers 12` took 5.44 seconds: an 8.3x wall-clock speedup. All 24
PNGs were byte-identical. This is a measured example, not a universal guarantee;
scaling depends on image size, layer/effect cost, encoder settings, storage, and
the memory-derived worker cap.

## Alpha and color precision

Internal images always contain four 32-bit floating-point channels. RGB is
linear-light and straight/unassociated alpha is retained independently, including
meaningful RGB values at alpha zero for compositors such as Blender.

Alpha has two deliberately separate controls:

- Each layer's **procedural alpha modulation** is artistic render data. Its
  minimum, maximum, frequency, phase, and loop cycles travel with that layer.
- **Write final alpha channel** is project-global output data. It selects RGB or
  RGBA without changing the artwork.

Adding a second layer enables final RGBA output automatically, as do features
that generate geometric/effect transparency. This does not silently enable
procedural modulation. Export validation rejects RGB combinations that would
drop real final-composite transparency; RGB remains valid when an opaque lower
stack guarantees an opaque result. Procedural modulation is neutral at its defaults
(`minimum == maximum == 1.0`); lower either bound to make it visible.

Built-in closed primitives and custom meshes are two-sided. With partial alpha,
the renderer samples the rear/exit surface and composites it behind the front;
it does not treat a translucent front as an opaque nearest-hit mask. Custom OBJ
rendering depth-peels up to eight distinct layers per pixel, stops early when no
deeper surface remains, and uses a faster nearest-surface path for fully opaque
input. Triangle winding never causes backface culling.

PNG export converts linear RGB to sRGB immediately before dithering and integer
quantization. PNG bit depth is per channel: 8-bit produces standard RGB/RGBA PNG;
16-bit produces RGB/RGBA PNG with 16-bit samples. EXR stores the original linear
RGB(A) values in FLOAT channels, including values above `1.0` created by glow.

Glow thresholds are measured in linear luminance. The default threshold is low
enough to create a visible, restrained bloom when Glow is enabled; raise it to
restrict the halo to only the brightest regions.

Block Scale groups the image into animated pixel blocks at its exact position in
the ordered effect stack. Its minimum and maximum multipliers are relative to the
canvas block size. The multiplier eases from minimum to maximum and back over a
seamless cycle; zero quantization steps is smooth, while a positive whole step
count produces deliberately stepped size changes.

## Project bundles and automatic versions

Normal Save always writes a project bundle, even for one layer. The default name
is the portable project name plus `.zip`:

```sh
./build/render9 --project-name "Midnight Bonfire" --save-default
./build/render9 --load "Midnight Bonfire.zip" --render
```

ZIP bundles and unpacked bundle directories contain the same human-readable
tree. The first Save/Save As fixes the sanitized archive/directory root. When a
project that has already been saved is renamed in the GUI, it offers four
explicit choices:

- **Keep Existing Filename** changes the semantic project name, marks the
  current document dirty, and records that rename as a normal new version on
  the next Save. The associated bundle path/root does not move.
- **Save As and Open** creates a new independent bundle using the sanitized new
  name by default, then switches the GUI to it.
- **Save Copy, Stay Here** creates the same independent bundle but leaves the
  original project open and restores its previous displayed name.
- **Cancel** changes neither project.

An independent rename copy contains only the current working state as version
0. It receives a new project UUID, new UUIDs for every layer, and fresh
bundle-local file identities; it inherits no source path, history, or stale-save
token. The original bundle is never rewritten, and the new-copy operation
refuses every existing destination. This is intentionally different from
adopting an exact filesystem copy of a complete bundle, which retains its
embedded identity and history. CLI project-name edits remain ordinary semantic
renames; they do not open this GUI choice dialog or implicitly fork a project.
A two-layer first save looks like this:

```text
Midnight Bonfire/
  metadata.txt
  metadata.sha256
  current
  0/
    metadata.txt
    render_output.txt
    0.pvt
    1.pvt
```

`render_output.txt` holds global canvas/loop and export settings. Each numbered
`.pvt` stores only one layer's render data. Version metadata stores the project
display name, program/time, layer UUIDs, stable file IDs, order, names, enabled
states, blend modes, opacity, and SHA-256 digests. Root metadata reflects the
current version's project name and also stores the project UUID,
creation/open/save timestamps, creating/changing program versions, and each
version-metadata digest; its digest is necessarily kept in the separate
`metadata.sha256` sidecar.

`current` is a small checksummed text pointer, not a filesystem symlink. This is
portable across ZIP extractors and avoids archive symlink hazards. A changed Save
appends the next numeric directory, then replaces root metadata/current through
checked atomic file operations (a ZIP replaces the whole outer archive);
old snapshots are immutable and gaps are valid. A no-change Save validates the
entire bundle and creates no version. **Make Current** changes root bookkeeping
and the pointer but never alters a numbered snapshot;
**Revert as New** copies the selected snapshot into a new highest-numbered
version, so even a rollback can itself be rolled back. Semantic diffs follow
layer UUIDs instead of confusing renames/reorders with unrelated objects.

Every numeric directory is accounted for during full validation. Parseable
orphans can be promoted, while unrelated malformed/external trees are retained
byte-for-byte in an explicitly checksummed preserved-history table; lineage
aliases keep valid descendants connected even if an ancestor was edited or
deleted outside the application. Saves are serialized with a hidden sibling
advisory lock (`.<bundle>.pvt-save.lock`) and compare the complete expected
on-disk digest while holding that lock, so cooperating processes cannot erase a
newer save. The lock file contains no project data and may safely remain beside
the bundle between sessions.

Load is read-only and transactional. It tries the valid `current` snapshot first,
then numeric directories from highest to lowest until one validates. Missing or
broken pointers therefore do not destroy recoverable work. A checksum mismatch
is reported neutrally as an external change/integrity mismatch, not proof of who
or what changed it: parseable, centrally valid data opens dirty and is promoted
to a first-class new version on the next explicit Save; invalid snapshots are
skipped. Opening alone never rewrites the bundle.

If bundle metadata records a creating or last-changing program version newer
than the running application, the project still loads when otherwise valid, but
the CLI and GUI warn before the user saves with older code. This is advisory rather
than a data-hostile version gate.

Archive and directory input is treated as hostile. Readers bound entry count,
per-file/expanded size, path length, compression ratio, metadata records, layers,
and versions; reject traversal, absolute/drive/UNC paths, NUL or malformed UTF-8,
case-colliding duplicates, encrypted/multidisk archives, symlinks and special
files, unsupported compression, unexpected tree entries, CRC failures, and
unparseable or invalid typed values. ZIP replacement and new directory-version commits use
checked sibling staging and atomic rename operations. Save also refuses a stale
or divergent destination rather than silently overwriting another history. An
exact copied/renamed bundle with the same UUID and observed state can be adopted
by Save As; a different UUID or advanced/divergent state is rejected.

Legacy deterministic line-oriented `.pvt` setup versions 1-3 remain importable;
current explicit legacy output is setup format 4. Format 4 adds effect stage and
local-area data, localized swings, starting palettes, and layer transforms while older
files receive neutral compatibility defaults. Import creates a new unsaved
one-layer project with a new project/layer UUID and clears its save association,
so normal Save can never overwrite the source `.pvt`. New saves remain bundles.
The CLI exposes
`--save-legacy FILE.pvt` only as a clearly lossy escape hatch and rejects it when
more than one layer exists.

Version 4.0.1 corrects the 4.0.0 palette-stage bug without changing the setup,
layer, or bundle schema: an enabled v4 palette now selects starting colors
instead of rewriting the final effected image. Existing affected projects will
therefore render with the corrected appearance by design.

## Scripted rendering

Common CLI overrides can be layered on defaults or on a loaded project:

```sh
./build/render9 --load "Midnight Bonfire.zip" --render \
  --width 640 --height 360 --block-size 4 \
  --frames 120 --fps 30 --waves 10 --workers 0 \
  --alpha --bit-depth 16 --png-compression 5 --dither blue \
  --output-dir preview --prefix ripple_
```

Layer selectors and modifiers are also processed left-to-right. `--layer 1`
selects the bottom layer; `--add-layer NAME` adds and selects a new top layer:

```sh
./build/render9 --load "Midnight Bonfire.zip" \
  --add-layer "Hot sparks" --blend add --layer-opacity 0.42 \
  --waves 5 --alpha-modulation --save "Midnight Bonfire.zip"
```

To wrap the generated image around a mesh from the command line:

```sh
./build/render9 --render --obj meshes/model.obj \
  --frames 120 --png-compression 5 --output-dir preview
```

`--obj` enables final RGBA output for the mapped exterior but does not alter the
active layer's procedural alpha modulation.

Run `./build/render9 --help` for all options. Existing matching output files are
protected unless `--overwrite` is explicit. A full sequence collision preflight
runs before frame zero, and each frame is installed atomically. Overwriting a
regular file preserves its explicit permission mode; overwriting a symlink replaces
the link entry rather than modifying its target.

Relative output directories are resolved against the process working directory.
On macOS, `make gui` runs the app-bundle executable directly so it inherits the
make working directory. If a desktop launcher supplies `/` (the source of the
old `.` export failure), the GUI rejects that unusable launch directory, anchors
relative paths in the user's home folder, and never treats `.` as filesystem root.

## Custom OBJ surfaces

The bounded OBJ loader accepts ASCII/UTF-8 `v`, `vt`, `vn`, and `f` records,
including positive or negative indices and the standard `v`, `v/vt`, `v//vn`,
and `v/vt/vn` face-corner forms. Simple polygon faces are validated and
triangulated while preserving winding and per-corner attributes. Object/group,
smoothing, and material metadata is ignored: the procedural frame is the sole
surface image, and no `.mtl` or sibling file is opened.

Meshes are uniformly normalized from their referenced bounds and rendered with
perspective-correct texture/normal interpolation. A triangle uses its authored
texture coordinates only when all three corners provide them; otherwise it uses
dominant-axis box projection. Missing normals fall back to the geometric face
normal. The loader limits file, line, collection, polygon, triangle, and expanded
mesh sizes; malformed loads are transactional. The last successfully loaded
mesh is cached across preview/export frames and reloaded when its path, size, or
modification time changes.

Relative OBJ paths use the same stable process working directory as relative
output paths. In the GUI, **Browse…** stores the selected absolute path and also
updates the remembered file-dialog folder.

Project bundles intentionally store the configured OBJ path, not a copy of the
mesh. A bundle using a custom OBJ is therefore not self-contained: move the mesh
with the project and repair the path, or use a stable shared/absolute location.
The loader never follows a bundle entry to fetch a mesh, material, texture, or
network resource implicitly.

Options are processed from left to right, so put `--load` before overrides that
should replace project values. `--alpha`/`--no-alpha` select final RGBA/RGB;
`--alpha-modulation`/`--no-alpha-modulation` control only the active layer's
artwork. `--defaults` remains a compatibility alias for `--render`; used by
itself, it renders the built-in defaults. Float EXR output always disables
dithering.

## Library build and API

The core has no Qt dependency. Build only `libProceduralVisualizerTool`, without
any executable containing `main`:

```sh
cmake -S . -B build-library -G Ninja \
  -DPVT_BUILD_CLI=OFF \
  -DPVT_BUILD_QT_GUI=OFF \
  -DPVT_BUILD_EXAMPLES=OFF \
  -DBUILD_TESTING=OFF \
  -DBUILD_SHARED_LIBS=ON
cmake --build build-library --parallel
```

The public header is `include/procedural_visualizer_tool.h`. It exposes:

- fully owned legacy render and project/layer configuration values;
- default factories, stable ID allocation, and validation;
- float RGBA layer/project rendering by frame index or normalized phase;
- bounded linear-light blend compositing, individual PNG/EXR writing, and
  composite sequence export;
- a bounded `SequenceRenderOptions` CPU worker policy, ordered atomic output,
  and serialized progress/cancellation callbacks; and
- backward-compatible transactional `.pvt` setup save/load.

Bundle/version persistence is an internal application helper, not installed
library ABI. As a result, a library-only build does not search for or fetch
minizip-ng, and installed static-library consumers do not inherit that
application dependency.

Build `examples/library_example.cpp` with:

```sh
cmake -S . -B build-example -DPVT_BUILD_EXAMPLES=ON
cmake --build build-example --parallel
./build-example/pvt_library_example
```

Install the library, public header, CMake package metadata, CLI, license, and
documentation with:

```sh
cmake --install build --prefix /desired/prefix
# Or after a Makefile build:
make install INSTALL_PREFIX=/desired/prefix
```

The installed CMake package target is
`ProceduralVisualizerTool::ProceduralVisualizerTool`. Installed shared-library
CLI builds use a relative runtime search path to find the sibling library
directory. The Qt GUI remains a build-tree application: a redistributable GUI
bundle also needs the matching Qt runtime and a libpng built for the intended
deployment target.

## Direct CMake Qt build

```sh
cmake -S . -B build-gui -G Ninja \
  -DPVT_BUILD_CLI=ON \
  -DPVT_BUILD_QT_GUI=ON \
  -DBUILD_SHARED_LIBS=ON \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x/<kit> \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-gui --parallel
```

Omit `CMAKE_PREFIX_PATH` when Qt is already discoverable.

## Validation

```sh
make check
```

The suite covers dynamic zero/one/ten-item configurations, deterministic frames,
exact and near-seam continuity for every effect in both synchronization modes,
texture/mapped-object stages, local effect and swing influence, starting
palette ordering and toggle bypass, post-effects quantization,
transforms, direction modes, alpha range and straight-alpha/glow composition,
primitive mappings, rear-surface alpha/color compositing, bounded OBJ
parsing/caching and
two-sided perspective rendering, animated smooth/stepped block grouping and
effect ordering, default glow visibility, memory and value limits, setup round
trips and transactional failure, project/layer validation, every blend mode,
opacity and paint order, ZIP/directory bundle round trips, immutable version
append/no-change validation, semantic diffs, current/revert behavior, legacy
promotion, checksum/fallback handling, and hostile archive/tree rejection,
8/16-bit RGB/RGBA PNG data, compression levels 0 and 9, FLOAT RGB/RGBA EXR channels,
deterministic dithering, byte-identical one/four-worker sequence output,
callback/cancel behavior, sequence collision preflight, Unicode paths, and the
public library API. It also exercises CLI help, option rejection, and the
multi-layer CLI self-test. With the GUI enabled, CTest launches it through Qt's
offscreen platform, exercises project/layer/bundle state, and verifies that Play
installs advancing completed preview frames.

## Planned architecture (not implemented)

The following requests are deliberately not represented as half-working fields
or external-path shortcuts:

- **Metal acceleration:** rendering is currently CPU-only. A future macOS Metal
  implementation should sit behind a backend-neutral frame-render interface,
  with the current CPU renderer retained as the reference and fallback. Backend
  selection, pipeline/resource caching, bounded GPU memory, cancellation, and
  CPU/Metal image/alpha/seam parity need to be designed and tested together.
  The existing frame worker scheduler should choose an appropriate CPU or GPU
  execution policy rather than layering ad-hoc Metal versions into individual
  effects.
- **Layer starting images:** a project must import and embed a bounded,
  checksummed image asset in its bundle; a saved layer must not depend on an
  arbitrary external filename. Layers should reference a stable asset ID or
  digest, while preview/export share an immutable decoded image. Edits and undo
  should use copy-on-write references (or compact deltas), not duplicate a
  full-resolution bitmap in every snapshot. Procedural generation with an
  optional starting palette and an embedded starting image are mutually
  exclusive source modes; either source then participates in the same periodic
  effects, surfaces, transforms, and paths so the result remains loop-safe.
- **Reusable closed motion paths:** paths should be named project resources,
  separate from bindings that attach them to a wave, effect center, or mapped
  object. A path will contain at least three nodes and closed cubic segments,
  with stable node IDs and Corner, Auto Smooth, Smooth, and Symmetric handle
  modes. Three arbitrary smooth nodes do not mathematically define an exact
  ellipse, so the editor should include an ellipse tool that creates the usual
  four-node cubic approximation. Each consumer binding owns enable, sync/free
  clock, cycles, phase, direction, offset, and optional follow-tangent settings.
  A bounded arc-length lookup table should provide visually uniform motion, and
  the GUI needs a dedicated node/handle editor plus strict persistence
  validation. This avoids duplicating geometry or baking one position per frame.

## Current boundary

Plane, cylinder, sphere, and cube mappings use analytic CPU intersections;
custom OBJ mapping uses a bounded cached parser and CPU rasterizer. OBJ materials
and textures are intentionally not loaded because the procedural frame supplies
the surface image. Cooperative cancellation is checked within base rendering,
effects, surface mapping, quantization, layer compositing, and OBJ rasterization.
An image encoder already writing one atomic output file is allowed to finish that
file before cancellation returns. Custom OBJ assets remain external to project
bundles. Metal rendering, embedded starting-image assets, and reusable path
animation remain planned work as described above. See
`IMPLEMENTATION_STATUS.md` for the detailed hand-off ledger.

This project is licensed under GPLv3. Applications distributed with the library
must account for the GPL's linking and source-distribution requirements.
