# Procedural Visualizer implementation ledger

Last updated: 2026-08-20

This is the hand-off point for humans and future coding agents. This repository
is the canonical working tree. Any loose C files retained outside it are legacy
snapshots, not inputs to the current build.

## 7.0.0 Mic clocks and realtime preview output

Both standard clock selectors expose a GUI-only **Mic (Live)...** choice. It
authors project or active-layer Live Audio routes while retaining a deterministic
offline clock, binds detected hardware only in machine-local settings, and
reveals the full gain, sensitivity, filter/EQ/range, meter, tap-calibration, and
latency controls. Duplicate device names receive distinct runtime identities;
stale bindings stay unavailable instead of silently selecting System default.
The single-capture runtime enforces one shared Audio role across simultaneous
project/layer Mic clocks.

The analyzer publishes coherent continuous beat positions for the full-band and
each named frequency stream. Route phase uses detected/tapped tempo only as the
beat-rate numerator and the authored project beats-per-loop as its divisor,
fixing the former cancellation to elapsed project time. Holdover, last-good,
blackout, role matching, capture-null callbacks, authored processing tuples,
calibration locality, input signatures, and layer-removal cleanup were audited
and corrected. Hardware-free beat-clock tests cover tempo rate, missed-onset
continuation, latency, wrapping, invalid inputs, and duplicate device names.
Independent project copies now remap layer/group-scoped Live clock routes, MIDI
outputs, setting mappings, and scene values along with regenerated identities,
so a copied Mic or particle-control setup remains connected.

**Live Preview Output** presents the ordinary preview or Preview Solo on a
chosen display using shared Auto/Full/75%/50%/25% quality and the selected
CPU/Metal backend, but starts none of performance Live's inputs, routes, scenes,
or sleep policy. Its device-pixel-ratio-aware frame controller is revision and
session gated, keeps one render in flight with a latest-pending request, and
handles watchdog adaptation without applying performance blackout semantics.
Presentation Escape/close stops presentation; performance-stage Escape hides
only the stage. Realtime output and export are excluded in both UI state and
entry-point guards. Project load/save and music-analysis transactions stop
presentation first, and their reverse transition into performance LIVE is
guarded at both the action state and entry point.

Known follow-ups are recorded in `REALTIME_OUTPUT_FOLLOWUP.md`. In particular,
the current capture engine is intentionally mono; making the persisted
`audio_channel` selector meaningful requires a multichannel/per-endpoint capture
manager rather than pretending one mono stream is several inputs.

## 7.0.0 particle authoring expansion

New Particle Field effects default to the **Defined** profile with a 9-output-
pixel base radius, visible size variation, and motion-following orientation.
Existing setups and layers migrate to **Legacy Glow**, retaining their prior
defaults and pixels. Defined Spark, Soft Orb, Ring, Diamond, and Star masks are
deterministic, antialiased, and shared by CPU and Metal. Artist controls now
include a logarithmic particle-size scale, exact output-pixel radius, variation,
definition, twinkle, seed/reseed, orientation, rotation, trail, and count. CLI,
randomization, validation, persistence, and focused tests share those semantics.
Live exposes the exactly representable particle controls (not the 64-bit seed),
uses an integer count target, and leaves its stable `radius` path unchanged.

Particle work admission is checked against canvas dimensions and an aggregate
stamp budget before backend allocation. Metal preflight additionally budgets
retained point, tile-grid, offset, and tile-index buffers, checks 32-bit address
limits, and both paths poll cancellation inside generation/raster loops. Defined
stationary fields with non-fixed orientation collapse the visual trail instead
of fabricating authored-direction motion. Unsafe workloads from setup <=14 or
layer <=12 load with the effect disabled, all particle values preserved, the
authored enabled value retained under a non-applying compatibility key, and an
explicit recovery note. Making the workload safe does not auto-enable it on a
later load.

Persistence is setup format 15 and layer format 13. Appending these controls to
the exported by-value `EffectConfig` changes public ABI. Version 7.0.0 therefore
advances the installed shared library to SONAME 7; existing projects remain
migration inputs, while installed-library clients must rebuild against it.

Local 7.0.0 release validation passes all 24 native Release tests, including
Cocoa GUI smoke and real Metal parity; all 23 C++20 tests; all 23 AddressSanitizer
plus UndefinedBehaviorSanitizer tests; and all 24 shared-library tests. A clean
install produced `libProceduralVisualizerTool.7.dylib`, and an external CMake
consumer configured, linked, and ran against that install. The macOS
distribution verifier checked 26 Mach-O files; deep strict signing, embedded
CLI version/self-test, packaged Cocoa smoke, arm64, plist version, macOS 13
deployment target, and `/usr/lib` RPATH checks pass. A freshly archived and
extracted package has one expected package root containing only the application,
README, and license, and its SHA-256 check passes. Physical microphone and
multi-display lab testing and the long performance soak remain manual follow-up;
Linux and Windows packages remain the tag-triggered CI release matrix's
responsibility until those jobs complete.

## 6.0.1 automatic Live window reliability

The LIVE action now opens the performance surface immediately in a companion
window while the main window remains on the editor. The 6.0.0 manual pop-out
path removed the workspace from `QStackedWidget`, which deliberately hid it,
but never restored the central widget's visibility after reparenting; the
result was a valid top-level shell with blank content. The corrected lifecycle
detaches ownership explicitly, shows the adopted central widget, brings an
existing Live window forward, and removes the now-redundant Pop Out button.

**Edit Project** brings the main editor forward without stopping the active
runtime. Closing the Live companion is an explicit stop: capture, rendering,
stage output, and the device-sleep assertion are released before the workspace
is returned to its private stack slot. Cocoa smoke drives the actual LIVE
action, requires a visible non-empty central workspace while the editor remains
selected, verifies uninterrupted authoring, closes and restores the workspace,
then opens and closes it a second time. This patch leaves the public API,
SONAME 6 ABI, setup format 14, layer format 12, and bundle formats unchanged.

Local 6.0.1 release validation on macOS arm64 passes all 23 native tests,
including Cocoa GUI smoke, all 22 strict C++20 tests, and all 22 AddressSanitizer
plus UndefinedBehaviorSanitizer tests. The self-contained distribution verifier
checks 26 Mach-O files; independent deep code-signing, GUI smoke, embedded CLI
version/self-test, arm64, plist-version, and `/usr/lib` LC_RPATH checks also
pass. A freshly archived and extracted package has one expected package root
with only the application, README, and license, and its SHA-256 check passes.
Linux and Windows packages remain the responsibility of the tag-triggered CI
release matrix and are not claimed by these local results.

## 6.0.0 audio routing and creative controls

Music and Live sources now share an `AudioInputProcessingConfig`: optional
high-pass and low-pass filters, a flat-by-default ten-band graphical equalizer,
and stable named frequency ranges. Processing runs before every analysis stage.
Music import computes and caches a complete analysis per named range; Live
capture maintains the same streams causally without allocating in the device
callback. Project and active-layer Music/Live clocks select the full band or a
named stream by UUID, and validation rejects stale processing caches or dangling
routes.

The Live workspace creates the default audio-beat project route when an audio
input role makes that decision unambiguous, offers a searchable grouped target
tree for control maps, detaches into its own window, and sends the actual routed
Live frame to the editor preview. An authored safety preference holds a native
macOS or Windows power assertion only while Live is active. Device identities,
network bindings, display identities, and transient performance state remain
local or ephemeral.

Edge Detect and Twirl join the staged effect catalog with CPU/Metal parity.
Particle Field now offers Spark, Soft Orb, Ring, Diamond, and Star procedural
shapes. Custom PNG sprites remain an asset-backed follow-up. Setup format 14 and
layer format 12 persist all new portable state and downgrade older formats to
their historical full-band, flat-processing, Spark-particle behavior. Because
the new public by-value fields change layout, version 6.0.0 advances the public
shared-library ABI to SONAME 6; installed-library clients must rebuild.

Local validation on 2026-08-19 passed the optimized Qt/Cocoa/real-Metal matrix
(23/23), the strict C++20 headless matrix (22/22), and the ASan/UBSan headless
matrix (22/22 with leak detection disabled). The self-contained arm64 app passed
the 26-Mach-O distribution verifier, deep strict code-signature validation, GUI
smoke, embedded `pvt-render` version/self-test and RPATH checks, and a fresh
single-root ZIP/checksum round trip. A clean shared install produced SONAME 6
and its out-of-tree installed consumer configured, linked, and ran successfully.
The only native build diagnostic was the existing beta-Xcode deployment-version
warning for the host C++ runtime.

## 5.0.3 unrestricted live authoring

Version 5.0.3 decouples the Live workspace from the Live runtime. Opening the
full project editor no longer calls the runtime shutdown path, and returning to
Live reuses the active input/render/stage session. The Live renderer requests a
fresh project snapshot for every frame rather than relying on a UI-refresh
cache, so scalar edits, structural edits, undo/redo, MIDI, and OSC all share one
authoritative project state. Cocoa GUI smoke explicitly opens the editor from
Live and proves the runtime remains active.

The related constraint audit removes UI and validation ceilings that expressed
policy rather than semantics: ordinary render parameters now use a shared
CPU/GPU representation boundary; integer controls use their actual signed-int
storage boundary; Live safety/mapping values, capture-buffer size, recent
projects, and ordinary numeric fields no longer stop at small round numbers. Domain
warp stops only when additional float octaves are no longer representable, and
Endless Zoom uses the largest finite base-2 float exponent instead of a
four-octave product cap. Normalized mixes, enum ranges, portable filename
components, protocol fields, hostile-input expansion checks, and checked
memory arithmetic remain intentionally bounded.

Per-wave/effect audio routing no longer depends on the item's synchronized
clock toggle; the explicit synchronized-only profile policy remains
authoritative when selected. Play Once and Play Once Then Project accept local
sources longer than the project and expose their honest reachable behavior
instead of disabling the choices. Regression coverage exercises values beyond
the former caps, long one-shot sources, and free-running item audio response.

This patch does not change the public configuration layout, SONAME 5 ABI,
setup format, layer format, or project-bundle format.

Local static and SONAME-5 shared release builds each passed all 23 CTest cases
with the host system C++ runtime path, including CPU, Metal, AVFoundation,
bundle, CLI, and Cocoa GUI smoke coverage; the headless ASan/UBSan build passed
22/22. The macOS 13 arm64 distribution target then verified 26 Mach-O files,
its ad-hoc signature and dependency closure; the staged GUI smoke, embedded
`pvt-render` version/self-test, architecture checks, archive root, and SHA-256
generation also passed.

## 5.0.2 wave-output reachability

