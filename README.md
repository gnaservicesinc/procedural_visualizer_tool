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
  duplicated, removed, and reordered. Controls include type-specific centers,
  edges, frequencies, harmonics/attenuation, glow bloom parameters, and block
  scale range/mix/quantization steps.
- Transparent, black, white, or reflected out-of-frame handling for coordinate
  effects.
- Multiple dynamic swing modulators with sine, triangle, smooth-pulse, and
  bounce variations, including add, remove, edit, and reorder controls.
- Independent feature toggles for displacement, slope lighting, spiral, and
  wall reflection.
- RGB, luminance, or hue quantization with 2-65,536 levels and adjustable mix.
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
opacity controls, a session-only **Solo** preview, draggable wave handles,
ordered wave/swing/effect editors, type-aware effect controls, a live
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
tree. The first Save/Save As fixes the sanitized archive/directory root. Later
project renames are versioned display data and never move the associated bundle;
a Save As to a new destination chooses a new root, while adopting an exact copied
bundle retains its embedded root. A two-layer first save looks like this:

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

Legacy deterministic line-oriented `.pvt` setup versions 1 and 2 remain
importable; current explicit legacy output is setup format 3, which preserves the
new final-alpha selection. Import creates a new unsaved one-layer project with a
new project/layer UUID and clears its save association, so normal Save can never
overwrite the source `.pvt`. New saves remain bundles. The CLI exposes
`--save-legacy FILE.pvt` only as a clearly lossy escape hatch and rejects it when
more than one layer exists.

## Scripted rendering

Common CLI overrides can be layered on defaults or on a loaded project:

```sh
./build/render9 --load "Midnight Bonfire.zip" --render \
  --width 640 --height 360 --block-size 4 \
  --frames 120 --fps 30 --waves 10 \
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
- progress/cancellation callbacks; and
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
direction modes, alpha range and straight-alpha/glow composition, primitive
mappings, rear-surface alpha/color compositing, bounded OBJ parsing/caching and
two-sided perspective rendering, animated smooth/stepped block grouping and
effect ordering, default glow visibility, memory and value limits, setup round
trips and transactional failure, project/layer validation, every blend mode,
opacity and paint order, ZIP/directory bundle round trips, immutable version
append/no-change validation, semantic diffs, current/revert behavior, legacy
promotion, checksum/fallback handling, and hostile archive/tree rejection,
8/16-bit RGB/RGBA PNG data, compression levels 0 and 9, FLOAT RGB/RGBA EXR channels,
deterministic dithering,
callback/cancel behavior, sequence collision preflight, Unicode paths, and the
public library API. It also exercises CLI help, option rejection, and the
multi-layer CLI self-test. With the GUI enabled, CTest launches it through Qt's
offscreen platform, exercises project/layer/bundle state, and verifies that Play
installs advancing completed preview frames.

## Current boundary

Plane, cylinder, sphere, and cube mappings use analytic CPU intersections;
custom OBJ mapping uses a bounded cached parser and CPU rasterizer. OBJ materials
and textures are intentionally not loaded because the procedural frame supplies
the surface image. Cooperative cancellation is checked within base rendering,
effects, surface mapping, quantization, layer compositing, and OBJ rasterization.
An image encoder already writing one atomic output file is allowed to finish that
file before cancellation returns. Custom OBJ assets remain external to project
bundles. See
`IMPLEMENTATION_STATUS.md` for the detailed hand-off ledger.

This project is licensed under GPLv3. Applications distributed with the library
must account for the GPL's linking and source-distribution requirements.
