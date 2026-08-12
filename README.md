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
- CLI/GUI music import uses pinned, in-tree miniaudio decoding and an adaptive
  beat/spectral analyzer. The same private component provides synchronized GUI
  playback without a Qt Multimedia or system-installed audio dependency. WAV
  (including IEEE 32-bit float), FLAC, and MP3 are accepted. These private
  targets are omitted from a core-library-only build.
- Qt 6.5 or newer with Widgets and Concurrent components for the optional GUI.
- On Apple platforms, the optional Metal backend uses Apple's header-only
  [metal-cpp](https://developer.apple.com/metal/cpp/) from
  `../3rd_party/metal-cpp` plus the system Foundation and Metal frameworks. Set
  `PVT_ENABLE_METAL=OFF` for an explicitly CPU-only build, or
  point `PVT_METAL_CPP_DIR` at another metal-cpp checkout. Other platforms build
  the same public backend API with the CPU fallback.
- On macOS, GUI video export uses the system AVFoundation, VideoToolbox,
  CoreMedia, and CoreVideo frameworks directly. FFmpeg is neither launched nor
  bundled.

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

Create a redistributable macOS app containing Qt, its required plugins, and
non-system dynamic libraries:

```sh
make distribution QT_PREFIX=/path/to/Qt/6.x/macos
# Result: build/Procedural Visualizer Tool.app
```

The unbundled linker output is kept under `build/distribution-intermediate/`;
the top-level app is always the staged, dependency-complete, verified result.

The default signature is ad-hoc. Pass
`-DPVT_DISTRIBUTION_CODE_SIGN_IDENTITY="Developer ID Application: ..."` through
`CMAKE_CONFIGURE_ARGS` when preparing a signed/notarized release.
Unless the caller explicitly supplies `CMAKE_OSX_DEPLOYMENT_TARGET`, this path
targets macOS 13. It builds the existing libpng dependency statically from a
SHA-256-pinned upstream archive instead of inheriting a local/Homebrew dylib,
and therefore needs network access on its first distribution build.

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
- A project-wide base clock with Default, frame-interval, elapsed-time, musical
  meter, and analyzed-music modes. Pulse interpolation can hold, move linearly,
  or ease with smoothstep; direction, phase, beat offset, and exact/fit-to-
  sequence behavior are explicit.
- An optional active-layer clock with the same controls. A layer can follow its
  own analyzed clip while the project timeline remains authoritative. Smart
  loop fit, straight fit, play once, play once then fall back to the project
  clock, and original-speed loop policies make short samples useful without
  destructive source edits. Each Music clock has a **Data only** switch; it
  keeps analysis-driven visuals while muting that source during preview and
  movie export. This defaults on for layer clocks and off for the project clock.
- An ordered dynamic effect stack with endless zoom, ripple, shake, flag wave,
  glow, animated block scaling, and a deterministic spark/trail particle field.
  Every effect can be enabled, synchronized, duplicated, removed, and reordered.
  Each effect explicitly runs either in
  **Texture** space before surface wrapping or on the **Mapped object** after
  wrapping and the layer mirror/flip; the latter moves or deforms the rendered
  silhouette of a cylinder, sphere, cube, or OBJ in final canvas coordinates
  rather than editing its source 3D geometry. Controls
  include type-specific centers, edges, frequencies, harmonics/attenuation,
  glow bloom parameters, and block scale
  range/mix/quantization steps.
- Per-layer closed motion presets: orbit, figure eight, bounce, and Lissajous,
  with center, horizontal/vertical travel, integer loop cycles, phase, rotation,
  and optional scale pulsing. They provide useful animated placement now while
  the more elaborate reusable cubic-path editor remains future work.
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
- A layer-wide Swing enable switch preserves all authored swing settings while
  bypassing the complete block. It remains authoritative in Music mode, where
  swings and audio response can be combined directly.
- Optional audio response routes independent normalized Beat, Onset, Energy,
  Bass, Midrange, Treble, Spectral Centroid, Spectral Flatness, Chroma Hue, or
  Chroma Strength data into wave amplitude, effect intensity, and color hue.
  Energy is the visible default hue route; the other sources remain selectable.
  Pitch-class hue is weighted by tonality confidence, so silence or noise does
  not cause arbitrary palette jumps.
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
- 8/16-bit RGB or RGBA PNG and 32-bit FLOAT RGB or RGBA EXR sequence output.
- PNG compression from 0 (off/fastest) through 9 (maximum), with a balanced
  default of 5. EXR output is unaffected.
- Optional deterministic blue-noise-like, ordered Bayer, or Floyd-Steinberg
  dithering for integer PNG output. Dithering is never applied to float EXR.
- CPU, CPU + GPU, and strict GPU frame backends. CPU + GPU is the application
  default: it runs adjacent project layers through bounded CPU and Metal lanes
  where possible, then preserves bottom-to-top compositing order. Strict GPU
  reports unavailable or unsupported work. Custom OBJ depth peeling remains on
  the CPU; hybrid mode falls back automatically for it.

The GUI includes a topmost-first Layers dock, project naming, per-layer blend and
opacity controls, a session-only **Solo** preview, draggable center handles for
waves, swings, and centered effects with visible radius rings, ordered
wave/swing/effect editors, palette and transform controls, type-aware effect
controls, a live checkerboard alpha preview, a continuously updating timeline,
and background composite export with cooperative cancellation. The
**Synchronization** tab owns both the global Clock and the selected layer's
optional active-layer Clock, Swing, and Audio Response blocks. The Output tab is
global while the Layer Render tab always edits the selected layer. **Randomize
values** keeps the current layer's stack structure and types while varying its
settings; **Randomize mix** creates a new bounded mix. Both live in the Settings
menu and require confirmation, keeping destructive experiments away from the
main toolbar. File dialogs remember their last usable folder and otherwise
begin in the home folder.

Every GUI field edit and structural move participates in session undo/redo.
**Settings > Application Settings…** (also available from the main toolbar)
provides extensible General and Rendering pages for program-wide preferences.
The undo step limit, rendering backend, window layout, and dialog locations are
stored with the platform's normal per-user settings service (`QSettings`), so
they persist across projects and relaunches and are never placed inside a
portable project. The General page can also capture the complete current
project as the template for future **New Project** commands or restore the
built-in template. New documents receive fresh project/layer identities, so
using a saved template never aliases histories or assets. A separate hard 128
MiB snapshot budget prevents a
large, high-layer document from turning a generous step limit into unbounded
memory growth; if history must be trimmed, the document remains correctly dirty.
Saved-version history is separate from session undo.

## Synchronization and seamless loops

The default clock preserves the original behavior: the renderer samples `N`
frames over the half-open interval `[0, 1)` and omits the duplicated endpoint.
Synchronized waves/effects use the shared phase after swing modulation;
unsynchronized items keep their own periodic cycle count and phase.

The Clock block can instead define calculated pulse/keyframe positions:

- **Frame:** one pulse every validated `N >= 1` frames.
- **Time:** one pulse every `N` milliseconds of animation time, derived from the
  frame index and FPS rather than wall-clock/rendering speed.
- **Meter:** a tempo plus tempo-note unit and a bounded meter expression. Simple
  (`7/8`), additive (`3+2+3/8`), mixed (`5/4 | 6/4`), and non-power-of-two
  denominators (`4/3`) are accepted. Meter alone cannot determine elapsed pulse
  time, which is why BPM and its note denominator remain separate controls.
- **Music:** analyzed beat events form a non-uniform clock that follows local
  tempo changes; half-, detected-, and double-time interpretations are
  reversible views of those events.

**Hold**, **Linear**, and **Smoothstep** interpolate the evaluated clock and its
procedural parameters between pulse anchors. They never crossfade two finished
RGBA frames: movement, geometry, effects, waves, alpha, and color are rendered
from the resulting parameter state at each output frame. Dense audio-response
features are sampled at the actual frame time even under Hold, so a transient
between beats can still affect a routed wave/effect/color control.

**Fit to sequence** adjusts pulse spacing enough to close on the sequence
boundary. **Exact interval** preserves the authored real interval. The stored
manual frame count is preserved in every mode; a render-ready Music clock uses
`ceil(source sample frames / sample rate * FPS)` instead, and the GUI disables
the ignored field until another clock is selected.

An enabled active-layer clock locally replaces that clock evaluation without
changing the project timeline or export length. Its duration policy maps the
local source over the project duration: **Smart loop fit** repeats the greatest
whole number of clips that fit and spreads the residual adjustment over that
aggregate; **Straight fit** makes one traversal; **Play once** holds the final
local visual state; **Play once then project** switches visual timing to the
project clock; and **Original-speed loop** repeats unchanged. The one-shot
policies are rejected when the local source is longer than the project because
that configuration cannot reach its intended transition inside the loop.

Wave propagation direction is continuous:

- `0.0`: horizontal propagation
- `0.5`: radial/all-directions behavior (the default)
- `1.0`: vertical propagation

Intermediate values blend between radial and the selected axis.

## Music analysis, synchronized playback, and native video

Import is asynchronous, cancellable, and transactional. The analyzer decodes
the full source, derives sample-accurate duration, tracks time-varying beat and
tempo observations, reconciles them with an offline multiband onset/tempogram
pass, and stores up to 8,192 dense feature samples. It does not reduce a song to
one fixed BPM. The source SHA-256, decoded format, channel/sample metadata,
beats, local tempo points, and normalized spectral/pitch features are cached in
the project; rendering never decodes or analyzes the song again.

Choosing music selects the Music clock and enables Audio Response for the active
layer on first import. The layer checkbox remains authoritative afterward: if
the user turns Audio Response off, switching clock modes or replacing the source
does not force it back on. Swings remain governed by their own active-layer
checkbox and can be mixed with audio response. A relink must match the cached
digest; reanalyze is the explicit way to accept changed audio.

When Play is active, every audible project and active-layer Music-clock source
is decoded and mixed from the matching timeline position. The preview applies
the same loop/fit/one-shot mapping used by the visual clocks. Seeking, beat
navigation, pause/resume, looping, imports, clearing sources, and project
replacement resynchronize or stop the mix. A persistent timeline volume control
changes monitoring volume only. **Data only** sources still drive visuals but
do not enter the mix. If an audio device cannot be opened, visual playback
continues and reports the silent fallback instead of disabling the preview.

On macOS, **Export Video** writes a QuickTime `.mov` directly through
AVFoundation/VideoToolbox. The choices are lossless 8-bit RGBA PNG frames,
ProRes 4444 or 4444 XQ for perceptually lossless editing, and deliberately
high-data-rate HEVC for smaller delivery files. Hardware encoding can be
preferred, required, or disabled; requiring it fails before rendering when the
requested VideoToolbox encoder is not advertised. PNG-in-MOV is codec-lossless
and does not use VideoToolbox because there is nothing useful for its video
hardware to accelerate. Transparency is retained only by codecs that support
it. Audible project and layer-clock sources are rendered into one synchronized
48 kHz float mix and supplied to the movie exporter; Data-only sources are
omitted. Constant-rate fit policies intentionally resample both timing and pitch
for faithful audition of their visual mapping. Source files and stored analysis
remain untouched.

Video duration uses the same effective project clock as preview and sequence
export. Destinations are written through a sibling temporary file, checked,
synced, and installed atomically; existing files are replaced only after the
GUI's explicit confirmation.

## Parallel sequence export and Metal

Sequence exports use a bounded frame worker pool. Each worker renders and
encodes an independent frame; completed files are still installed atomically in
ascending frame order, and progress callbacks run serially on the calling
thread. This keeps collision protection, deterministic filenames, cancellation,
and callback behavior compatible with the former sequential exporter.

The CLI and GUI default to **CPU + GPU**. A multi-layer frame pairs one CPU lane
with one Metal lane when both layers are supported, while single layers and any
remaining supported layer use Metal. CPU rendering remains the reference and is
used automatically when Metal is unavailable, a Metal operation fails, or a
custom OBJ surface requires the bounded CPU rasterizer. **GPU (Strict)** never
hides such a fallback: every contributing layer must use Metal, while final
linear-light project compositing remains on the CPU. The installed library's
legacy overloads retain CPU as their compatibility default; callers opt into
acceleration with
`FrameRenderOptions` or `SequenceRenderOptions::frame`.

Metal compiles the embedded shader source once per process, caches its command
queue and compute pipelines, and admits at most two frames by default (hard
limit eight) before allocating their three shared float-RGBA working buffers.
The device's recommended working-set size and the existing aggregate sequence
memory budget provide additional bounds. Cancellation prevents queued work from
being submitted and keeps the destination transactional. A command buffer that
has already reached the GPU is allowed to finish; its result is discarded when
cancellation is observed.

The CPU renderer intentionally does not add a vImage pass. Its expensive work
is custom procedural sampling, effect evaluation, surface projection, and
linear-light compositing rather than the image-format conversions and standard
filters that vImage accelerates. AppleClang can still auto-vectorize suitable
loops to ARM NEON, while the existing Metal backend handles the much larger
parallel opportunity. Converting every float-RGBA frame into vImage buffers for
a few isolated operations would add bandwidth and precision/alpha transitions
without a demonstrated end-to-end win. Native video conversion likewise keeps
the renderer's explicit linear-to-sRGB/Rec.709 and straight-alpha semantics.

The GUI and the default library overload select workers automatically. The CLI
can choose the upper bound explicitly:

```sh
# Hardware-concurrency auto selection (also the default)
./build/render9 --render --workers 0

# Reproducible sequential reference, or an explicit bounded pool
./build/render9 --render --workers 1
./build/render9 --render --workers 12

# Manual backend selection and optional Metal admission bound
./build/render9 --render --backend cpu
./build/render9 --render --backend cpu+gpu --gpu-in-flight 2
./build/render9 --render --backend gpu
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
  assets/
    <sha256-of-music-or-obj-or-other-attachment>/
      original-filename.ext
    <sha256-of-music-analysis>/
      music_analysis.txt
  0/
    metadata.txt
    render_output.txt
    music_analysis.txt
    0.pvt
    1.pvt
```

`render_output.txt` holds the small global canvas/clock/export settings, while
the small per-version `music_analysis.txt` is a checksum reference to a shared,
content-addressed analysis object. The large time-varying feature table is
therefore stored once when multiple versions use identical analysis, but each
version remains independently reconstructable: there is no base snapshot or
delta chain that can be invalidated by deleting an older version. Exact legacy
snapshots with embedded analysis are compacted on their next Save without
discarding history; direct/manual edits are either preserved or promoted through
the normal external-edit path. Each numbered `.pvt` stores only one layer's
render data. Version metadata stores the project
display name, program/time, layer UUIDs, stable file IDs, order, names, enabled
states, blend modes, opacity, attachment references, and SHA-256 digests. Root
assets use collision-safe content-identity directories, but the asset itself
always keeps the exact imported filename and extension. A valid direct file
replacement or unambiguous rename is loaded as a dirty external edit; Save
records fresh filename/digest/size metadata and promotes it to a new version.
Directly replaced music is reanalyzed
before it is accepted, matching the GUI import behavior. Managed cached copies
are created at attachment time, so moving or deleting the original
image/audio/OBJ before Save cannot break the project. Version-2 bundles with
legacy bare `assets/<sha256>` entries remain readable and are upgraded by the
next changed Save.
Root metadata reflects the current version's project name and also stores the project UUID,
creation/open/save timestamps, creating/changing program versions, and each
version-metadata digest; its digest is necessarily kept in the separate
`metadata.sha256` sidecar.

`current` is a small checksummed text pointer, not a filesystem symlink. This is
portable across ZIP extractors and avoids archive symlink hazards. A changed Save
appends the next numeric directory, then replaces root metadata/current through
checked atomic file operations (a ZIP replaces the whole outer archive);
old snapshots are immutable and gaps are valid. A no-change Save validates the
entire bundle and creates no version. When rewriting a ZIP, already validated,
unchanged entries retain their compressed bytes rather than being recompressed;
the completed temporary archive is still read back and compared with the exact
desired file set before installation. **Make Current** changes root bookkeeping
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
newer save. The lock file contains no project data and intentionally remains
beside the bundle between sessions. Its presence does not mean a save is active;
the operating-system lock state does. Removing it after unlock would be unsafe,
because a waiting process and a new process could then lock different file
identities while writing the same project.

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
unparseable or invalid typed values. Unpacked bundles ignore regular `.DS_Store`
files because Finder can create them merely by browsing a directory; ZIP bundles
remain exact and do not allow such extra entries. ZIP replacement and new directory-version commits use
checked sibling staging and atomic rename operations. Save also refuses a stale
or divergent destination rather than silently overwriting another history. An
exact copied/renamed bundle with the same UUID and observed state can be adopted
by Save As; a different UUID or advanced/divergent state is rejected.

Legacy deterministic line-oriented `.pvt` setup versions 1-5 remain importable;
current explicit legacy output is setup format 6. Format 4 added effect stage,
local-area data, localized swings, starting palettes, and layer transforms;
format 5 adds clock, music-analysis, audio-response, and embedded-source
identity data. Format 6 adds Data-only music, active-layer clocks, compact layer
motion, and particle settings. Older files receive neutral compatibility
defaults. Import creates a new unsaved
one-layer project with a new project/layer UUID and clears its save association,
so normal Save can never overwrite the source `.pvt`. New saves remain bundles.
The CLI exposes
`--save-legacy FILE.pvt` only as a clearly lossy escape hatch and rejects it when
more than one layer exists.

Version 4.0.1 corrected the 4.0.0 palette-stage bug without changing its schema:
an enabled v4 palette selects starting colors instead of rewriting the final
effected image. Versions 5 and 6 add the synchronization/music/asset and local
clock/motion/particle data above.

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
active layer's procedural alpha modulation. It also imports the mesh into the
project's managed attachment cache immediately.

To analyze a song, let it drive selected controls, and save a portable project:

```sh
./build/render9 --music tracks/live-tempo.flac \
  --music-tempo detected \
  --fps 30 --save "Live Tempo.zip"
```

The first `--music` import selects the Music clock and enables active-layer audio
response. Replacements preserve its current state; put `--no-audio-reactive`
later on the command line to override the first-import default explicitly.

Clock overrides also include `--clock default|frame|time|meter|music`,
`--pulse-frames`, `--pulse-ms`, `--meter`, `--bpm`, `--tempo-note`,
`--clock-interpolation hold|linear|smoothstep`, `--clock-fit exact|sequence`,
phase/direction/beat-offset controls, and a selected-layer `--swings` master
toggle. Options are processed left-to-right, so put `--load` before overrides.
The interactive CLI editor additionally exposes active-layer clocks, their
duration and Data-only policies, layer motion presets, and particle controls;
scripted workflows can configure those fields in the GUI or load a saved bundle.

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
output paths. The CLI and GUI import the chosen file immediately into the
project's managed attachment cache and update the remembered file-
dialog folder. A saved bundle therefore remains self-contained after the
original OBJ is moved or deleted. Only the selected OBJ bytes are embedded; the
loader never follows `.mtl`, texture, sibling, or network references implicitly.

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
- backend-neutral CPU/CPU+GPU/GPU frame options and Metal capability reporting;
- a bounded `SequenceRenderOptions` worker policy, ordered atomic output, and
  serialized progress/cancellation callbacks; and
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
directory. On macOS, configure with `PVT_ENABLE_DISTRIBUTION=ON` and build the
`distribution` target (or use `make distribution`) to stage the matching Qt
runtime, plugins, and other non-system dependencies inside the app. A private,
pinned static libpng avoids carrying the build machine's dylib into the bundle.
The packaging check recursively rejects Mach-O dependencies that still point
into `/opt`, `/usr/local`, or a user's home directory; rejects any binary whose
minimum macOS version exceeds the chosen deployment target; requires the app
and third-party license notices; and verifies the completed code signature.

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
transforms, closed layer motion, particles, direction modes, alpha range and
straight-alpha/glow composition,
primitive mappings, rear-surface alpha/color compositing, bounded OBJ
parsing/caching and
two-sided perspective rendering, animated smooth/stepped block grouping and
effect ordering, default glow visibility, memory and value limits, setup round
trips and transactional failure, project/layer validation, every blend mode,
opacity and paint order, ZIP/directory bundle round trips, immutable version
append/no-change validation, semantic diffs, current/revert behavior, legacy
promotion, checksum/fallback handling, readable attachment names and direct-edit
promotion, deleted-original recovery, invalid replacement rejection, hostile archive/tree
rejection, adaptive beat/tempo changes, dense transient and spectral/pitch
features, global/active-layer clock interpolation, duration mapping and response
routing, native PNG/ProRes/HEVC movies, synchronized multi-source audio mixing,
Data-only exclusion, hardware-required encoder
selection, video cancellation/collision safety,
8/16-bit RGB/RGBA PNG data, compression levels 0 and 9, FLOAT RGB/RGBA EXR channels,
deterministic dithering, byte-identical one/four-worker sequence output,
callback/cancel behavior, sequence collision preflight, Unicode paths,
CPU/Metal base/effect/analytic-surface image and straight-alpha parity,
near-seam parity, strict-backend errors, hybrid fallback, bounded admission,
transactional cancellation, and the public library API. It also exercises CLI
help, option rejection, and the
multi-layer CLI self-test. With the GUI enabled, CTest launches it through Qt's
offscreen platform, exercises project/layer/bundle/synchronization state, verifies
that Play installs advancing completed preview frames, and checks adaptive UI
layout behavior.

## Live performance direction

A laptop can already use pre-analyzed project and layer clips for synchronized
visual/audio playback, including short experimental samples used only as clock
data. The program does **not** yet capture a microphone, audio-interface input,
pedalboard return, MIDI, or OSC stream, so it is not yet a low-latency live-input
instrument.

That instrument should stay in this repository. The preferred shape is a
focused **Live mode** or sibling `pvt-live` front end that shares the renderer,
effects, project schema, presets, and validation with the editor. A long-lived
fork would duplicate fixes and make projects drift. Live analysis should be an
ephemeral bounded stream feeding the existing clock/audio-response concepts;
persist device-independent mappings and calibration, not a pretend music file
or machine-specific device identity.

A stage-ready pass should add selectable low-latency audio capture, an input
ring buffer and incremental features, latency calibration, tap tempo plus
MIDI/OSC/foot-controller scene control, full-screen output/display routing,
freeze/blackout controls, dropout-safe last-good behavior, and a frame-time
watchdog. An audio-interface aux or post-effects send would let a performer
choose whether the visuals react to the acoustic instrument, the effected
pedal/looper chain, or both. NDI/Syphon/Spout-style output can follow after the
local performance path is dependable.

## Planned architecture (not implemented)

The following requests are deliberately not represented as half-working fields
or external-path shortcuts:

- **Layer starting images:** the bounded, checksummed, readable attachment
  store is implemented and can already retain generic image attachments. The
  remaining work is the actual layer source-mode schema, decoder/cache, GUI,
  and rendering path. Layers should reference the existing stable attachment
  ID/digest while preview/export share an immutable decoded image. Procedural
  generation with an optional starting palette and an embedded starting image
  should be mutually exclusive source modes; either source then participates in
  the same periodic effects, surfaces, transforms, and paths.
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

Plane, cylinder, sphere, and cube mappings have analytic CPU and Metal paths;
custom OBJ mapping uses a bounded cached CPU parser and rasterizer. OBJ materials
and textures are intentionally not loaded because the procedural frame supplies
the surface image. Cooperative cancellation is checked within CPU rendering,
Metal admission, effects, surface mapping, quantization, layer compositing, and
OBJ rasterization. An in-flight GPU command buffer or image encoder already
writing one atomic output file is allowed to finish, but its result is discarded
before destination installation when cancellation is observed. Music, custom
OBJ, and registered generic attachments are embedded under their content
identity and exact original filename; starting-image rendering is not yet
implemented. Compact closed layer-motion presets are implemented; reusable
editable cubic paths remain planned work as described above. Live audio/data
capture remains future work. See
`IMPLEMENTATION_STATUS.md` for the detailed hand-off ledger.

This project is licensed under GPLv3. Applications distributed with the library
must account for the GPL's linking and source-distribution requirements.