Version 5.0.2 makes the Wave editor's render prerequisites explicit. Waves
produce visible pixels through generated-pattern displacement, slope lighting,
or both; previously those two layer switches lived only under Modifiers, so an
enabled wave could appear inert without any explanation. The Wave page now
shows both output switches in place, keeps them synchronized with the existing
Modifiers controls, and reports whether enabled waves have an active output.
The supplied Test3 bundle is the regression case: its version 4 contains an
enabled wave while both output switches are off.

Automated GUI smoke now verifies the silent-output warning and its recovery.
After first confirming that the persisted rendering-backend preference was
restored correctly, the remainder of smoke rendering uses CPU so a user's
strict-GPU preference cannot make the test depend on host GPU availability.

Local 5.0.2 validation passed the Release CPU/core and CLI suite (21/21), the
native AVFoundation video-export check, the Cocoa GUI smoke test, and the
public `pvt-render --version`/`--self-test` checks. Real Metal execution,
signed distribution verification, and native Windows/Linux packages remain
gates of the tag workflow rather than claims from this host.

This patch does not change the renderer, public API, SONAME 5 ABI, setup
format, layer format, or project-bundle format.

## 5.0.1 cross-platform release reliability

Version 5.0.1 repairs the two integration failures found by native release and
PPA builders after the 5.0.0 feature tag. The CLI eagerly initializes the
Qt/OpenGL service on its main thread before the sequence renderer starts worker
threads, so a worker never blocks on a GUI-thread dispatch while the CLI main
thread waits for that worker. Qt's post routine now releases GL objects, returns
the context to the GUI thread, and joins the dedicated render thread before
platform teardown.

Ubuntu minizip-ng 4 declares its zero-copy reader buffer mutable. Palette/KPL
loading now supplies a lifetime-stable mutable `std::string` buffer, matching the
upstream contract without copying or casting away constness. Portable GPU
admission is intentionally platform-specific: Windows includes flat Plane
rotation plus Cylinder/Sphere/Cube; Linux includes Cylinder/Sphere/Cube and
keeps flat Plane rotation on CPU after Mesa parity caught a straight-RGB source
block mismatch. Displacement Plane and OBJ meshes remain ordered CPU stages.

## 5.0.0 displacement Plane and closed Cylinder

Version 5.0.0 replaces the built-in Cylinder's former rectangular side mask
with a closed perspective ray-cast primitive. The CPU and Metal paths intersect
the cylindrical side plus both caps, use a fixed cap-revealing tilt and authored
Y rotation, texture sides and caps separately, apply the same lighting rule,
and composite the exit surface behind a translucent entry surface. A shape
regression rejects a rectangular alpha bounding box, while the existing
neutral-curvature, shell-alpha, distinct-rear-color, loop, and CPU/Metal parity
contracts remain active.

Plane mapping now owns optional `PlaneDisplacementConfig` state: embedded PNG
path/content identity, signed minimum and maximum height, normalized zero-height
midpoint, and positive pixels-per-node ratio. A checked grid generator samples
linear PNG luminance, preserves both output edges, emits indexed triangles,
UVs, and smooth normals, and uses stable 2D-aspect normalization so the same
authored height range does not change scale when the map's extrema change.
Decoded-image identity, effective render dimensions, ratio, and all height
controls key a bounded LRU mesh cache. The editor preview and adaptive Live
renderer already scale the effective project canvas before rendering, so they
request lower-resolution grids; full frame/video and full-quality Live paths
request their corresponding output grid. The GUI exports the authored-output
mesh atomically as a Wavefront OBJ.

Height maps are layer-scoped content-addressed attachments. They materialize,
duplicate, detach, independently-copy, compare, recover, and save with the same
transactional bundle rules as starting images and custom OBJs. GUI and CLI
source controls validate/decode before commit, keep source selection reachable
while use is disabled, preserve a disabled asset for later reuse, and enable
final alpha whenever the displaced mesh exterior can be visible. Metal renders
the ordered mesh stage on CPU and resumes the same strict GPU pipeline rather
than silently retrying a failed layer on CPU.

Setup format advances to 13 and layer format to 11; formats through setup 12
and layer 10 migrate to neutral displacement defaults. Public by-value
configuration layout grows, so the product and shared-library ABI advance to
5.0.0/SONAME 5 and installed clients must rebuild. Existing projects remain
compatible inputs.

Windows and Linux Qt product builds now compile a public-API OpenGL 3.3 surface
backend. A process-lifetime service creates a `QOffscreenSurface` on the GUI
thread, owns one serialized render context on a dedicated thread, uploads and
reads float RGBA textures, and evaluates the analytic closed Cylinder, Sphere,
and Cube mappings in a fragment shader. Windows additionally admits flat Plane
rotation; Linux keeps that inexpensive transform on CPU after Mesa parity
testing exposed driver-dependent straight-RGB sampling. `RendererCapabilities` reports
compiled/available state, driver renderer, and actionable status. CPU + GPU
uses the stage when available and does not hide an admitted shader/runtime
failure behind a CPU retry. Strict GPU requires a supported active analytic
surface on these platforms; imported OBJ and displacement-Plane meshes remain
explicit ordered CPU stages. Metal remains preferred on macOS and unchanged in
coverage. The release matrix builds the backend on x64/ARM64 Windows and Linux;
the Linux job exercises CPU/OpenGL parity under Xvfb/Mesa, while physical
Intel/AMD/NVIDIA hardware qualification remains outstanding.

Application Settings and Video Export now place their content inside resizable
scroll areas, cap themselves to the active screen's available geometry, and
leave their dialog buttons outside the scrolling region. GUI smoke verifies the
settings scroll contract and screen bound, covering the high-scale/small-screen
cutoff shown in the supplied screenshot.

Local release-candidate validation passes all 23 optimized static and all 23
optimized shared native tests, including real Metal parity and Cocoa GUI smoke;
all 22 strict C++20 CPU/CLI tests; and all 22 ASan/UBSan CPU/CLI tests. The
OpenGL translation unit passes the same strict C++ warning policy against Qt
6.8.3, and both embedded GLSL 330 shaders pass `glslangValidator`. A clean
installed shared-library consumer configures, links, and runs against SONAME 5.
The self-contained macOS distribution verifies all 26 Mach-O files, passes deep
code-signature verification, and passes GUI and CLI smoke both before and after
single-root archive extraction. Remote Windows/Linux native OpenGL/package
results remain the release tag's final gate.

## 4.0.1 generated alpha reliability

Version 4.0.1 fixes a first-layer-only transparency failure in the built-in
Workbench template. That template explicitly disabled `alpha.use_source_alpha`
on its original layer even though normally added layers inherited the public
default of true. Because generated alpha was also incorrectly gated by that
palette/PNG control, selecting **Include alpha as a generated color dimension**
could still produce an opaque first layer.

Generated alpha is now controlled directly by
`StartingColorConfig::include_alpha`, while `AlphaConfig::use_source_alpha`
continues to non-destructively suppress only starting-palette and embedded-PNG
alpha. CPU and Metal constants/kernels, final-alpha reachability, export
validation, GUI/CLI wording, Live target labels, and the built-in layer default
use the same rule. The supplied two-layer project is an exact regression case:
its top layer has generated alpha enabled, alpha range 0 through 0.5, and the
legacy source-alpha flag off; a fixed render retains varying non-opaque alpha.

This patch does not change public structure layout, SONAME 4, setup format 12,
or project-bundle formats.

Local 4.0.1 release validation passed all 22 native optimized tests, including
real Metal and Cocoa GUI smoke, plus independent 21/21 optimized C++20 shared-
library and 21/21 AddressSanitizer/UndefinedBehaviorSanitizer matrices. The
supplied `alphabug.zip` rendered through the packaged strict-GPU CLI as RGBA;
its first frame retained a varying 8-bit alpha range of 25 through 223 instead
of becoming opaque. An external installed consumer linked and ran against
`libProceduralVisualizerTool.4.dylib` with current version 4.0.1. The
self-contained arm64 macOS application passed verification over 33 Mach-O
files, deep strict code-sign verification, embedded CLI version/self-test, and
GUI smoke before and after archive extraction. Its single-root archive contains
only the application, README, and license and passes its generated SHA-256
check. Remote platform packages remain the tag-triggered workflow's release
gate.

## 4.0.0 Live performance and final post effects

Version 4.0.0 adds the first integrated Live performance surface and final
post-processing expansion. Portable authored state now covers logical
audio/MIDI/OSC/foot endpoints, mappings, project/layer
clock inputs, project/layer 24-PPQN MIDI outputs, scenes, latency compensation,
display preferences, and watchdog/dropout policy. The Qt runtime adds causal
low-latency audio analysis, CoreMIDI and OSC routing, MIDI Learn, full-screen
secondary output, scene recall, freeze/blackout, and latest-frame/last-good
safety. Device identities, OSC address/port bindings, display identities,
captured buffers, current scene, freeze, and blackout remain machine-local or
ephemeral. `LIVE_PERFORMANCE_STATUS.md` records deferred routing and the exact
hardware/release handoff.

Layer-local Post Effects now include independently mixed RGB inversion, alpha
inversion, and premultiplied edge antialiasing before quantization, with CPU and
Metal parity paths and Qt controls. The editor also has a scalable dark studio
theme and custom vector-painted knobs/meters/lamps inspired by the supplied UI
references. A height-for-width policy bug that collapsed wrapped help labels
was fixed globally. Setup persistence advances to format 12 while compatible
layer/render/split wrappers route older formats to neutral defaults.

The new public Live and post-processing members change exported C++ struct
layout, so Version 4.0.0 advances the shared-library ABI to SONAME 4 and
installed clients must rebuild. Existing setup and project data remain
compatible migration inputs. Native controller/interface and venue soak testing
remains documented in `LIVE_PERFORMANCE_STATUS.md`; non-Apple MIDI backends and
Syphon/Spout/NDI routing remain explicit follow-up work rather than hidden
fallback behavior.

Local 4.0.0 release validation passed all 22 native optimized tests, including
real Metal and Cocoa GUI smoke, plus independent 21/21 optimized C++20 shared-
library and 21/21 AddressSanitizer/UndefinedBehaviorSanitizer matrices. An
external installed consumer linked and ran against
`libProceduralVisualizerTool.4.dylib`. The self-contained arm64 macOS app
passed the distribution verifier over 33 Mach-O files, deep strict code-sign
verification, privacy-plist and Qt Network checks, embedded `pvt-render 4.0.0`
version/self-test, and GUI smoke both before and after archive extraction. The
single-root archive contains only the app, README, and license beneath its
package directory and passes its generated SHA-256 check. Physical audio/MIDI,
display hot-plug, and long-running venue soak checks remain explicitly
unperformed hardware qualification.

