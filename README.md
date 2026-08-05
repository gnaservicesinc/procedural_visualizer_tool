# Procedural Visualizer Tool

A seamless-loop procedural image renderer with a reusable C++ library, command-line
editor, and optional Qt 6 desktop GUI. Rendering is performed in linear-light
32-bit floating-point RGBA, then exported as 8/16-bit PNG or full 32-bit FLOAT EXR.

## Requirements

- CMake 3.20 or newer and a C++17 compiler.
- libpng development files for the library, CLI, and tests.
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

- Any number of waves from zero through the validated safety limit, with add,
  duplicate, remove, enable, and reorder controls.
- Per-wave synchronization, placement, amplitude, spatial frequency, phase,
  cycles per loop, and propagation direction.
- An ordered dynamic effect stack with endless zoom, ripple, shake, flag wave,
  and glow. Every effect can be enabled, synchronized, duplicated, removed, and
  reordered. Both clients expose each type-specific center, edge, frequency,
  harmonic/attenuation, glow pulse, radius, threshold, and soft-knee control.
- Transparent, black, white, or reflected out-of-frame handling for coordinate
  effects.
- Multiple dynamic swing modulators with sine, triangle, smooth-pulse, and
  bounce variations, including add, remove, edit, and reorder controls.
- Independent feature toggles for displacement, slope lighting, spiral, and
  wall reflection.
- RGB, luminance, or hue quantization with 2-65,536 levels and adjustable mix.
- Plane, cylinder, sphere, and ray-cast cube mappings.
- Independent procedural alpha modulation with minimum/maximum alpha, spatial
  frequency, phase, and cycles per loop.
- 8/16-bit RGB or RGBA PNG and 32-bit FLOAT RGB or RGBA EXR output.
- Optional deterministic blue-noise-like, ordered Bayer, or Floyd-Steinberg
  dithering for integer PNG output. Dithering is never applied to float EXR.

The GUI includes draggable wave handles, ordered wave/swing/effect editors,
type-aware effect controls, a live checkerboard alpha preview, a loop timeline,
background preview rendering, setup load/save, and between-frame cancellation
for background sequence export.

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

Enable the alpha channel before using transparent effect edges or a curved
cylinder, sphere, or cube surface. Export validation rejects those combinations
as RGB because dropping their generated transparency would not match the preview.
Enabling RGBA is neutral by default (minimum and maximum alpha are both `1.0`);
lower either value to add procedural alpha modulation.

PNG export converts linear RGB to sRGB immediately before dithering and integer
quantization. PNG bit depth is per channel: 8-bit produces standard RGB/RGBA PNG;
16-bit produces RGB/RGBA PNG with 16-bit samples. EXR stores the original linear
RGB(A) values in FLOAT channels, including values above `1.0` created by glow.

## Setup files

Use **Save setup** and **Load setup** in either UI, or use the CLI:

```sh
./build/render9 --save setup.pvt
./build/render9 --load setup.pvt --render
```

The `.pvt` format is deterministic, versioned, line-oriented, and human-readable.
Loading is transactional: bounded parsing, duplicate/unknown-key rejection, exact
type and enum checks, complete central validation, and commit only after the whole
file succeeds. A failed load leaves the active setup unchanged. Saving uses a
synced sibling temporary file, mode-preserving atomic replacement, and never
follows a destination symlink.

## Scripted rendering

Common CLI overrides can be layered on defaults or on a loaded setup:

```sh
./build/render9 --load setup.pvt --render \
  --width 640 --height 360 --block-size 4 \
  --frames 120 --fps 30 --waves 10 \
  --alpha --bit-depth 16 --dither blue \
  --output-dir preview --prefix ripple_
```

Run `./build/render9 --help` for all options. Existing matching output files are
protected unless `--overwrite` is explicit. A full sequence collision preflight
runs before frame zero, and each frame is installed atomically. Overwriting a
regular file preserves its explicit permission mode; overwriting a symlink replaces
the link entry rather than modifying its target.

Options are processed from left to right, so put `--load` before overrides that
should replace setup values. `--defaults` remains a compatibility alias for
`--render`; used by itself, it renders the built-in defaults. Float EXR output
always disables dithering.

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

- fully owned C++ configuration values and dynamic collections;
- default factories, stable ID allocation, and validation;
- float RGBA frame rendering by frame index or normalized phase;
- individual PNG/EXR writing and full sequence export;
- progress/cancellation callbacks; and
- transactional setup save/load.

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
mappings, memory and value limits, setup round trips and transactional failure,
8/16-bit RGB/RGBA PNG data, FLOAT RGB/RGBA EXR channels, deterministic dithering,
callback/cancel behavior, sequence collision preflight, Unicode paths, and the
public library API. It also exercises CLI help, option rejection, and the CLI
self-test. With the GUI enabled, CTest launches it through Qt's offscreen
platform and round-trips a setup through the live GUI state.

## Current boundary

Plane, cylinder, sphere, and cube mappings are implemented as analytic CPU
mappings. Custom OBJ loading needs a bounded mesh parser, UV-aware rasterizer,
depth buffer, perspective-correct interpolation, and path policy; it remains the
main visual feature deferred to a later pass. Cancellation currently takes effect
between frames rather than inside an individual expensive frame. See
`IMPLEMENTATION_STATUS.md` for the detailed hand-off ledger.

This project is licensed under GPLv3. Applications distributed with the library
must account for the GPL's linking and source-distribution requirements.