Version 3.0.2 is a logic-correctness patch for Flow Workbench timing, editing
scope, and neutral effects. Play-once-then-project layer clocks now fall back to
the project clock exactly once under every opt-in project/layer clock-mixing
mode, rather than feeding the project phase back through a second mix. Group
selection and authoring locks continue to protect layer-owned stages,
synchronization, and audio overrides while leaving project-wide Project and
Export workspaces reachable; the layer-analysis Cancel action also remains
available while its clock editor is disabled.

Neutral Lens Distortion effects now remain true no-ops across work admission,
memory estimation, final-alpha reachability, CPU compositing, and the Qt output
editor. This patch adds focused core, composite, and GUI-structure regressions
for those invariants. It does not change the public API, SONAME 3 ABI, setup
format, or project-bundle format.

Local 3.0.2 portable validation passed 21/21 tests in optimized C++17 and an
independent optimized C++20 build, plus 21/21 under AddressSanitizer and
UndefinedBehaviorSanitizer with leak detection disabled on this macOS beta
host. The rebuilt `pvt-render` reported 3.0.2 and passed self-test. Qt 6 and
metal-cpp were unavailable in this checkout, so the native GUI, real Metal,
signed distribution, archive layout, and downloadable assets remain the
tag-triggered native package matrix's release gate.

Version 3.0.1 is the focused UI follow-up to the 3.0.0 renderer and ABI release.
It replaces the misleading Surface and Object FX primary stages with seven
purpose-based workspaces, separates all 11 effect types into five exclusive
catalogs, makes Texture the invariant default for newly added and randomized
effects, restores first-class Project and Export discovery, and keeps the full
synchronization editor behind a compact live summary. No public API, SONAME,
setup format, or project-bundle format changes in this patch.

The local 3.0.1 release build passed all 22 tests, including native Metal and
Cocoa GUI smoke. The distribution verifier passed over 28 Mach-O files, the
staged and freshly extracted applications passed deep strict code-signature and
build-machine dependency checks, embedded `pvt-render` reported 3.0.1 and passed
self-test, and the 25 MiB macOS archive passed its generated SHA-256 check with
only the application, README, and license beneath its package root.

Version 3.0.0 implements the Flow Workbench overhaul and
the requested procedural expansion. The native Qt editor presents seven focused
workspaces—Project, Starting Colors, Modifiers, Movement, Layer Effects, Post
Effects, and Export—with a compact collapsible Synchronization strip and direct
Project Settings entry points. Procedural shaping, alpha, transforms, and the
advanced surface-mapping controls live under Modifiers. A single ordered Layer
Effects editor filters its complete catalog into Movement & Distortion, Light &
Energy, Stylize, Particles, and Blur; each new or randomized effect starts on
Texture while mapped-surface placement remains an explicit advanced choice.
The default new-project document is a neutral 1920×1080, 60 FPS, one-layer
starting point modeled on the supplied `Untitled.zip`; saved custom defaults
continue to override it.

Active-layer clock mixing is an explicit advanced opt-in and defaults off in
both new and migrated data. Disabled mixing preserves the historical layer-
replaces-project result. Enabled mixing combines independently transformed
project/layer phases by Replace, Add, Difference, continuous Soft XOR, or exact
24-bit XOR before Swing, while project duration/frame count remains
authoritative and dense layer-music features retain their local envelope.

Generated sources add loop-seam-safe Kaleidoscope and deterministic Domain Warp
shaping. The ordered effect stack adds Glitch, Starburst, and Lens Distortion,
including type-specific Qt/CLI controls and CPU/Metal parity. Setup format 11
and project layer format 9 persist these values and recover neutral behavior
when older records are absent.

Palette interchange is implemented as a non-executing, bounded parser/exporter
for GIMP GPL, Krita KPL, GIMP-style CSS/Python/PHP/Java/text, PNG, and FLOAT EXR.
Image palettes traverse row-major, ignore alpha-zero pixels, and keep the first
exact decoded duplicate. The UI presents a structured summary before Replace or
Append and reports precision/name/alpha/encoding loss after export. Palette
entry names, column layout, alpha, and per-entry sRGB versus finite linear/HDR
values now round-trip through setup 11, layer 9, project bundles, and the local
palette library.

These additions grow installed by-value configuration structs. Version 3.0.0
therefore advances the shared-library ABI to SONAME 3, and installed clients
must rebuild. Existing setup and project data remain supported migration inputs.

Focused implementation validation covered the core renderer, bundle,
project/composite, palette-I/O, native GUI build/smoke, and strict Metal parity
paths. The final local 3.0.0 release build passed all 22 tests, including native
Metal and Cocoa smoke, and produced a verified ad-hoc-signed macOS application
containing 28 Mach-O files, the ABI-3 renderer library, and `pvt-render 3.0.0`.
The beta host toolchain requires `/usr/lib` as a test-only dyld fallback; the
staged application passed the project's dependency, deployment-target, and
deep-signature verification. Remote package matrices and release assets remain
the tag workflow's responsibility.

## 2.0.1 release record

Version 2.0.1 is a correctness and security-hardening patch. Arithmetic now
widens before multiplication wherever CodeQL identified a narrow intermediate
being converted to a larger buffer-size, frame-count, sample-offset, cache,
filter, or chroma result. The 34 findings covered the pinned miniaudio and
Beat-and-Tempo-Tracking copies plus one project-owned chroma calculation; they
were valid width/precision hardening opportunities rather than 34 independently
confirmed exploits. Review-only upstream patches are retained under
`patches/upstream/` and have not been submitted.

This patch does not change the public API, SONAME 2 ABI, setup format, or
project-bundle format. The following 2.0.0 section is retained as the preceding
renderer-semantics and ABI release record.

Version 2.0.0 is a renderer-correctness and shared-library ABI boundary. Layer
Starting phase is now independent of cycle count; reusable-path Reverse changes
travel without negating the authored start; follow-tangent direction survives
the Metal preparation path; and CPU/Metal agree on static rotation, reusable
motion, and exact identity cases. Numeric generated-color values remain pinned
so older compiled callers still map value 4 to the legacy square spiral, while
the public SONAME advances to 2 because by-value configuration structs grew
during 1.x.

RGB/RGBA validation now follows the final visible stack rather than individual
controls in isolation. It includes authored palette/generated alpha, honors the
source-alpha switch, recognizes reusable paths and starting rotation, models
erasers in paint order, and avoids forcing alpha for zero-travel motion, neutral
scale pulses, inactive blur, opaque-decoded images, or guaranteed zero-alpha
erasers. The GUI and CLI use the same reachability decisions and no longer
silently force one-way RGBA settings when adding or duplicating opaque layers.

Extreme valid meters are stored as compact pulse runs instead of allocating
billions of objects. Signed 64-bit beat offsets are reduced without losing their
low microseconds, including tiny cycles and negative boundary corrections.
Project-only copies reject detached embedded-image identities; CLI editing now
round-trips palette alpha, generated RGBA controls, source-alpha policy, and
starting rotation. GUI background completions retain their originating document
metadata, replacement stops playback and refreshes derived state, and the Cocoa
smoke exits without an unattended dirty-document prompt.

The following 1.2.6 section is retained as the preceding release record.

The 1.2.6 generated-color correction makes every ordered pattern read as a
rainbow without throwing away source colors. **Continuous hue** remains the
default. Horizontal, vertical, diagonal, true radial spiral, and square spiral
choices now traverse the complete automatically sized RGB/RGBA lattice in
hue-major order across the full render. The old rectangular-ring artwork is
preserved as **Square spiral**, including migration of its saved token, while
**Spiral** now produces the circular winding its name promises. Deterministic
bijective color static remains available only as the explicit **Random** mode.

Generated sources automatically size their float32 RGB/RGBA lattice from the
full-resolution dimensions and block size, preserve Min/Max ranges for every
generated choice including Continuous hue, keep every block unique whenever
the selected ranges and output representation permit it, and use transient
full-resolution coordinates in reduced previews. Authored palettes and images
are intentionally outside that generated-range stage. With all procedural
controls disabled the ordered generated source is time-invariant, block size
is honored, and current-frame/sequence exports sample the same source placement
as preview. Direct 16-bit PNG decode coverage
proves that source values never pass through an 8-bit intermediate; 32-bit
FLOAT remains available for EXR output, while starting-image input is explicitly
8/16-bit PNG.

Metal now renders generated orderings, generated/source alpha, fitted-image
palette selection and parallel dithers, reusable path bindings, and particle
fields. Floyd-Steinberg input quantization and custom OBJ depth peeling remain
ordered CPU stages inside the same accelerated frame rather than forcing a
whole-layer retry. CPU + GPU pairs independent project layers on CPU and Metal,
uses Metal for a single layer's parallel pixel stages, and surfaces unexpected
Metal errors instead of silently repeating a frame on CPU. The supplied Test
Fire project renders successfully in GPU mode at 1920×1080; its two
otherwise-static test frames are byte-identical and each of the 2,073,600
pixels has a distinct exported RGB tuple.

Blur modulation now uses `Cycles per loop` as its sole count and ignores the
retained legacy `blur_pulses_per_cycle` record; the duplicate UI field is gone.
Saving preserves the user's selected tab, including background completion, and
Cocoa smoke covers the regression. Effect synchronization is labeled simply
`Synchronization`, without the misleading swing-clock wording.

The public product version is now 2.0.1 and has one source of truth in
`VERSION`. CMake propagates it to the GUI, About PVT, native app metadata,
installed package metadata, and saved-project provenance;
`scripts/bump-version.sh` performs a validated SemVer/prerelease bump without silently
tagging, building, or publishing a release. The installed shared-library SONAME
and Debian runtime package advance to ABI 2; saved setup and bundle formats
remain loadable.

The 1.1.5 correctness patch keeps the Versions page synchronized with document
replacement. Starting a new project clears the prior bundle's rows, summary,
diff selectors, and action targets; comparison remains unavailable until the
active document has at least two saved versions. Completion from an older
background comparison is revision-gated and cannot re-enable stale controls.

The 1.1.4 performance patch removes complete-history decoding from ordinary
Open and Save. The selected snapshot is still fully validated and materialized;
history metadata/tree identities remain checked, while historical content is
decoded on demand for comparison, revert, direct open, or explicit validation.
Music-analysis decimal parsing now avoids a locale/facet stream per value,
split analysis is decoded once, and a runtime-only dual 64-bit semantic
fingerprint makes no-change detection cheap without replacing any persisted
SHA-256 integrity identity. Changed saves reuse their already-computed semantic
identity instead of serializing the project twice.

Normal GUI Open and Save now run as transactional QtConcurrent work with visible
busy state and editing guards; completion adopts the document atomically and
continues pending close/new/open/version actions. Version comparison is an
explicit background action instead of hidden synchronous work when the Versions
tab refreshes. On the supplied 41 MiB Potato Fire directory, local CLI timings
fell from approximately 5.3 to 1.05 seconds for load, 11.6 to 1.77 seconds for a
no-change load/save, and 13.2 to 3.38 seconds for a changed load/save.

The 1.1.3 persistence patch restores directory projects as a first-class GUI
workflow: Open / Import explicitly chooses a project file or unpacked folder,
and Save As offers the unpacked form first for large projects. Storage now keeps
one physical copy per attachment SHA-256 even when historical references retain
different filenames. Repeated active-layer music-analysis tables are split into
shared content-addressed objects and legacy history is compacted transactionally
on the next explicit Save. Normal completion removes the transient sibling save
lock by file identity before unlocking; stale blank locks from crashes or older
builds are reused and cleaned by the next successful save.

On the supplied 18-version Potato Fire directory, load improved from 28.1 to
10.8 seconds without modifying the fixture. A cloned explicit-Save migration
reduced the expanded tree from 419 MiB to 41 MiB, retained all 18 loadable
versions, collapsed the reported duplicate MP3/PNG objects, and then loaded in
about 4.0 seconds.

The 1.1.2 patch keeps embedded starting-image layers on Metal: the bounded PNG
decoder supplies a linear-float source buffer, while Stretch, Contain, Cover,
and Tile fitting run in a dedicated GPU stage. Built-in layer placement,
rotation, and scale now run on Metal too, so image-driven animated layers do not
fall straight back to the CPU or fail strict GPU mode. CPU/Metal parity covers
every image fit and every compact motion path, including their combined path.

The 1.1.1 patch makes the starting-image source flow reachable: **Choose…** and
**Clear** remain available while the source is disabled, and a separate **Use
embedded PNG as layer source** checkbox controls rendering after import. Cocoa
GUI smoke now verifies that disabling the source cannot disable its chooser.

The 1.1.0 feature pass adds a full-quality **Export Current Frame** command. It
captures the current timeline position, renders at the authored canvas size
through the selected CPU/hybrid/Metal backend, and writes the selected 8/16-bit
PNG or 32-bit FLOAT EXR settings transactionally. The live preview's display
scaling is never reused for this export.

Each layer now has a persistent Alpha Over/Alpha Under choice. Alpha Over is the
default and the migration value for older bundles; Alpha Under swaps the
Porter-Duff source/backdrop order after applying layer opacity while retaining
the selected artistic blend. Destination-out erasers remain explicit masks of
the accumulated lower stack. CPU, hybrid, strict-GPU layer scheduling, GUI,
CLI, undo, validation, semantic diffs, and project-version manifest format 5
share the same model.

The Project & Layers panel now provides flat, non-nested groups as contiguous
folder blocks in the existing paint order. Groups contain one or more layers
and support add, rename, visibility, session-only solo, lock/unlock, whole-block
movement, and safe removal that ungroups instead of deleting artwork. Layer
membership changes preserve contiguity, locked groups protect contained edits,
group visibility also gates audio and export, independent project copies remap
group identities, and old bundles load without synthetic groups.

The accompanying capacity audit removes product-policy ceilings that were not
grounded in an implementation boundary. OBJ input no longer stops at 64 MiB,
one million records, 4,096 face corners, a 256 MiB expanded mesh, or eight
transparent depth layers. Canvas/frame counts, collection counts, setup and
bundle metadata, text, source images, audio analysis, attachments, project
history, undo depth, and worker/GPU admission now follow their concrete
`int`, `uint32_t`, `int64_t`, `size_t`, filesystem-component, codec, or
allocation boundary. Checked arithmetic, transactional parsing/writes,
archive traversal/collision/type rejection, checksums, compression-ratio abuse
protection, and caller-configured resource budgets remain intact.

The 1.0.1 patch restores the documented sync/free timing boundary. Timeline
resolution now retains both the effective synchronized project/layer clock and
the independent linear loop clock, so free waves, effects, and motion-path
bindings no longer inherit Frame, Time, Meter, or Music Hold behavior. CPU and
Metal carry the same two clocks. Endless Zoom now turns intensity above 1 into
additional octave depth after its source mix reaches 100%, making positive
audio response visible at the default intensity. The GUI and CLI expose Audio
Response only when the active layer's effective project or local clock is
Music, and the zoom intensity label documents its two ranges.

The final audit repairs the Block Scale editor invariant in both GUI and CLI:
raising its minimum now raises an incompatible maximum, while lowering the
minimum restores the full valid maximum range. Synchronization and audio-source
labels now describe the controls' actual clock and profile-master semantics.
The tag workflow distinguishes stable versions from prereleases, so final tags
publish a current stable GitHub release instead of being mislabeled as release
candidates. Archive parsing and source-lifetime code also received defensive
cleanup identified by static analysis.

RC3 makes the synchronized wave/effect Audio response control a real source
override: Default inherits the effective profile, direct Beat/Energy and other
feature choices opt the item in, and the prior format-8 force/ignore meanings
remain available and migrate exactly. Setup format 9 and layer format 7 carry
the richer selector. The floating Project & Layers panel can now be redocked by
double-clicking its title or recovered unconditionally through View; invalid or
off-screen saved dock geometry is repaired on launch.

The RC1 persistence pass fixes the split-render-output v1 migration boundary
that incorrectly required `paths.count` in pre-path bundles. Setup/layer/output
readers now salvage every independently valid typed field, rebuild missing or
unusable values from bounded defaults, retain unknown records verbatim, and
store rejected originals in a recovery envelope for later builds to retry.
Actual recovery data drives GUI/CLI notices; historical product-version strings
alone no longer produce a false data-loss warning. The supplied 12-version
`In stages it unfolds Fire.zip` and expanded directory both load, validate,
copy, and reopen without rewriting history. Legacy `.pvt` import also now keeps
the project clock and reusable paths instead of silently resetting them.
The accompanying logic audit fixed compatibility loss in GUI Save Copy, CLI
edits, and project-global synchronization; made dependent recovery groups retry
after their prerequisites; retained unknown future root/version metadata; and
refuses a save rather than silently dropping malformed internal preservation
data.

This pass also closes former remaining-work items 2 through 6. Layers can use a
bounded embedded PNG source with Stretch, Contain, Cover, or Tile fitting and a
shared bounded decoded-image cache. Reusable project-level closed cubic paths
have stable path/node IDs, explicit handles and handle policies, a four-node
ellipse tool, bounded arc-length sampling, and independently clocked bindings
for the active layer, wave sources, and effect centers. Texture/local-Swing
editing on non-plane surfaces uses a labelled unwrapped source/UV inset instead
of pretending a screen-space circle is a projection. PNG and EXR encoders check
cancellation between scanlines while preserving atomic-output cleanup. The Qt
application is installed by `cmake --install`; system-Qt installs and opt-in Qt
runtime deployment are documented for Linux/Windows, while the verified macOS
distribution path remains the release-grade bundle route.

The layer blend list adds three destination-out operations. Erase uses source
alpha; Color Eraser (tones) uses a soft linear-light color-distance match; Color
Eraser (brightness) removes backdrop pixels darker than the mask layer. All
three affect only already-composited lower layers, never later layers above.
About PVT includes GPLv3-or-later/no-warranty information plus project, bug, and
private vulnerability-report links.

Cross-platform GPU and native-video parity are not falsely declared complete.
Metal and AVFoundation/VideoToolbox remain the tested native implementations;
PNG/EXR sequences remain portable. Optional Linux GStreamer export and a
user-selected, never-downloaded-or-redistributed Windows FFmpeg executable are
post-1.0 candidates that require native platform validation.

`PORTABILITY_ROADMAP.md` now makes the next implementation sequence concrete.
The Qt editor should use Qt's public OpenGL context/offscreen APIs for a first
Linux/Windows GPU backend rather than adding a second GLFW event/window layer.
GLFW remains a possible thin PVT-Live presentation front end. Windows native
movie work should prefer Media Foundation before an explicitly selected FFmpeg
fallback; Linux should use an optional GStreamer API backend.

### Earlier version-6 foundation

Version 6 adds optional per-layer clocks, Data-only Music sources, five bounded
duration mappings, compact closed layer-motion presets, and a deterministic
spark/trail particle effect. Preview playback now mixes every audible project
and active-layer clock source at the same mapped timeline position used by the
renderer. Native movie export renders that synchronized mix before passing it
to AVFoundation/VideoToolbox; Data-only sources remain visual control data and
are silent. Music import/clear, seeking, beat navigation, pause, looping, and
project replacement all resynchronize safely. The new setup, layer, and
render-output records retain compatibility defaults for older files.

The live-performance architecture is bounded and integrated as a sibling Qt
mode in this repository. Low-latency device input and incremental features are
ephemeral renderer inputs; projects retain logical roles, mappings, scenes,
clock routes, calibration, and safety policy without storing hardware identity,
captured buffers, or active freeze/blackout/scene state.

Application Settings can store the complete current project as the per-user
template for future new documents or reactivate the built-in default; each new
document still receives independent project/layer identities. Randomize Values
and Randomize Mix moved from the toolbar into Settings and now require
confirmation. The supplied PNG is generated into a complete macOS icon family.
The public and local wrapper Makefiles expose `make distribution`, whose CMake
target uses `macdeployqt`, signing, required-content checks, and recursive Mach-O
dependency/deployment-target verification to produce a self-contained app
bundle. Distribution defaults to macOS 13 and statically builds a SHA-256-pinned
libpng, avoiding this workstation's macOS-26 local dylib; program and
third-party license notices are embedded before signing. The bare linker bundle
lives under `distribution-intermediate/`; the obvious top-level app is the
verified distributable, preventing it from being confused with an incomplete
build-tree bundle.

vImage was evaluated and deliberately not inserted into the CPU renderer. Its
standard conversions/filters do not match the dominant custom procedural,
projection, effect, and float-compositing work; extra frame-format passes would
cost memory bandwidth and complicate precision/alpha semantics. AppleClang
retains normal NEON auto-vectorization opportunities and Metal remains the
purpose-built parallel backend.

The application is now project-oriented. A named `ProjectConfig` owns global
canvas/loop and export data plus independently configurable render
layers. Each layer has a stable UUID and bundle file ID, name, enabled state,
opacity, blend mode, alpha mode, optional group membership, and full
`RenderData`. Flat groups carry stable identities, names, visibility,
and authoring locks; their contiguous member runs remain part of the one layer
paint order. The backward-compatible
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
snapshot owns a small `render_output.txt`, an optional checksum reference to a
shared content-addressed music-analysis object, layer `.pvt` files, and
checksummed metadata. Identical multi-megabyte analysis tables are stored once,
without history deltas or a dependency on the oldest version. Exact legacy
snapshots are compacted transactionally on their next Save. Root metadata and
its separate SHA-256 sidecar index immutable versions;
`current` is a portable checksummed text pointer rather than a filesystem
symlink. Changed saves append; no-change saves verify recorded history trees and
the current snapshot while explicit Validate decodes all history. Make Current only
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
The lock sidecar is transient: cleanup targets the still-locked file identity,
and a contender verifies path identity after acquiring so normal completion can
remove the file without splitting cooperating writers across stale inodes.

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
Regular Finder `.DS_Store` files are ignored only in unpacked bundles so browsing
a directory project cannot make it unloadable; link/special-file checks remain.
New directory snapshots and ZIP replacements use sibling staging/atomic rename;
stale or divergent destinations are refused. ZIP updates raw-copy validated
unchanged compressed entries and read back the complete temporary archive before
installation, avoiding repeated compression of large attachments and history.

On the reported 23-version Cody Fire fixture, shared analysis reduced the ZIP
from 90,408,835 to 68,864,205 bytes and the logical bundle from roughly 179 MiB
to 74,609,085 bytes. A reproduced clean CLI load-and-save fell from 49.97 to
7.36 seconds; the one-time migration took 17.19 seconds. The remaining ZIP size
is dominated by the already single-copy, poorly compressible 69,120,154-byte
WAV attachment.

The subsequent layer-analysis/asset-identity pass covers the 18-version Potato
Fire fixture. Before migration, load fell from 28.1 to 10.8 seconds through
component-digest and parsed-layer reuse. One explicit directory Save compacted
419 MiB to 41 MiB and removed byte-identical MP3/PNG filename aliases; the
complete compacted history then loaded in about 4.0 seconds.

All registered files now use a generic readable attachment store. Music, custom
OBJ meshes, images, and future attachment types are copied into a managed cache
at import and saved once beneath `assets/<sha256>/`. The digest directory is the
physical identity; logical version references retain their exact user filenames
and extensions even when multiple names share those bytes. Valid direct
replacements or unambiguous renames are loaded dirty and promoted with fresh
identity metadata on Save; directly replaced music is reanalyzed before
acceptance. Legacy version-2 bare-digest assets remain readable.

The Qt GUI separates Synchronization, Layer Render, and global Output, adds a
topmost-first Layers dock, project name/title, blend/alpha/opacity/enable/rename/
duplicate/reorder controls, group folder controls, layer/group session Solo,
full-resolution current-frame export, version list/diff/current/revert tools,
and application-wide undo/redo. Synchronization owns the global Clock plus the
selected layer's optional local Clock and master Swing, plus visible project-wide
Audio Response defaults and an optional active-layer override. Synchronized
waves and effects can inherit the effective profile or override it with Beat,
Onset, Energy, spectral, tonal, and other analyzed sources; advanced force and
ignore choices preserve format-8 behavior. An effective profile summary makes
inheritance explicit, and a one-click copy action creates a layer override from
the project profile. Dense panels now use consistent
spacing, frameless scrolling, scrollable document tabs, and explanatory
tooltips for non-obvious behavior.
Optional checkable blocks collapse to compact headers while disabled, keeping
the complete control surface discoverable without forcing every editor open.
Undo depth, window state,
and dialog locations use per-user platform settings. Undo depth can be Unlimited
up to Qt's signed-int command index; allocation failure clears history safely
without clearing document edits or dirty state.
Preview/export cancellation now reaches per-frame effects and OBJ rasterization;
stale previews are cancelled and document replacement is blocked during export.
The GUI now also exposes draggable numbered center handles and visible radius
rings for effects and swings, Texture versus Mapped-object effect stages,
custom/built-in per-layer palettes, directional mirror plus horizontal/vertical
layer transforms, and closed motion presets. Block Scale remains a whole-image effect
without a center/radius overlay. Version 5 adds a project-wide Default/Frame/
Time/Meter/Music clock, parameter-state Hold/Linear/Smoothstep interpolation,
time-varying music analysis, beat navigation, and layer audio-response routing.
Version 6 added local clocks and mapping, Data-only sources, motion, and particles;
setup formats 1-7 load with compatibility-preserving defaults under current setup
format 8. The CLI exposes the same
clock/music/swing/audio-response/motion/particle state plus immediate portable
music and OBJ attachment import.

Music analysis is deliberately not a fixed whole-song BPM estimate. A causal
local beat/tempo observer is reconciled with offline multiband spectral-flux and
local-tempogram evidence, while dense feature samples retain Energy, Bass,
Midrange, Treble, Onset, Beat, Spectral Centroid, Spectral Flatness, Chroma Hue,
and Chroma Strength. These independent routes can modulate waves, effects, and
color at the actual frame time, including between clock pulses. Interpolation
always evaluates procedural parameters; rendered RGBA frames are never
crossfaded.

Music-mode frame count is derived from decoded sample frames/sample rate and
FPS while preserving the stored manual count for later modes. PNG/EXR image
sequences remain the portable cross-platform workflow, while the macOS GUI also
offers native QuickTime movie export.

Sequence export now uses independent-frame CPU workers for rendering and
encoding. Automatic selection is bounded by hardware concurrency and frame
count; explicit counts fit the signed-int worker/API index. A configurable
aggregate memory budget derives admission from the checked per-frame estimate.
Output installation and progress callbacks remain in
ascending frame order. On the representative Apple M2 Max workload recorded for
this pass (24 frames, 960x540, block size 1, 64 waves, PNG compression 0), 12
workers reduced wall time from 44.95 seconds to 5.44 seconds (8.3x), with all 24
PNGs byte-identical. This is evidence for that workload, not a general speedup
guarantee.

Native video export uses the same checked frame-level scheduling principle.
Independent workers render and convert lossless-PNG or BGRA payloads ahead of
the movie writer; AVFoundation append, presentation timestamps, progress, and
audio synchronization remain serialized in frame order. Automatic selection is
bounded by hardware concurrency and frame count, with the configurable default
2 GiB aggregate admission budget, so video no longer reduces procedural rendering
to a one-core render/encode loop. On a CPU-only 24-frame 960x540, block-size-1,
64-wave lossless-PNG movie workload, automatic 12-worker export reduced wall
time from 50.96 seconds to 5.72 seconds (8.9x) on the same M2 Max host. This is
one representative measurement rather than a universal scaling guarantee.

The CLI opens ZIPs, directories, and legacy setups; renders composite projects;
edits the selected layer; manages representation-bounded layers; separates final alpha from
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
| 2 | Optional synchronization per wave/effect | Complete | Each wave and effect has a `synchronized` toggle; free clocks are independent but still periodic. Every item can inherit the audio profile or choose a specific feature, force the profile feature, or ignore audio; only the explicit synchronized-only profile policy filters free-running items. |
| 3 | Toggle/add/remove/reorder any quantity | Complete | Dynamic wave, swing, and effect collections support zero through the signed-int UI/API index capacity; memory availability is the practical lower boundary. |
| 4 | Direction from horizontal through radial to vertical | Complete | Continuous `0.0` horizontal, `0.5` radial/default, `1.0` vertical control. |
| 4a-f | Endless zoom, ripple, shake, flag wave, glow, block scale | Complete | Ordered, configurable effects; coordinate effects support alpha/black/white/reflected edges; Glow uses visible straight-alpha-safe HDR bloom; Block Scale animates smooth or stepped pixel grouping at its stack position. |
| 4g | Particle field | Complete; release matrix pending | Legacy-compatible or Defined antialiased particles provide five distinct silhouettes, output-pixel size, variation, definition, twinkle, deterministic reseeding, orientation/rotation, motion-following trails, count, intensity, locality, and composable Texture/Mapped-object staging with bounded CPU/Metal work. |
| 5 | 8/16-bit PNG and 32-bit float EXR | Complete | RGB/RGBA PNG at 8 or 16 bits per channel with compression levels 0-9 (default 5); RGB/RGBA uncompressed scanline EXR with FLOAT channels. |
| 6 | Optional alpha throughout | Complete | Internal images are straight float RGBA, including meaningful RGB at zero alpha; RGB export drops only the fourth channel. |
| 7 | Float processing and lower-depth dithering | Complete | Linear float image pipeline; sRGB conversion plus blue-noise-like, Bayer, or Floyd-Steinberg dithering for PNG; dither is off/ignored for EXR. |
| 8 | Safe setup save/load | Complete | Versioned deterministic `.pvt` files, bounded strict parser, complete validation, transactional load, atomic/durable save, UTF-8 paths, and no NUL truncation. |
| 9 | Library-only build and useful Qt-facing API | Complete | `libProceduralVisualizerTool`, public header, installable CMake package, example consumer, and build switches that omit every `main`. |
| 10 | Qt GUI using the library | Complete | Qt Widgets client with live async preview, draggable wave/swing/effect center handles and visible local-radius rings, dynamic stack editors, all configuration fields, timeline, project bundle/legacy import I/O, background export, consistent dense-panel layout, and meaningful tooltips for non-obvious controls. |
| 11 | More quantization/swing levels and variations | Complete | Quantization levels span 2 through signed-int storage, with RGB/luminance/hue modes and mix; dynamic sine/triangle/smooth-pulse/bounce swing stacks. |
| 12 | Plane/cube/sphere/cylinder/custom OBJ wrapping | Complete | Analytic built-ins plus transactional, representation-bounded cached OBJ parsing and perspective rasterization; authored UV/normal data has safe fallbacks. All mappings are two-sided and transparent closed surfaces peel until exhausted. |
| 13 | Configurable PNG compression | Complete | Levels 0-9 are available in the API, setup v2-v9, CLI, and GUI; level 5 is the balanced default and EXR ignores it. |
| 14 | Randomize stack values or composition | Complete | Confirmed Settings-menu actions preserve existing identity/type structure or create a new bounded mix of waves, swing waveforms, effect types, and enabled items; they are no longer exposed on the main toolbar. |
| 15 | Stable paths, dialogs, and playback | Complete | Relative paths are anchored to a stable launch directory, first file dialogs use home then remember their last location, completed previews advance during Play even under timer/render overlap, Space owns preview play/pause outside text editors, and audible global/layer Music-clock tracks are synchronized and mixed. |
| 16 | Named projects and default filenames | Complete | Semantic name appears in the title and a portable sanitizer supplies the initial `.zip`. Saved-project rename choices either preserve that path or create a current-state-only independent project with fresh identities, then open it or stay in the original. |
| 17 | Full render-config layers | Complete | Global canvas/export data is split from per-layer render data; layer switches cannot overwrite global output state. Stable UUIDs/file IDs survive names and reorders. |
| 18 | Layer compositing | Complete | Sequential bounded float-RGBA compositing implements all 11 requested modes plus opacity; only one layer frame and one accumulator are retained. |
| 19 | Alpha split | Complete | Per-layer procedural modulation and global final RGB/RGBA selection are independent. Multi-layer creation enables final alpha without changing artwork; validation follows actual final-composite transparency. |
| 20 | Human-readable project bundles | Complete | ZIP/directory bundle tree stores root/version metadata, small per-version global output, shared content-addressed music analysis, per-layer `.pvt` data, SHA-256 indexes, and a portable text current pointer. |
| 21 | Automatic immutable save versions | Complete | Changed Save appends; clean Save validates and compacts exact legacy analysis; ZIP saves reuse unchanged compressed entries; load fallback, field-level recovery/preservation, external-change promotion, semantic diff, Make Current, and revert-as-new preserve recoverability. New Project clears every prior-version view and action target. |
| 22 | Legacy compatibility without overwrite | Complete | Setup v1-v8 imports remain supported; setup v8 adds hierarchical/nullable audio routing and setup v9 adds explicit per-item feature overrides while preserving every older meaning. Only explicit one-layer `--save-legacy` writes `.pvt`. |
| 23 | GUI session undo/redo and preferences | Complete | All editor/structural actions use undo/redo; Application Settings exposes an Unlimited-or-signed-int step limit, rendering backend, and complete current-project default template. Allocation failure safely clears history without losing edits; UI preferences live outside portable bundles. |
| 24 | Hostile-input bundle handling | Complete | Strict tree/archive/metadata bounds and checks reject traversal, collisions, links/special files, unsupported/encrypted archives, expansion abuse, stale saves, and invalid typed data transactionally. |
| 25 | Saved-project rename workflow | Complete in Qt GUI | Keep the existing filename, Save As/open an independent version-0 copy, save that copy and stay with the old name restored, or Cancel. Copy creation is no-clobber and the open-document swap is transactional. CLI name edits remain ordinary semantic renames. |
| 26 | Better CPU utilization | Complete | Image sequences and native movies use checked frame-level render/convert workers with ordered output and serialized progress; `--workers 0..INT_MAX` configures portable sequences and native video auto-selects within hardware and configurable memory admission. Representative M2 Max image-sequence measurement: 44.95 s to 5.44 s (8.3x). |
| 27 | Texture versus mapped-surface effects | Complete | Each effect runs before surface mapping or after it; mapped-object coordinate effects move/deform the primitive silhouette. Relative order is retained within each stage. |
| 28 | Draggable/localized effects | Complete | Numbered preview handles edit centers; zero area radius preserves whole-layer behavior and positive radii add feathered local influence. Glow blur and influence radii remain separate. |
| 29 | Localized Swings | Complete | Zero radius retains global clock modulation; positive shorter-edge-relative radius creates a movable feathered source/UV timing region for waves and Texture effects. Mapped-object effects use the global synchronized clock because projection is not uniquely invertible. |
| 30 | Per-layer starting palettes | Complete | One or more embedded sRGB source colors through signed-int UI/API indexing, six presets, custom GUI/CLI editing, reliable independent enablement, and once-per-procedural-block linear-light selection. Lighting and effects may create other colors afterward; post-effects quantization remains separate. |
| 31 | Transform and move layer | Complete for compact controls | Directional mirrors/flips plus loop-safe orbit, figure-eight, bounce, and Lissajous placement, rotation, and scale pulse run after surface mapping and before mapped-object effects and post-effects quantization on both CPU and Metal. |
| 32 | Metal GPU acceleration | Complete | Backend-neutral CPU/CPU+GPU/GPU rendering accelerates live preview and export through cached metal-cpp pipelines, three bounded shared frame buffers per admitted render, output-scaled generated sources, fitted-image palette/dither and alpha handling, path resolution, particles, starting images, motion, analytic surfaces/effects, transactional cancellation, and CPU/Metal image/straight-alpha/seam parity tests. Project frames pair independent CPU and Metal layer lanes with ordered compositing. Floyd-Steinberg source quantization and custom OBJ depth peeling are dependency-ordered CPU stages inside the accelerated frame; neither causes whole-layer fallback. Available-Metal failures are surfaced rather than silently retried on CPU. |
| 33 | Layer starting image | Complete | Embedded PNG layers use the existing managed attachment store, strict metadata/path/dimension bounds, a shared 512 MiB/64-entry LRU decoded cache, CPU and Metal Stretch/Contain/Cover/Tile sampling, CLI and GUI controls, undo, bundle copies, cancellation, and seam/render/parity coverage. Procedural palettes are bypassed only at the source stage; later effects/surfaces/transforms/motion/quantization still apply. |
| 34 | Closed reusable motion paths | Complete | Named project-level paths contain at least three stable-ID cubic nodes through signed-int UI/API indexing, with explicit handles and Corner/Auto Smooth/Smooth/Symmetric policies. A dedicated GUI table editor includes a four-node ellipse factory and handle fitting. Separate layer/wave/effect bindings own sync/free clock, integer cycles, phase, reverse, offset, and tangent following. Setup v7, layer v5, render-output v4, bundles, semantic diffs, validation, undo, CPU rendering, Metal preparation, checked arc-length sampling, and seam/round-trip tests cover the feature. |
| 35 | Synchronization tab and clock controls | Complete | Global and optional active-layer Default/Frame/Time/Meter/Music clocks, interpolation, fit/exact spacing, direction/phase/beat offset, five local-duration mappings, Data only, an authoritative per-layer Swing block, project audio defaults, and a visible layer override live in one GUI tab and in the CLI/API. |
| 36 | Adaptive high-detail music response | Complete | Full-source decoding plus time-varying beat/tempo reconciliation and a dense multiband/onset/spectral/chroma track through signed-int container/API capacity drive the base clock and independently routable wave/effect/color response. First project import enables the shared profile without creating layer overrides, Energy supplies a visibly dynamic default hue route, and later user choices remain intact. No fixed whole-song BPM clock is used. |
| 37 | Native music-video export | Complete on macOS | AVFoundation/VideoToolbox writes atomic MOV files as lossless PNG, ProRes 4444/XQ, or high-rate HEVC; bounded parallel frame render/conversion feeds one ordered writer while hardware policy, alpha, synchronized audio, Data-only exclusion, progress, cancellation, and collision safety remain explicit. No FFmpeg executable or library is used. |
| 38 | Portable embedded attachments | Complete | Music, OBJ, image, and generic files retain exact filenames/extensions beneath collision-safe digest directories, accept valid direct replacements as first-class edits, keep managed copies, and retain hostile/oversize rejection plus v2 compatibility. |
| 39 | User-defined new-project defaults | Complete in Qt GUI | Settings can transactionally capture the complete current project or reactivate the built-in template; every new document regenerates project/layer identities and starts unsaved. |
| 40 | macOS icon and self-contained distribution | Complete | The supplied PNG generates a full ICNS resource; public and local `make distribution` targets stage Qt/plugins, statically build pinned libpng, embed license notices, enforce the macOS deployment baseline across every Mach-O, reject build-machine paths, sign, and verify the app. |
| 41 | vImage/NEON assessment | Complete - no vImage integration | The workload is dominated by custom float procedural/effect/projection code; compiler NEON vectorization plus Metal is a better fit than extra vImage format passes without measured benefit. |
| 42 | Active-layer clocks | Complete | Each layer may locally override the project clock while retaining the master duration; Smart loop fit, Straight fit, Play once, Play once then project, and Original-speed loop are validated, persisted, previewed, and exported. |
| 43 | Live performance mode | Implemented; hardware/release validation pending | A sibling Qt performance surface shares the renderer and project model; the complete project editor can remain open while Live input/render/stage output continues, with fresh project snapshots per frame. Allocation-free capture/incremental analysis, portable MIDI/OSC/foot mappings, project/layer clocks and MIDI clock output, scenes, full-screen output, freeze/blackout, last-good behavior, and a watchdog are integrated. Machine bindings and active performance state stay local/ephemeral. See `LIVE_PERFORMANCE_STATUS.md`. |
| 44 | Destination-out eraser blends | Complete | Alpha erase, soft linear-light tone erase, and brightness-threshold erase affect only the accumulated lower stack; serialization, CLI, GUI descriptions, alpha validation, and composite tests cover them. |
| 45 | Projected source/UV editing | Complete | Texture-effect and localized-Swing handles move and render inside an explicit labelled unwrapped source/UV inset for non-plane surfaces; mapped-object controls remain in final screen space. |
| 46 | Encoder cancellation and GUI install | Complete | PNG/EXR writers abort between scanlines and discard their temporary file. `cmake --install` installs the Qt application when enabled; `PVT_DEPLOY_QT_RUNTIME` optionally stages Qt dependencies, while system-Qt installs remain the Linux default. |
| 47 | Hierarchical audio-response routing | Complete | A project-wide profile feeds inheriting layers; an optional layer profile overrides it; every wave/effect can inherit, opt in with an explicit source, force the item on with the profile source, or ignore audio independently of clock synchronization. The profile master and explicit synchronized-only policy remain authoritative. Missing/null fields are neutral, old projects preserve historical output, every CPU/Metal preparation path shares the same semantics, and GUI/CLI/persistence/undo tests cover the hierarchy. |
| 48 | Export current frame | Complete | The GUI renders the current timeline frame at full canvas resolution through the selected backend and transactionally writes the configured 8/16-bit PNG or 32-bit FLOAT EXR quality, independent of preview scaling. |
| 49 | Per-layer Alpha Over/Under | Complete | Alpha Over retains legacy source-over behavior; Alpha Under places the layer beneath the accumulated lower stack after opacity while preserving artistic blend selection. GUI, CLI, project render paths, manifest v5 migration, semantic diffs, validation, and composite tests cover it. |
| 50 | Layer groups | Complete | Flat contiguous folder groups support one or more layers, rename, visibility, preview solo, authoring lock/unlock, membership changes, atomic reordering, and safe remove-to-ungroup. Rendering, audio, undo, bundle persistence/copies, validation, and Cocoa GUI smoke coverage share the same semantics. |
| 51 | Expanded final post effects | Complete; full release matrix pending | Layer-local linear RGB and alpha inversion have independent mixes; edge antialiasing works in premultiplied space with strength, threshold, and a positive signed-int pass count before quantization. CPU, Metal, persistence, GUI, and interactive CLI paths share neutral defaults and representation-bounded controls. |
| 52 | Pre-analysis audio processing and named clock streams | Complete; hardware qualification pending | Music and Live apply optional HP/LP plus graphical EQ before analysis, then expose stable named frequency-range analyses to project and active-layer clocks. Defaults are flat/full-band; validation, transactional reanalysis, setup-14 persistence, and causal/offline tests cover routing. |
| 53 | Live workflow expansion | Complete; hardware qualification pending | Audio inputs receive a smart default beat route, control-map targets use a searchable grouped tree, LIVE opens automatically in a visible companion window while the editor remains available, editor preview consumes routed Live frames, and supported platforms can prevent sleep for precisely the active session. |
| 54 | Edge/Twirl effects and particle shapes | Complete; release matrix pending | Linear-light Edge Detect, seamless Twirl, and five visibly distinct deterministic particle silhouettes share CPU/Metal, GUI, CLI, Live mapping, randomization, validation, migration, and persistence semantics. Custom PNG sprites remain deferred. |

## Important implementation map

- `include/procedural_visualizer_tool.h`: public C++ API and complete owned config model.
- `src/core.cpp`: defaults, validated clock/meter/music-event evaluation,
  parameter interpolation, audio-response routing, periodic/spatial phases,
  staged effects including particles, palettes, transforms and closed layer
  motion, float RGBA layer rendering,
  quantization, alpha, and analytic surface mappings.
- `src/frame_renderer.cpp` / `src/metal_backend.cpp` / `src/metal_kernels.metal`:
  backend-neutral dispatch, explicit Metal failure reporting, cached metal-cpp resources and compute
  pipelines, bounded admission, transactional cancellation, and GPU kernels for
  output-scaled base generation, starting images and palette mapping, ordered effects including particles, analytic surfaces,
  transforms, built-in layer motion, and quantization. Non-Apple/disabled
  builds use `src/metal_backend_stub.cpp`.
- `src/composite.cpp`: project/layer validation, UUIDs, active-layer clock
  timeline mapping, linear-light blend modes, bounded ordered compositing,
  hybrid CPU/Metal layer pairing, and project frame rendering.
- `src/obj_mesh.cpp` / `src/obj_surface.cpp`: bounded Wavefront parsing/cache and
  perspective, two-sided, layered custom-mesh rasterization.
- `src/image_io.cpp`: PNG/EXR encoding, bounded frame-worker scheduling, PNG
  compression, dithering, collision preflight, ordered atomic installation,
  serialized progress, and cancellation checks.
- `src/config_io.cpp` / `src/config_codec.cpp`: setup v1-v14 codec and split
  per-layer/global bundle records with transactional legacy file I/O.
- `src/project_bundle.cpp` / `src/bundle_archive.cpp`: checksummed project/version
  metadata, semantic history operations, readable/direct-editable attachment storage,
  independent current-state copies, bounded ZIP/directory loading, and atomic
  save staging. This helper is intentionally not installed ABI.
- `src/audio_input_processing.cpp` / `src/audio_analysis.cpp`: shared bounded
  pre-analysis filters/EQ, private full-source decoding, named frequency-stream
  analysis, adaptive beat/local-tempo reconciliation, and dense multiband/
  onset/spectral/chroma extraction.
- `src/audio_playback.cpp`: bounded real-time multi-source monitoring and
  cancellable float WAV mixing using the same local duration mappings as visual
  clock evaluation.
- `third_party/miniaudio` / `third_party/btt`: pinned private decoder and causal
  beat/tempo observer; neither enters the installed core ABI.
- `app/cli_main.cpp`: project/layer-aware interactive client plus scripted
  clock, music, attachment, audio-response, and worker controls.
- `gui/main_window.cpp`: Qt 6 project/layer/version client, global/local
  Synchronization UI, asynchronous analysis, undo/redo, saved-rename workflow,
  spatial centers, palettes/transforms/motion, adaptive unclipped layouts, and
  cancellable sequence/video export with synchronized audio mixing.
- `tests/test_main.cpp`, `tests/project_composite_test.cpp`, and
  `tests/bundle_test.cpp`: core/seam/format/setup/I/O, layer/blend/project, and
  persistence/archive-safety coverage, including worker determinism, setup-v8
  compatibility, clock/music response, staged/local effects, embedded assets,
  palettes/transforms, readable/direct-edited assets, and rename copies.
  `tests/audio_analysis_test.cpp` covers analysis accuracy/density and offline
  synchronized audio mixing.

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
- Sequence and native-video workers operate on independent frames, not layers.
  The count is capped by frames and the signed-int worker/API index; automatic
  selection also uses hardware concurrency. A configurable default 2 GiB
  aggregate admission budget remains a host resource policy. Image encoding and video conversion may overlap,
  but file installation or AVFoundation append, timestamps, and progress remain
  ordered; sequence collision preflight still happens before frame zero.
- Effects are grouped into two explicit stages: every Texture effect runs before
  surface wrapping and every Mapped-object effect afterward. List order remains
  stable within a stage; the mapping boundary cannot be interleaved ambiguously.
- Effect and Swing radii use zero as the backward-compatible whole-layer mode.
  Positive values are feathered circles measured against the shorter canvas
  edge; Glow's pixel blur radius is not reused as its influence radius.
- The explicit pipeline is procedural base generation, optional starting-palette
  selection, Texture effects, surface mapping, layer mirror/flips,
  Mapped-object effects, post-effects RGB/alpha inversion and premultiplied
  edge antialiasing, then explicit quantization. A later
  localized mapped effect can intentionally break earlier mirror symmetry;
  neither starting-palette selection nor transforms rewrite alpha.
- A pre-product renderer revision called 4.0.1 intentionally reinterpreted the
  existing v4 `palette.enabled` field as source-stage enablement. Those
  revision numbers predate the product SemVer line. No schema or ABI layout
  changed, but affected legacy projects render differently after correction.
- Project layers are stored bottom-to-top; the GUI reverses that order for a
  conventional topmost-first list. Rendering skips disabled layers and composites
  enabled layers sequentially into one accumulator.
- Layer UUIDs and file IDs are unique and stable. Reordering never renames layer
  files; deletion deliberately leaves gaps. Session Solo is never persisted.
- IDs are stable, nonzero, and unique after insertion; factory objects start at
  ID zero and must receive `allocate_id(config)`.
- Capacity limits are tied to actual representations and APIs: Qt-facing
  indexes/text fit signed `int`, OBJ corner indexes reserve `UINT32_MAX` as a
  sentinel, clocks persist signed 64-bit microseconds, filesystem components
  use a portable 255-byte boundary, and storage uses checked `size_t`
  arithmetic plus successful allocation. Semantic parameter domains, codec
  formats, caller-selected worker budgets, and hostile-archive protections are
  documented separately rather than disguised as capacity limits.
- Existing output frames are protected unless overwrite is explicit, including
  an all-frame preflight and atomic no-clobber installation.
- Transparent effect edges and curved 3D-surface exteriors can generate alpha;
  validation rejects RGB export only when the final composite can retain
  transparency. An opaque lower stack may make a translucent upper layer safe
  for RGB, although adding a second layer defaults final output to RGBA.
- Transparent closed surfaces composite every distinct depth until no deeper
  surface remains. OBJ rasterization never culls by winding.
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
- Bundle commits hold a transient hidden sibling advisory lock and compare the
  complete expected on-disk state while locked. Successful completion removes
  that exact lock identity before unlock; sidecars contain no project data.
- New attachments live once beneath `assets/<sha256>/`; logical references keep
  imported filenames while identical bytes share one physical file. Valid
  direct replacements are treated as external edits and Save gives them a
  fresh identity; legacy bare-digest assets remain readable.
- Music rendering trusts only cached analysis tied to a valid embedded source
  digest. Relink verifies identity; reanalysis is the explicit content-change
  path. The clock uses event times, never one global BPM estimate.
- GUI undo snapshots grow to the configured signed-int step limit or Unlimited.
  Allocation failure clears history safely without clearing edits or dirty
  state, and no-op normalized edits do not create commands.

## Validation record

The 2026-08-17 2.0.1 correctness and security-hardening release passed the
complete Qt-enabled Release suite 21/21, the independent C++20 suite 20/20,
and the AddressSanitizer plus UndefinedBehaviorSanitizer suite 20/20 with leak
detection disabled on the macOS beta host. GitHub CodeQL accepted the widened
arithmetic changes and reports zero open alerts after all 34 findings were
resolved. The self-contained macOS verifier inspected 27 Mach-O files; the
embedded `pvt-render` reported 2.0.1 and passed self-test, while native Cocoa
smoke and deep strict signing passed. A workflow-shaped arm64 archive passed
single-root layout, ZIP integrity, SHA-256, extraction, version, self-test,
signature, and extracted-runtime checks. The release preserves the 2.0.0
public API, SONAME 2 ABI, setup format, and project-bundle format.

The 2026-08-17 2.0.0 correctness and ABI release passed the complete
Qt-enabled Release suite 21/21, including native Metal parity and Cocoa GUI
smoke coverage. The independent C++20 suite passed 20/20, and the
AddressSanitizer plus UndefinedBehaviorSanitizer suite passed 20/20 with leak
detection disabled on the macOS beta host. A shared-library install exposed
`@rpath/libProceduralVisualizerTool.2.dylib` at compatibility/current version
2.0.0, and an independent installed-package consumer linked and ran against it.
The self-contained macOS application passed embedded CLI version/self-test,
native Cocoa smoke, deep strict signing, arm64 architecture, dependency and
RPATH inspection, single-root archive layout, ZIP integrity, extraction,
checksum, and extracted-runtime checks without a library-path override.

The 2026-08-16 1.2.6 generated-rainbow correction passed the complete
Qt-enabled Release suite 21/21, including CPU and real-Metal coverage for all
generated choices. Regression coverage proves exact Cartesian-set preservation,
hue-sector ordering, distinct radial and square spiral geometry, old-token
migration, channel Min/Max handling, 24K reference scaling, time stability,
and preview/export coordinate parity. The supplied Test Fire project renders
as an ordered broad rainbow through both CPU and strict GPU execution. The
self-contained macOS distribution and final workflow-shaped archive passed
version, self-test, native Cocoa smoke, deep strict signing, arm64 architecture,
macOS deployment-target, single-root layout, ZIP integrity, and SHA-256 checks.

The 2026-08-16 1.2.5 generated-rainbow correction passed the complete
Qt-enabled Release suite 21/21, the independent C++20 suite 20/20, and the
AddressSanitizer plus UndefinedBehaviorSanitizer suite 20/20 with leak detection
disabled on the macOS beta host. Core regressions cover the complete 2×2×2×2
RGBA Cartesian product in every ordered/Random mode, non-square diagonal and
spiral bijections, all-unique 192×108 block-size-1 RGB output, 24K reference
scaling, stable preview/export coordinates, generated Min/Max including
Continuous hue, and isolation of authored-image values from those generated
limits. Native CPU/Metal parity passed for all six choices. The supplied Test
Fire project rendered as a broad ordered whole-frame progression rather than
shuffled color static. The self-contained macOS verifier inspected 27 Mach-O
files; embedded `pvt-render` reported 1.2.5 and passed self-test, deep strict
signing and native Cocoa smoke passed, and both executables were arm64 with a
macOS 13.0 minimum. The workflow-shaped single-root archive passed ZIP
integrity, SHA-256, layout, version, self-test, and signature verification.

The 2026-08-16 1.2.3 correction passed the complete Qt-enabled Release suite
21/21, including real-Metal generated-source, image/palette/dither, particle,
path, blur, custom-OBJ split-stage, cancellation, and CPU parity coverage plus
the native Cocoa save-tab regression. The independent C++20 suite and the
AddressSanitizer plus UndefinedBehaviorSanitizer suite each passed 20/20; leak
detection alone was disabled on this macOS beta host. Clang 22 static analysis
reported no project-owned findings; all 24 reports were confined to the pinned
vendored miniaudio amalgamation. The supplied Test Fire project rendered two
byte-identical 1920×1080 frames through the packaged GPU renderer, with all
2,073,600 exported RGB tuples distinct. The self-contained macOS distribution
verifier passed over 27 Mach-O files; its embedded `pvt-render` reported 1.2.3
and passed self-test, deep strict signing and native Cocoa smoke passed, and the
workflow-shaped archive passed checksum, integrity, architecture, deployment-
target, and single-root layout checks. The release workflow YAML and
`git diff --check` passed.

The 2026-08-15 1.1.5 correctness patch passed the Qt-enabled Release suite
21/21, including a Cocoa GUI regression that replaces a saved project and
checks the empty new-document version list, selectors, summary, diff text, and
disabled actions. The self-contained macOS distribution verifier passed over
27 Mach-O files; embedded `pvt-render` reported `1.1.5` and passed self-test,
deep strict code-sign verification passed, the staged app passed Cocoa smoke,
and the workflow-shaped archive contained only the package directory with the
application, README, and license. `git diff --check` passed.

The 2026-08-15 1.1.4 performance patch passed the fresh Qt-enabled Release
suite 21/21, the independent C++20 suite 20/20, and the AddressSanitizer plus
UndefinedBehaviorSanitizer suite 20/20; leak detection alone was disabled on
this macOS beta host. On the supplied 41 MiB, 20-version Potato Fire directory,
the released 1.1.3 CLI measured about 5.3 seconds for Open, 11.6 seconds for an
unchanged open/save cycle, and 13.2 seconds after a real change; 1.1.4 measured
about 1.05, 1.77, and 3.38 seconds respectively. The self-contained macOS
distribution verifier passed over 27 Mach-O files; embedded `pvt-render`
reported `1.1.4` and passed self-test, deep strict code-sign verification
passed, the staged app passed native Cocoa smoke, and `git diff --check` passed.

The 2026-08-14 1.1.3 persistence patch passed the complete Qt-enabled Release
suite 21/21, the independent C++20 suite 20/20, and the AddressSanitizer plus
UndefinedBehaviorSanitizer suite 20/20; leak detection alone was disabled on
this macOS beta host. The focused bundle regression verifies transient lock
cleanup, one physical attachment per SHA-256 identity, shared layer-analysis
references, legacy migration, and directory/ZIP round trips. The untouched
18-version Potato Fire directory loaded in 10.8 seconds versus a 28.1-second
baseline. An explicit Save on a clone retained all 18 loadable versions,
reduced 419 MiB to 41 MiB, and produced an approximately 4.0-second load. The
self-contained macOS distribution verifier passed over 27 Mach-O files;
embedded `pvt-render` reported `1.1.3` and passed self-test, deep strict
code-sign verification passed, the staged app passed Cocoa smoke, and
`git diff --check` passed.

The 2026-08-14 1.1.2 patch passed the Qt-enabled Release, C++20, and
AddressSanitizer plus UndefinedBehaviorSanitizer suites at 21/21 each; leak
detection alone was disabled because this macOS beta does not support it. A
focused Clang 22 static-analysis build reported 0 bugs. Strict GPU rendering of
the supplied 10-layer Potato Fire project completed 242 consecutive 1024 x 1024
frames before the bounded probe cancelled the next frame, while its CPU use fell from the
reported roughly eleven cores to roughly one core. The self-contained macOS
distribution verifier passed over 27 Mach-O files; embedded `pvt-render`
reported `1.1.2` and passed self-test, deep strict code-sign verification and
Cocoa smoke passed, and the workflow-shaped ZIP passed integrity and layout
inspection.

The 2026-08-14 1.1.1 patch passed the complete Qt-enabled Release suite 21/21,
including the new starting-image chooser reachability regression and native
Cocoa GUI smoke. The host's macOS 27 beta SDK emitted `@rpath/libc++.1.dylib`;
normalizing that beta-only load command to the system libc++ allowed the fresh
build to execute. GitHub's native package matrix remains the release gate.

The 2026-08-14 1.1.0 feature pass passed the Qt-enabled Release suite
21/21, the C++20 compatibility suite 20/20, and the AddressSanitizer plus
UndefinedBehaviorSanitizer suite 20/20. Clang static analysis produced no
project-owned reports; its 24 reports were all confined to the pinned vendored
`third_party/miniaudio/miniaudio.c` amalgamation. The self-contained macOS
distribution verifier passed over 27 Mach-O files; its embedded `pvt-render`
reported `1.1.0` and passed self-test, deep strict code-sign verification
passed, and the staged app passed native Cocoa smoke. A workflow-shaped local
ZIP contained only the app, README, and license beneath its package root and
passed archive-integrity inspection. The release workflow YAML and
`git diff --check` also passed.

The 2026-08-14 1.0.1 patch passed the Qt-enabled Release suite 21/21,
the C++20 compatibility suite 20/20, and the AddressSanitizer plus
UndefinedBehaviorSanitizer suite 20/20. A clean project-source static-analysis
build reported 0 bugs. The self-contained macOS distribution verifier passed
over 27 Mach-O files; its embedded `pvt-render` reported `1.0.1` and passed
self-test, deep strict code-sign verification passed, and the staged app passed
native Cocoa smoke. The release workflow YAML and `git diff --check` also
passed.

The 2026-08-14 final 1.0.0 audit passed the Qt-enabled Release suite 21/21,
the C++20 compatibility suite 20/20, and the AddressSanitizer plus
UndefinedBehaviorSanitizer suite 20/20. A clean project-source static-analysis
build reported 0 bugs after defensive archive parser cleanup; diagnostics from
the vendored miniaudio amalgamation remain third-party findings. The
self-contained macOS distribution verifier passed over 27 Mach-O files, its
embedded `pvt-render` reported `1.0.0` and passed self-test, deep strict
code-sign verification passed, and the staged app passed native Cocoa smoke.
Release-workflow stable/prerelease classification, YAML parsing, and
`git diff --check` also passed.

The 2026-08-13 RC3 correction passed the focused AddressSanitizer plus
UndefinedBehaviorSanitizer suite 4/4 and the Qt-enabled Release suite in its
required environments. Coverage renders explicit per-wave/per-effect feature
overrides through both CPU and prepared Metal data, round-trips setup v9/layer
v7 records, preserves setup v8/layer v6 force/ignore values, and restores a
floating or hidden Project & Layers panel. Offscreen and native Cocoa GUI smoke
tests passed. The restricted host suite passed 20/21; its only failure was the
ProRes path reporting `Cannot Encode` while macOS encoder services were
sandboxed. The same native-video test passed 1/1 with normal VideoToolbox
access, so all 21 tests have a passing result in their required environment.
The self-contained RC3 distribution verifier passed over 27 Mach-O files, its
embedded `pvt-render` reported `1.0.0-RC3`, deep strict code-sign verification
passed, and the final staged app passed native Cocoa smoke. `git diff --check`
passed.

The 2026-08-12 0.9.0 feature pass passed the complete Qt-enabled Release suite
21/21. New regression coverage renders and round-trips PNG source layers,
checks tile/stretch edge sampling, verifies reusable-path validation,
serialization, tangent following, and exact loop closure, exercises all three
eraser modes, checks the CLI version, and opens/inspects both About PVT and the
motion-path editor in GUI smoke. A fresh core-only configure kept Qt deployment
off; a fresh macOS GUI configure selected it on. `cmake --install` deployed Qt,
the installed GUI passed its Cocoa smoke test, and the installed CLI passed
self-test. `git diff --check` is part of final validation below.

The 2026-08-12 native-video scheduling correction passed the Qt-enabled Release
suite 19/19, focused native-video AddressSanitizer plus UndefinedBehaviorSanitizer
coverage, native and offscreen GUI smoke tests, and the self-contained macOS
distribution verifier over 26 Mach-O files. Lossless movies produced by the
automatic two-worker path and a forced one-worker path were byte-identical;
tests also retain audio, ProRes/HEVC where available, progress/cancellation,
no-clobber, atomic replacement, permission preservation, and worker-bound
coverage. `git diff --check` passed. The representative 8.9x video timing is
recorded above.

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
`pvt-render` build/self-test, CLI CTest 8/8, two-layer composite PNG export, first
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

- Release C++17 library/CLI build, CTest 10/10, and `pvt-render --self-test`.
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

## Post-1.0 / 1.1.x roadmap

1. **Live performance validation and routing:** exercise the integrated sibling
   Live mode with actual audio interfaces, controllers, secondary displays, and
   long venue-style soak tests. Add native Linux/Windows MIDI adapters, then
   optional Syphon/Spout/NDI transports without serializing machine identities
   or making those transports dependencies of the shared renderer.
2. **Snapcraft distribution:** create, test, publish, and debug the Snap in an
   Ubuntu VM. Snap confinement, plugs, desktop integration, and store behavior
   require a real target environment; do not infer success from macOS.
3. **Debian/PPA distribution:** build and test Debian packages and a Launchpad
   PPA from Debian/Ubuntu systems, including clean-install, upgrade, dependency,
   and desktop-entry behavior.
4. **Cross-platform acceleration:** retain explicit CPU, CPU+GPU, and GPU
   policy. Metal remains the broad macOS backend; 5.0.0 adds the first
   Qt-hosted OpenGL stage for analytic Cylinder/Sphere/Cube mapping on Windows
   and Linux, plus flat Plane rotation on Windows. Continue the staged design in
   `PORTABILITY_ROADMAP.md`
   through sources, effects, quantization, and GPU-resident compositing, and
   call the complete backend production-ready only after real Windows/Linux
   Intel, AMD, and NVIDIA stacks pass; Mesa/hosted CI does not prove physical
   driver parity. GLFW may host a future standalone Live output window but does
   not replace the renderer or Qt editor.
5. **Cross-platform native video:** investigate optional GStreamer export on
   Linux. Prefer native Media Foundation on Windows; a later integration may
   prompt for an already-installed FFmpeg executable and link to external
   instructions, but must neither download nor redistribute FFmpeg. Treat codec
   availability, licensing, patents, cancellation, audio sync, alpha, and atomic
   output as explicit policies.
6. **Deeper creative controls:** add more effect types and greater parameter
   control, plus bounded embedded depth-map and normal-map sources that can be
   targeted to all or part of a layer.
7. **Exhaustive GUI automation:** expand beyond the bounded smoke paths to drive
   every layer editor, undo merge boundary, long semantic diff, bit-depth
   transition, progress path, and cancellation race.
