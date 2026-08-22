# Procedural Visualizer Tool

Current product version: **12.0.0**. The version is read from `VERSION` by every
build and appears in the GUI title, About PVT dialog, native application
metadata, library package metadata, and saved-project provenance.

A seamless-loop procedural image renderer with a reusable C++ library, command-line
editor, and optional Qt 6 desktop GUI. A named project can contain a stack of
independently configurable fire layers; each frame is rendered and blended in
linear-light 32-bit floating-point RGBA, then exported as 8/16-bit PNG or full
32-bit FLOAT EXR.

## 12.0.0 ordered finishing and host-aware workflow controls

Post Effects is now an artist-orderable eight-stage finishing pipeline:
combined RGB inversion, individual Red/Green/Blue/Alpha inversions,
simultaneous RGBA channel routing, edge antialiasing, and quantization. Every
output channel can read Red, Green, Blue, Alpha, Zero, or One from the same
incoming pixel, which makes replacement, duplication, clearing, filling, and
true swaps deterministic instead of depending on overwrite order. The routing
mix is available to numeric LFOs, while routing enable/source choices are Live
targets. CPU and Metal execute the saved order with matching float-RGBA
semantics; setup format 19 and layer format 17 persist it, and older records
load with the historical order and a disabled identity map.

**Application Settings > Performance** now accepts render-memory budgets as
Automatic, MiB, GiB, or a percentage of detected physical RAM, displays the
resolved budget, and warns when a manual value exceeds installed memory. The
host-adaptive Automatic policy reserves most RAM for the operating system and
other applications. Export pauses editor-preview work by default so current-
frame, sequence, and native-video jobs own the renderer; this remains an
explicit machine-local preference and normal preview/playback resumes when the
job ends.

New documents open on the Project page with the project name focused and
selected, including after **New Project**. Saved templates still generate fresh
project/layer identities and empty history. The expanded public by-value
`PostProcessConfig` advances the product major and shared-library SONAME
together to 12.0.0/12; installed consumers must rebuild.

## 11.0.2 deterministic release validation

Verified no-change Save smoke coverage now checks durable project state: the
project remains clean and no bundle version is appended or selected. It no
longer requires the status bar to retain “No changes,” because a concurrent
preview may legitimately replace that transient message with “Rendering
preview…”. GUI smoke failures are also written to standard error on Windows,
where Qt otherwise sends GUI diagnostics only to the debugger. This repairs
the macOS ARM64 and Windows ARM64 release gates without changing product
behavior, public ABI/SONAME 11, setup format 18, or layer format 16.

## 11.0.1 cross-platform release repair

The first release-repair attempt made GUI smoke validation wait for the
background Save completion handler before inspecting dirty state and the
title. The 11.0.1 tag did not publish because its remaining status-text
assertion could still race preview progress; 11.0.2 supersedes it.

## 11.0.0 project location and channel inversion controls

The File menu now includes **Show Project in File Browser**. Unpacked project
folders open directly. ZIP/file projects are revealed and selected through
Finder on macOS, Explorer on Windows, or the desktop-neutral
`org.freedesktop.FileManager1` service on Linux/Ubuntu; Linux falls back to
opening the containing folder when selection is unavailable. Saved projects
and imported legacy setups also show their absolute file/folder path before the
project name and PVT version in the title bar.

Post Effects adds independent **Invert red**, **Invert green**, and **Invert
blue** stages, each with its own 0-to-1 mix. The existing all-RGB and alpha
inversions remain available. All-RGB inversion runs first and each channel
stage runs afterward, so enabling both at full strength intentionally applies a
double inversion to that channel. CPU and Metal rendering, the desktop editor,
interactive CLI, numeric LFO/Live target registries, persistence, migration,
and parity coverage share the same order and defaults.

Persistence advances to setup format 18 and layer format 16. Older formats
load with the new channel stages disabled and full-strength mixes ready for
deliberate enabling. Adding these fields to the public by-value
`PostProcessConfig` changes the installed library ABI, so version 11.0.0 also
advances the shared-library SONAME to 11; installed consumers must rebuild.

## 10.0.3 macOS video and secondary-display recovery

Native HEVC-with-alpha export no longer supplies the redundant
`PreserveAlphaChannel` encoder property that macOS 27 rejects. The codec still
preserves straight alpha and retains the authored HEVC data rate and dedicated
alpha-quality settings. The native export regression test now covers HEVC with
transparency and the High Quality preset.

Stopping full-screen Live Preview Output now exits Qt's native full-screen
desktop before hiding the stage window. A selected secondary display therefore
returns to its normal desktop instead of remaining on a black output Space.
The Cocoa smoke test enforces the full-screen state-change-before-hide ordering
and still covers a subsequent windowed restart. The public ABI, SONAME 10,
project/setup/layer formats, and bundle formats are unchanged.

## 10.0.0 GPU-first scheduling and performance controls

The macOS CPU + GPU scheduler is now GPU-primary: every Metal-supported layer
stays on Metal, while bounded CPU workers handle only genuinely unsupported
independent layers. CPU rendering no longer stops at two busy cores; independent
project layers use a host-adaptive worker pool with authored-order compositing,
cooperative cancellation, and a shared host-memory admission gate. Image and
native-video exports divide automatic CPU and memory capacity across their
actual outer frame workers, avoiding nested thread-pool oversubscription.

On the attached six-layer 512×512 project, the same 12-frame CLI workload
(`--workers 1`, including project load and PNG encoding) changed from 6.18 s to
3.17 s in CPU + GPU mode; strict GPU was 3.23 s before and 3.25 s after. CPU
changed from 6.73 s to 4.34 s, and the hybrid PNGs were byte-identical to strict
GPU. With the project already loaded, automatic CPU layer scheduling reached
6.682 fps versus 1.434 fps with one layer worker (4.66×).

**Application Settings > Performance** is machine-local and defaults to
**Automatic (GPU-first)**, falling back to CPU only when no usable accelerator
is available. It also exposes safe 0=Auto limits for preview/LIVE/still CPU
workers, concurrent export frames, CPU layer workers per export frame, Metal
frames in flight, and render memory. Serializing outer export frames therefore
does not silently single-thread the independent layers inside each frame.
The Metal admission control is disabled with an explanatory effective value of
one when the platform is using Qt's serialized OpenGL context.
Working color remains native linear float32 RGBA; simulated 1/2/4-bit buffers
would add quantization work and reduce quality, so output bit depth remains the
honest project-local precision control.

Starting either realtime output now cancels queued and in-flight editor preview
work. Stopping GO LIVE restores export availability and editor preview, while
stage/companion dismissal clears stale full-screen state and returns focus to
the appropriate window. Project identity and project navigation now live in the
Project page; the compatible saved-layout dock is visibly named **Layers &
Groups** and contains only layer/group work.

`FrameRenderOptions` gains CPU-worker and host-memory fields. Version 10.0.0
therefore advances the product major and installed shared-library SONAME to 10;
installed clients must rebuild against the new ABI.

## 9.0.0 numeric LFOs and universal GPU admission

The editor now exposes **LFOs…** in the project toolbar and Settings menu.
Each layer can drive any supported numeric render value between an authored
minimum and maximum using Sine, Triangle, Smooth pulse, or Bounce, with whole
cycles per loop, phase, and pulse shape. Targets use stable wave/swing/effect
IDs, survive reordering, participate in undo, and persist in setup format 17
and layer format 15. The underlying numeric field remains the fallback value;
the oscillator is materialized only into each rendered frame.

The backend choice is now simply **GPU**. On Windows and Linux, every valid
layer is admitted and the completed frame passes through the OpenGL renderer,
including starting images, alternate generated-color modes, palettes, Custom
OBJ work, and projects with no active analytic surface. Dependency-ordered
stages can still run on the reference lane inside that GPU-owned frame, but
selecting GPU no longer makes a saved project invalid. Runtime context or
shader failures remain visible and are never hidden by a whole-frame CPU retry.

Setup persistence advances to format 17 and layer records to format 15;
existing projects remain migration inputs with no LFOs enabled by default.
Because `ParameterLfo` extends the public by-value configuration layout, the
product major and installed shared-library SONAME advance together to 9.0.0/9.
Installed-library clients must rebuild against the new ABI.

## 8.0.3 portable generated-layer acceleration

Windows and Linux now execute ordinary Continuous hue generated layers in a
real offscreen OpenGL 3.3 fragment pass. Wave evaluation, spatial swings,
displacement, slope lighting, spiral/wall signals, generated RGB ranges, audio
hue response, and procedural alpha are covered; supported surface mapping can
run in the same accelerated frame. CPU + GPU keeps unsupported ordered stages
on bounded CPU lanes, but never disguises a failure after an OpenGL stage has
been admitted.

**GPU** remains a first-class editor, CLI, and library choice on every
platform. A no-surface default layer succeeds through the generated-source
shader, and runtime context/shader failures remain actionable errors with no
whole-frame CPU retry. Preferences are preserved instead of migrated. The CLI
also pumps Qt graphics events while its
sequence coordinator runs, so drivers that require the OpenGL context to stay
on the GUI thread accelerate instead of deadlocking.

## 8.0.2 Windows GPU selection correction (superseded)

8.0.2 renamed the hybrid choice but incorrectly treated GPU as a UI
selection problem, hiding it in the Windows/Linux editor and migrating saved
preferences. 8.0.3 restores that first-class backend and fixes the actual
missing generated-layer GPU stage.

## 8.0.1 Linux OpenGL surface correction

Displaced Plane surfaces now use a dedicated cached-mesh OpenGL raster path on
Linux and Windows instead of entering the analytic shader and silently omitting
their height map. CPU + GPU and strict GPU modes both preserve the authored
mesh, projection, XYZ transform, lighting, outside-fill, and straight-alpha
composition without collapsing performance to CPU rasterization. Flat Plane
transforms remain accelerated as well.

Linux OpenGL parity tests now compare straight-alpha images by their composited
color and alpha, so unobservable RGB stored under fully transparent pixels does
not fail a package. A separate opaque fixture retains strict raw-float parity
coverage for every accelerated analytic surface.

Live audio and MIDI clocks now apply the authored **Between pulses** choice on
every realtime frame. Hold, Linear, and Smoothstep therefore change the active
Live beat motion immediately instead of taking effect only after Live stops;
Mic routes also keep the control reachable when their deterministic offline
fallback is Default or an inactive layer clock.

Incremental CMake builds now watch the canonical `VERSION` file and reconfigure
before relinking, preventing a version bump from leaving stale application or
shared-library metadata in an otherwise current build tree.

## 8.0.0 explicit surface composition

Surface selection no longer invents a camera or composition. A newly enabled
Plane is a pixel-aligned, unrotated, unlit 100% view; changing Plane to
Cylinder, Sphere, Cube, or Custom OBJ preserves every authored view value.
Formerly hard-coded perspective, three-quarter tilt, initial turn, visible
scale, sphere radius, outside fill, light direction, ambient/diffuse split,
rear-surface compositing, and OBJ normalization are now ordinary portable
project settings.

The surface editor and CLI expose orthographic/perspective projection, Contain,
Cover, Stretch, and short-side sizing, visible size, independent XYZ scale and
position, starting rotation and integer loop turns on all three axes, all six
Euler rotation orders, camera distance, focal length, outside-surface behavior,
curvature, lighting amount,
light direction, ambient and diffuse levels, transparent rear-surface
compositing, and OBJ normalization. Neutral front and Classic three-quarter
buttons are explicit presets with Undo support; they do not run when a surface
type is selected. The same representable controls are available to Live
mappings. CPU, Metal, and Qt OpenGL analytic surfaces share these semantics;
ordered Custom OBJ and displaced-Plane mesh rasterization keeps the same
explicit model.

Setup formats 1 through 15 and layer formats 1 through 13 migrate the old
implicit presentation into explicit values, preserving their established view
without carrying hidden behavior into new work. Portable persistence advances
to setup format 16 and layer format 14. Because `SurfaceConfig` is a public
by-value structure, version 8.0.0 advances the installed shared library to
SONAME 8 and installed-library clients must rebuild.

## 7.0.2 Windows ARM64 package correction

Windows ARM64 deployment now makes Qt's x64 host tools query the ARM64 target
installation through its `qtpaths` wrapper. The target and companion host Qt
installations share the layout expected by that wrapper, preventing
`windeployqt` from placing x64 Qt DLLs beside the ARM64 application. Package
validation checks the PE machine type of the application, Qt Core, and both
platform plugins before launching smoke tests, so an architecture mismatch is
reported directly instead of appearing as a hung application. The packaged GUI
smoke test is hardware-independent and never opens an OpenGL capability probe,
preventing headless display drivers from blocking release validation while the
normal application continues to report and use real accelerator capabilities.

## 7.0.1 high-precision image input and cross-platform reliability

Height maps and layer starting images now accept both PNG and OpenEXR. PNG data
maps preserve their native 8- or 16-bit sample codes without applying a display
transfer function; OpenEXR scanline images retain 16-bit HALF or 32-bit FLOAT
channels in the renderer's float32 linear pipeline. Single-channel EXR height
maps are supported directly, while RGB/RGBA inputs remain available for both
data maps and artwork. Palette-image import also accepts HALF or FLOAT EXR.
Supported EXR compression is NONE, RLE, ZIPS, and ZIP.

Windows and Linux surface acceleration now negotiate several public Qt OpenGL
3.3 profiles against the actual context format before creating the offscreen
surface. Drivers that provide hardware OpenGL without threaded-context support
can render on the GUI thread instead of losing acceleration. Independent project
layers use two bounded CPU lanes for CPU-only stages and fallback scenes, which
keeps preview and export responsive when only part of a composition is portable
to OpenGL.

Numeric editors no longer replace zero with phrases such as `Whole layer (0)`;
the meaning stays in the adjacent label so zero remains ordinary editable text.
Unfocused spin boxes and selectors forward precision-touchpad wheel gestures to
their containing scroll area, preventing two-finger navigation from moving
focus or changing values. The Ubuntu ARM64 Snap build also gains the missing
standard-library include required by GCC 13. Randomized layer values and mixes
are now retried against the real canvas/workload validator, eliminating a rare
unsafe particle combination.

## 7.0.0 Mic clocks and Live Preview Output

The standard project and active-layer clock selectors now include **Mic
(Live)...**. Choosing it opens a detected-device selector and the existing Live
audio analysis/calibration surface, creates a portable logical Audio role and
beat-clock route, keeps the host device binding and measured device correction
machine-local, and starts the performance workspace. Project and layer Mic
clocks share the one physical capture role supported by the current runtime;
offline rendering and `pvt-render` remain deterministic because the underlying
authored clock is preserved instead of adding a hardware-only core clock mode.

Live beat routing now uses the capture analyzer's continuous beat position, so
detected or tapped tempo changes its rate instead of algebraically cancelling
out. Named frequency streams use their own onset/tempo state. Device identity is
duplicate-safe, missing saved devices remain visibly unavailable rather than
falling back silently, calibration separates portable phase intent from local
device latency, and dropout/last-good behavior no longer mistakes a deterministic
fallback frame for recovered audio.

**Live Preview Output** is a general presentation output for the normal editor
preview (including Preview Solo). It streams to a selected display at Auto,
Full, 75%, 50%, or 25% render scale through the selected CPU/Metal policy,
without starting audio, MIDI, OSC, scenes, mappings, or sleep prevention.
Presentation and performance Live share display/quality preferences while
remaining mutually exclusive with frame/video export. Rendering is
device-pixel-ratio aware, revision-gated, and coalesces to one in-flight plus the
latest pending frame so slow rendering does not starve output indefinitely.

## 7.0.0 particle authoring expansion

Particle Field now has two explicit rendering profiles. **Legacy Glow** is the
compatibility profile used by setup formats through 14 and layer formats
through 12, preserving their prior pixels and defaults. Newly created particle
effects use **Defined**: larger antialiased Spark, Soft Orb, Ring, Diamond, and
Star silhouettes whose shape remains readable at full output resolution.

The effect editor provides a prominent logarithmic **Particle size** scale plus
an exact base radius in output pixels. Artists can independently vary size,
shape definition, twinkle, deterministic seed, orientation, and rotation; a
**Surprise me - reseed** action makes quick variations playful without changing
the particle count or other authored controls. Follow-motion orientation uses
the actual loop path, while stationary Defined fields avoid inventing a fake
directional trail. The CLI exposes every control; the Live mapping registry
exposes count, size, shape, profile, variation, definition, twinkle,
orientation, and rotation (the stable Live `radius` path remains unchanged).
Seed stays editor/CLI-only because Live targets use doubles and therefore cannot
preserve every unsigned 64-bit seed exactly.

CPU and Metal use matching deterministic silhouettes and conservative bounds.
Validation admits only a checked, canvas-aware aggregate particle stamp budget,
and Metal admission also accounts for particle point, tile-grid, and tile-index
buffers retained through command completion. Cancellation is checked within
particle generation and raster loops. A formerly valid legacy file whose active
particle workload exceeds the new safety bound still loads transactionally: the
effect is disabled, its authored values and prior enabled state are preserved in
a non-applying compatibility record, and a recovery note explains how to make
it safe. The disabled state remains authoritative even after the artist reduces
the workload; re-enabling is always deliberate.

Portable persistence advances to setup format 15 and layer format 13. The new
fields append to the public by-value `EffectConfig`, so version 7.0.0 advances
the installed shared library to SONAME 7. Existing projects remain migration
inputs, while installed-library clients must rebuild against the new ABI.

## 6.0.1 automatic Live companion window

Choosing **LIVE** now opens the performance controls immediately in their own
window while leaving the complete editor visible and usable. The Live workspace
is explicitly reparented and shown after leaving its internal stack, fixing the
blank detached window introduced in 6.0.0. **Edit Project** brings the editor
forward without stopping input, rendering, or stage output; closing the Live
window stops the runtime, releases sleep prevention, and allows it to reopen
cleanly. No prompt or intermediate in-editor Live screen is used.

This patch does not change the public API, SONAME 6 ABI, setup format, layer
format, or project-bundle format.

## 6.0.0 audio routing and creative-control expansion

Music and Live audio can now pass through an optional high-pass filter, optional
low-pass filter, and a graphical ten-band parametric equalizer before any beat,
tempo, onset, spectral, or tonal analysis. Artists can add stable, named frequency
ranges such as **Kick**, **Voice**, or **Cymbals**; each range receives its own
causal or offline analysis and can independently drive the project clock or an
active-layer clock. The full-band signal remains the default, all processing is
off/flat by default, and edits to file analysis are transactional.

Live setup now creates an audio-beat project clock when an audio-input role makes
that route unambiguous. Its mapping target browser is searchable and grouped by
project, layer, wave, Swing, and effect instead of opening one enormous combo box.
Live mode can detach into its own window, optionally holds the system awake while
active on supported platforms, and publishes its routed frames to the editor
preview so authoring and program output use the same clocks.

The effect catalog adds seamless **Edge Detect** and **Twirl** stages with CPU and
Metal parity. Particle Field adds Spark, Soft Orb, Ring, Diamond, and Star
procedural shapes without introducing an external-asset dependency. Custom PNG
particle sprites remain a possible asset-backed follow-up rather than being
silently embedded into the portable procedural format.

These additions advance legacy setup persistence to format 14 and project layer
records to format 12. Older formats retain their historical flat/full-band audio,
clock, effect, and particle defaults on import. Because the additions grow
exported by-value configuration structures, version 6.0.0 advances the installed
shared library to SONAME 6; installed-library clients must rebuild against it.

## 5.0.3 unrestricted live authoring

Version 5.0.3 keeps Live input, rendering, clocks, and stage output running
while the full project editor is open. **Edit Project** changes the workspace,
not the runtime state, and every live frame obtains the current project
snapshot, so ordinary UI edits become visible just like MIDI/OSC changes.

The accompanying artificial-limit audit removes round-number policy ceilings
from renderer fields, editor widgets, Live mappings/safety timings, audio input
buffers, recent-project history, and one-shot layer clocks.
Normalized values and format/protocol/representation boundaries remain real
constraints. Per-item audio response is now independent of clock
synchronization unless the artist explicitly enables the profile's
**synchronized items only** policy; a one-shot local source longer than the
project is valid and simply shows the available prefix.

## 5.0.2 wave-output reachability

Version 5.0.2 makes enabled waves useful and understandable again when their
layer outputs are disabled. The Wave page now exposes generated-pattern
displacement and wave-slope lighting directly, keeps those switches synchronized
with Modifiers, and explains when an enabled wave cannot affect pixels because
both outputs are off. GUI smoke coverage protects that state and no longer lets
a persisted strict-GPU preference make otherwise portable editor checks depend
on the test machine's GPU.

## 5.0.1 cross-platform release reliability

Version 5.0.1 makes the 5.0 renderer and packages reliable on their actual
release platforms. The CLI initializes its Qt/OpenGL service on the main thread
before worker rendering and releases render-thread contexts before Qt platform
shutdown, avoiding headless export deadlocks. Ubuntu's minizip-ng 4 API now
receives the mutable, lifetime-stable input buffer its reader contract requires,
repairing the Launchpad binary-package build.

Portable GPU coverage follows tested platform capability: Windows accelerates
flat Plane rotation plus Cylinder, Sphere, and Cube; Linux accelerates the three
curved 3D primitives and keeps flat Plane rotation on CPU after Mesa exposed
driver-dependent straight-RGB sampling. Displacement Plane and imported OBJ
meshes remain ordered CPU raster stages on both platforms. macOS retains its
broader Metal pipeline.

## 5.0.0 displacement Plane and closed Cylinder

The built-in **Cylinder** is now a perspective-projected, closed 3D primitive
with a rounded silhouette, textured side, visible top/bottom caps, lighting,
and front/rear straight-alpha compositing. Its CPU and Metal implementations
share the same ray intersection, UV, phase, and tilt semantics; selecting
Cylinder no longer produces a rectangular box mask.

The built-in **Plane** can now generate real height-field geometry from an
embedded 8/16-bit PNG or 16-bit HALF/32-bit FLOAT OpenEXR image. Raw linear
sample luminance below and above an artist-selected
neutral midpoint maps independently to signed minimum and maximum displacement.
**Pixel-to-node ratio** controls mesh density: `1` creates one vertex per render
pixel, while larger values reduce geometry and always retain both outer edges.
PVT lazily builds and caches a separate mesh for each effective render
resolution, so the scaled editor preview and adaptive Live monitor use lower
resolution geometry while full-resolution frames, video, and Live output use
the authored/output resolution. Changing the map, range, midpoint, ratio, or
effective resolution selects or rebuilds the matching mesh. The GUI can export
the authored-output mesh as Wavefront OBJ with positions, UVs, normals, and
triangles.

The height map is a content-addressed layer attachment and follows the same
transactional bundle, duplication, history, and stale-writer protections as
starting images and custom OBJs. Strict GPU rendering keeps Metal active around
the ordered CPU mesh-raster stage instead of repeating the whole layer on CPU.
Setup persistence advances to format 13 and layer records to format 11. The new
public `PlaneDisplacementConfig` member grows by-value configuration types, so
5.0.0 advances the shared-library ABI to SONAME 5; installed C++ clients must
rebuild. Older setups and project bundles remain migration inputs with plane
displacement disabled by default.

Windows and Linux product builds now include the first portable GPU stage: a
public-Qt, offscreen OpenGL 3.3 renderer for the built-in analytic Cylinder,
Sphere, and Cube mappings, plus flat and displaced Plane mappings. CPU + GPU
activates it when a suitable
runtime context exists. The desktop editor names this mode GPU acceleration and
does not offer its surface-only strict diagnostic mode as a general Windows or
Linux renderer; an old persisted strict selection is migrated automatically.
The CLI and library retain strict GPU for diagnostics, where it requires a
supported active surface and reports context, shader, or imported-OBJ
limitations directly. The rest of the frame and final project compositing
remain on the reference CPU path, while macOS continues to prefer its broader
Metal pipeline.
`RendererCapabilities` reports compiled/runtime status and the actual OpenGL
renderer instead of guessing from the operating-system name. This is genuine 3D
surface acceleration, not yet a claim of full portable pixel-pipeline coverage
or qualification on every vendor driver.

Tall pop-out settings are screen-aware in 5.0.0. Application Settings and Video
Export cap their initial size to the active monitor's available work area, keep
their action buttons visible, and scroll the content region when display size or
UI scaling cannot fit the natural layout.

## 4.0.1 generated alpha reliability

Version 4.0.1 fixes generated alpha on the built-in first layer and in existing
projects created from that template. **Include alpha as a generated color
dimension** is now authoritative for generated sources; the separate source-
alpha switch applies only to starting palettes and PNG pixels. This removes a
hidden two-control dependency while preserving the ability to ignore authored
image/palette alpha non-destructively.

The CPU renderer, Metal renderer, project transparency analysis, export safety,
Live target labels, CLI wording, and GUI defaults now share that contract.
Regression coverage includes the reported saved-state combination—generated
alpha enabled while palette/image alpha is disabled—and verifies real layer
compositing as well as CPU/Metal parity. This patch does not change public
structure layout, SONAME 4, setup format 12, or project-bundle formats.

## 4.0.0 Live performance and final post effects

Version 4.0.0 adds an artist-facing sibling **Live mode** with low-latency
audio capture and causal analysis, CoreMIDI and OSC control, MIDI Learn,
portable foot-controller mappings, independent project/layer live clocks,
24-PPQN MIDI clock outputs, scenes, secondary-display program output, freeze,
blackout, last-good-frame safety, and an active frame-time watchdog. Logical
roles, mappings, scenes, signed latency compensation, clock routes, and safety
preferences travel with a project; device IDs, network bindings, screen
identities, captured samples, current scene, freeze, and blackout remain local
or ephemeral.

The editor now uses a cohesive dark studio theme with resolution-independent
knobs, meters, and status lamps, and wrapped help text correctly expands instead
of being clipped. Layer-local Post Effects add independently mixed RGB
inversion, alpha inversion, and premultiplied edge antialiasing before final
quantization on both CPU and Metal.

Setup persistence advances to format 12. Public by-value renderer and project
configuration types grew, so 4.0.0 advances the shared-library ABI to SONAME 4
and installed clients must rebuild. Existing setup files and project bundles
remain supported migration inputs. CoreMIDI is the first native MIDI backend;
other platforms currently expose the portable model and UI with a clear runtime
stub while native adapters are developed. Syphon, Spout, and NDI remain optional
follow-up transports rather than renderer dependencies.

## 3.0.0 Flow Workbench and procedural expansion

Version 3.0.0 reorganizes the Qt editor around seven purpose-based workspaces:
**Project, Starting Colors, Modifiers, Movement, Layer Effects, Post Effects,
and Export**. Effects have separate Movement & Distortion, Light & Energy,
Stylize, Particles, and Blur catalogs, so adding more algorithms does not create
one endless menu. Every new or randomized effect starts on Texture; mapped-
surface placement remains an advanced per-effect option. A compact collapsible
Synchronization strip summarizes project clock, active-layer clock, Swing, and
audio routing without permanently consuming editor space. Project Canvas &
Loop, Synchronization & Audio, Export, and History are also directly reachable
from the Project page.

This release also adds opt-in project/layer clock mixing, Kaleidoscope and seamless
Domain Warp generated-source shaping, and Glitch, Starburst, and Lens Distortion
effects with CPU/Metal implementations. Clock mixing is **off for every old and
new project**. With an active layer clock and mixing off, the historical rule is
unchanged: the layer clock replaces the project clock. Enabling the advanced
switch exposes Replace, Add, Difference, Soft XOR, and exact 24-bit XOR.

Starting palettes now import and export GIMP GPL, Krita KPL, GIMP-style CSS,
Python, PHP, Java and hex text, plus PNG and HALF/FLOAT OpenEXR palette images. Image
pixels are traversed top-to-bottom and left-to-right; fully transparent pixels
are ignored, exact decoded duplicates keep only their first occurrence, and a
review summary is shown before Replace or Append. Entry names, grid columns,
alpha, and sRGB versus linear/HDR encoding are retained whenever the chosen
format can represent them; export summaries identify any name, alpha, encoding,
or precision loss.

These changes grow installed by-value renderer configuration types, so 3.0.0
advances the shared-library ABI to SONAME 3 and requires installed clients to
rebuild. Existing setup and project data remain compatible migration inputs.

## 2.0.1 correctness and security hardening

Version 2.0.1 widens arithmetic operands before buffer-size, frame-count,
sample-offset, cache-size, filter, and chroma calculations are converted to a
larger result type. This resolves all 34 CodeQL multiplication-width findings
in the project and its pinned miniaudio and Beat-and-Tempo-Tracking sources.
The findings represented correctness and overflow-hardening opportunities, not
34 independently confirmed exploits.

Review-only patches for both upstream projects are included under
`patches/upstream/`; they have not been submitted upstream. This patch release
does not change the public API, SONAME 2 ABI, setup format, or project-bundle
format.

## 2.0 compatibility note

Version 2.0 advances the shared-library SONAME to 2. Applications that link
`libProceduralVisualizerTool` must be recompiled because public by-value
configuration types grew during the 1.x line. Existing `.pvt` setups and ZIP or
unpacked project bundles remain loadable; this is an ABI and rendering-semantics
boundary, not a project-data reset.

Two corrected motion rules can intentionally change phase-zero placement in
existing artwork: layer Starting phase is now added independently after the
cycle multiplier, and a reusable path's Reverse switch reverses travel without
negating its separately authored starting phase. Projects that used non-unit
motion cycles with a nonzero Starting phase, or Reverse with a nonzero binding
phase, should be reviewed once after opening in 2.0.

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
- Qt 6.5 or newer with Gui, Widgets, Concurrent, and Network components for the
  optional GUI, its bounded OSC listener, and the Windows/Linux offscreen
  OpenGL generated-source/surface accelerator.
- On Apple platforms, the optional Metal backend uses Apple's header-only
  [metal-cpp](https://developer.apple.com/metal/cpp/) from
  `../3rd_party/metal-cpp` plus the system Foundation and Metal frameworks. Set
  `PVT_ENABLE_METAL=OFF` for an explicitly CPU-only Apple build, or point
  `PVT_METAL_CPP_DIR` at another metal-cpp checkout. Windows and Linux Qt product
  builds enable generated-source/surface OpenGL by default; pass
  `PVT_ENABLE_OPENGL_SURFACE=OFF` to build those products without it. Core-only
  library builds remain Qt-free and CPU-only.
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

The default signature is ad-hoc. This project does not publish Developer
ID-signed or notarized packages. Local builders who want to use their own
identity can pass `-DPVT_DISTRIBUTION_CODE_SIGN_IDENTITY="Developer ID
Application: ..."` through `CMAKE_CONFIGURE_ARGS` and complete Apple's
notarization process themselves.
Unless the caller explicitly supplies `CMAKE_OSX_DEPLOYMENT_TARGET`, this path
targets macOS 13. It builds the existing libpng dependency statically from a
SHA-256-pinned upstream archive instead of inheriting a local/Homebrew dylib,
and therefore needs network access on its first distribution build.

### Automated desktop packages

GitHub Actions builds tested desktop packages after every push to `main`, for
every pull-request update, on manual request, and for version tags. Permanent
versioned downloads and their `SHA256SUMS.txt` are published on the
[GitHub Releases page](https://github.com/gnaservicesinc/procedural_visualizer_tool/releases).
The **Build desktop packages** workflow also retains per-commit artifacts for
30 days:

- `procedural-visualizer-tool-macos-arm64.zip` supports Apple Silicon Macs on
  macOS 13 or newer.
- `procedural-visualizer-tool-windows-x86_64.zip` is a portable 64-bit Windows
  directory.
- `procedural-visualizer-tool-windows-arm64.zip` is a native Windows on ARM
  directory.
- `procedural-visualizer-tool-linux-x86_64.tar.gz` and
  `procedural-visualizer-tool-linux-arm64.tar.gz` are native Linux directories
  built and tested on Ubuntu 24.04.

Each package contains the Qt GUI, the optional `pvt-render` command-line
renderer, licenses, and documentation. On macOS, the renderer is kept inside
`Procedural Visualizer Tool.app/Contents/MacOS/` so the archive presents one
obvious application instead of a stray historical executable. Linux and
Windows install it under `bin/`. As an intentional project policy, the macOS
app uses an ad-hoc signature and is not notarized, and the Windows executable is
not Authenticode-signed. Users can approve the downloaded application through
their platform's security controls, or build from source and sign the result
locally. The workflow does not require or expect commercial signing
credentials.

### Ubuntu PPA and Snap Store packages

Install published PPA builds with:

```sh
sudo add-apt-repository ppa:gnaservicesinc/proceduralvisualizertool
sudo apt update
sudo apt install procedural-visualizer-tool
```

## Snapcraft
[![procedural-visualizer-tool](https://snapcraft.io/procedural-visualizer-tool/badge.svg)](https://snapcraft.io/procedural-visualizer-tool)


## What is configurable

- A semantic UTF-8 project name used in the window title. Display-safe characters
  such as `:` are allowed; the first Save/Save As derives a portable sanitized
  root and `<project-name>.zip` filename without changing the displayed name.
- Full render layers up to the signed-int index capacity used by the GUI and
  public APIs. Layers have stable UUID/file identities and can
  be named, enabled, duplicated, removed, selected, and reordered. Paint order
  is bottom-to-top; the GUI presents the topmost layer first.
- Flat layer groups act as contiguous folders in the single authoritative paint
  order. A group can contain one or more layers and can be renamed, shown or
  hidden, soloed for preview, locked or unlocked, moved as one block, or removed
  without deleting its artwork. Layers move into and out of groups through the
  selected layer's **Group** control; groups do not nest.
- Per-layer opacity and Normal (`none`), Soft Light, Grain Merge, Overlay,
  Color Dodge, Linear Burn, Color Burn, Difference, Subtract, Multiply, and Add
  blend modes. Three destination-out modes use the current layer as a mask for
  lower layers only: Erase uses source alpha, Color Eraser (tones) matches
  linear-light color distance, and Color Eraser (brightness) removes darker
  backdrop pixels. Layers above remain untouched.
- Per-layer **Alpha Mode** chooses Porter-Duff ordering independently from the
  artistic blend: **Alpha Over** (the default and legacy behavior) paints the
  layer over the accumulated lower stack, while **Alpha Under** places it under
  that stack after applying the layer opacity. Destination-out erasers retain
  their explicit lower-stack masking behavior in either alpha mode.
- Any number of waves from zero through the signed-int UI/API index capacity,
  with add,
  duplicate, remove, enable, and reorder controls.
- Per-wave synchronization, placement, amplitude, spatial frequency, phase,
  cycles per loop, propagation direction, and per-wave audio-feature override.
  **Default** inherits the effective profile; Beat,
  Onset, Energy, Bass, Midrange, Treble, spectral, and tonal sources can be
  selected directly.
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
- Advanced project/layer clock mixing is available only while the active-layer
  clock is enabled and only after its separate opt-in switch is turned on.
  Replace, Add, project-minus-layer Difference, continuous Soft XOR, and exact
  24-bit XOR combine independently transformed project/layer phases before
  Swing. Mixing defaults off in both migrated and newly created projects, so an
  active layer clock retains the historical replace behavior.
- An ordered dynamic effect stack with endless zoom, ripple, shake, flag wave,
  glow, animated block scaling, deterministic antialiased particle fields, Blur,
  scanline/RGB-split Glitch, radial Starburst, and barrel/pincushion Lens
  Distortion, linear-light Sobel Edge Detect, and seamless Twirl distortion.
  Every effect can be enabled, synchronized, duplicated, removed, and reordered.
  An effect can inherit its effective audio category and source,
  override that source with any analyzed feature, force that effect on with the
  profile source even when the category default is off, or ignore audio without
  changing its authored intensity. The effective profile master switch remains
  authoritative.
  Each effect explicitly runs either in
  **Texture** space before surface wrapping or on the **Mapped object** after
  wrapping and the layer mirror/flip; the latter moves or deforms the rendered
  silhouette of a cylinder, sphere, cube, or OBJ in final canvas coordinates
  rather than editing its source 3D geometry. Controls
  include type-specific centers, edges, frequencies, harmonics/attenuation,
  glow bloom parameters, and block scale
  range/mix/quantization steps. Particle fields add an output-pixel size scale,
  exact radius, size variation, definition, twinkle, deterministic reseeding,
  orientation, rotation, trail amount, count, and five distinct silhouettes.
- Per-layer closed motion presets: orbit, figure eight, bounce, and Lissajous,
  with center, horizontal/vertical travel, integer loop cycles, phase, rotation,
  and optional scale pulsing. Starting phase is additive and independent of the
  cycle counts. Projects can also own reusable closed
  cubic paths. The GUI edits stable nodes, explicit in/out handles, Corner,
  Auto Smooth, Smooth, and Symmetric policies, and creates four-node cubic
  ellipse approximations. Independent bindings drive the active layer, wave
  sources, or effect centers with sync/free clocks, integer cycles, phase,
  reverse, offsets, optional tangent following, and bounded arc-length sampling.
  Reverse changes travel direction while retaining the authored starting phase.
- Draggable numbered effect centers in the preview. A local area radius of zero
  preserves whole-layer behavior; a positive radius creates a smoothly
  feathered circle around the center for zoom, ripple, shake, flag wave, and
  glow. Glow's blur radius remains a separate control. Texture-effect and Swing
  overlays use a labelled unwrapped source/UV inset whenever a non-plane or
  displaced Plane surface is active; mapped-object overlays remain final screen
  coordinates.
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
  The project owns a complete default profile. Each layer inherits it until an
  active-layer override is enabled, and **Copy project settings into override**
  provides a non-destructive starting point for layer-specific art direction.
  Energy is the visible default hue route; the other sources remain selectable.
  Pitch-class hue is weighted by tonality confidence, so silence or noise does
  not cause arbitrary palette jumps. Missing or explicit `null` inheritance and
  per-item routing fields resolve to neutral defaults. These controls appear
  only when the selected layer's effective clock—project-wide or overridden
  locally—is Music, because other clocks have no analyzed audio envelope.
  Free waves, effects, and path bindings retain their independent linear loop
  clock even when the synchronized clock uses Hold or another pulse mapping.
  Endless Zoom uses intensity 0–1 as source mix and values above 1 as additional
  zoom depth, keeping positive audio modulation visible at its default full mix.
- An optional starting palette per layer with one or more authored sRGB or
  imported linear/HDR colors
  up to the signed-int UI/API index capacity, custom
  add/edit/remove controls, and six presets: Ember, Deep Ocean, Vaporwave,
  Forest Biolume, Arcade, and Moonlight. The palette chooses exact procedural
  source colors in linear light without changing alpha; lighting and effects
  may create other colors afterward. Presets never silently change
  whether the starting palette is enabled.
  The UI also imports/exports GIMP GPL, Krita KPL, CSS, Python, PHP, Java, hex
  text, PNG, and HALF/FLOAT EXR. Code-looking formats are parsed only as bounded data
  and are never executed. PNG/EXR image palettes import each non-fully-
  transparent pixel in row-major order and remove exact decoded duplicates.
  Import and export summaries make skipped colors and format losses explicit.
- Generated starting colors begin with **Continuous hue** or one of five
  whole-render patterns: **Horizontal rainbow**, **Vertical rainbow**,
  **Diagonal rainbow**, **Spiral rainbow**, or **Square spiral rainbow**.
  Every ordered pattern walks the same automatically sized RGB/RGBA lattice
  without repetition. The lattice is enumerated hue-first around six exact RGB
  gamut sectors, with brightness and saturation variations retained inside
  each sector, so the complete source-color set reads as a large rainbow
  instead of fine RGB-channel static. Spiral applies a circular radial winding;
  Square spiral preserves the nested rectangular-ring pattern. **Random** is
  the separate repeatable color-static choice. The selected output dimensions and block size automatically choose
  enough per-channel working values for every full-resolution block, whether
  the canvas is 512×512, 1920×1080, 24K, or another supported size.
  The removed `Values` controls survive only as ignored compatibility records.
  Every mode inside the Generated starting colors box—including Continuous
  hue and Random—obeys RGB/alpha Min/Max without reducing the renderer's
  float32 processing precision. Authored palettes and embedded images remain
  separate sources and are not remapped by those generated-color limits.
  Preview sampling retains full-resolution coordinates, so resizing the live
  preview cannot rearrange colors and preview/export placement stays identical.
  Optional Kaleidoscope shaping adds mirrored radial segments, rotation, and
  blend amount; deterministic multi-octave Domain Warp adds strength, scale,
  integer loop cycles, and a full 64-bit seed. Both preserve the loop seam and
  bypass authored starting-image pixels.
- An optional embedded 8/16-bit PNG or HALF/FLOAT OpenEXR starting image per layer, with Stretch,
  Contain, Cover, and Tile fitting. The decoder converts directly to the
  float32 linear working image without an 8-bit intermediate. It replaces
  procedural generation; an enabled starting palette may then quantize those
  fitted pixels before effects. Surface mapping, transforms, motion,
  quantization, compositing, and export still run afterward. Files use the validated
  content-addressed attachment store, and an evicting shared decoded-image cache
  avoids repeated work across frames and layers.
- Per-layer horizontal/vertical flips and directional mirror symmetry
  (left-to-right, right-to-left, top-to-bottom, bottom-to-top, or four-way).
- Independent feature toggles for displacement, slope lighting, spiral, and
  wall reflection.
- An artist-orderable eight-stage Post Effects pipeline: combined RGB and
  individual R/G/B/A inversions, simultaneous RGBA channel routing,
  premultiplied edge antialiasing with strength/threshold/pass controls, and
  RGB, luminance, or hue quantization from 2 through the signed-int UI/API
  capacity with adjustable mix. Each mapped output can read R/G/B/A/Zero/One
  from the same input pixel, so duplication and swaps are deterministic. This
  final pipeline remains independent of the starting palette.
- Plane (flat or PNG-displaced), closed ray-cast cylinder, sphere, ray-cast
  cube, and custom Wavefront OBJ mappings.
  Displacement Plane density follows the effective preview/Live/output
  resolution and its pixel-to-node ratio; the GUI exports the generated mesh.
  OBJ files may provide texture coordinates and normals; automatic box UVs and
  geometric normals cover meshes that omit them.
- Independent procedural alpha modulation with minimum/maximum alpha, spatial
  frequency, phase, and cycles per loop.
- 8/16-bit RGB or RGBA PNG and 32-bit FLOAT RGB or RGBA EXR sequence output.
- PNG compression from 0 (off/fastest) through 9 (maximum), with a balanced
  default of 5. EXR output is unaffected.
- Optional deterministic blue-noise-like, ordered Bayer, or Floyd-Steinberg
  dithering for integer PNG output. Dithering is never applied to float EXR.
- Automatic GPU-first, CPU, CPU + GPU, and GPU frame policies. Automatic is the
  recommended machine-local default, while **GPU** remains a strict performance
  and debugging choice. On macOS every Metal-supported project layer remains on
  Metal; bounded CPU workers run only independently unsupported layers, and
  ordered CPU stages such as mesh rasterization stay inside an otherwise
  accelerated layer. On Windows and
  Linux it dispatches ordinary Continuous hue generated layers and supported
  analytic Cylinder/Sphere/Cube and flat/displaced Plane mapping through a
  serialized offscreen OpenGL 3.3 shader service. An admitted GPU-stage failure
  is reported instead of silently repeating that stage on CPU. OpenGL GPU mode
  admits every valid layer and owns the completed frame even when an ordered
  dependency still executes on the reference lane.

The GUI uses seven focused Flow Workbench categories—Project, Starting Colors,
Modifiers, Movement, Layer Effects, Post Effects, and Export—alongside a
topmost-first Layers & Groups dock. Surface mapping is an advanced Modifiers
section rather than a primary workspace. The Layer Effects workspace filters
the single ordered effect stack into five type-based catalogs; Texture versus
mapped-surface placement remains editable on each effect without duplicating
the stack into separate windows.
An Edit/LIVE mode switch keeps those seven authoring categories intact while
opening a purpose-built performance surface. A matte charcoal studio theme,
familiar action icons, teal/amber/red operating states, and original scalable
knob/meter/lamp controls provide an analog-digital instrument feel without
stretching bitmap controls on Retina or UHD displays. Wrapped explanatory text
uses real height-for-width layout, including at narrow widths and enlarged text
sizes.
It includes project naming, per-layer blend,
alpha-order, group, and opacity controls, session-only layer/group **Solo**
preview, draggable center handles for
waves, swings, and centered effects with visible radius rings, ordered
wave/swing/effect editors, palette interchange and transform controls, type-aware effect
controls, a live checkerboard alpha preview, a continuously updating timeline,
and background composite export with cooperative cancellation. The
project Synchronization & Audio page owns both the global Clock and the selected
layer's optional active-layer Clock, Swing, project-wide Audio Response defaults,
and active-layer override. A compact collapsible Synchronization strip makes
effective routing and the clock-mixing opt-in available without obscuring the
current workspace. **Randomize
values** keeps the current layer's stack structure and types while varying its
settings; **Randomize mix** creates a new bounded mix. Both live in the Settings
menu and require confirmation, keeping destructive experiments away from the
main toolbar. File dialogs remember their last usable folder and otherwise
begin in the home folder. Dense editors use consistent spacing, frameless
scrolling, scrollable document tabs, and field-specific tooltips that explain
units, stage order, inheritance, destructive boundaries, and non-obvious ranges.
The Layers & Groups panel can be floated or docked by dragging/double-clicking
its title bar; **View > Restore Layers & Groups Panel** always shows it and
redocks it on the left. Off-screen floating geometry is repaired on launch.
Checkable optional blocks collapse to compact headers when off, keeping the
Synchronization workspace readable without hiding available controls.

Every GUI field edit and structural move participates in session undo/redo.
**Settings > Application Settings…** (also available from the main toolbar)
provides extensible General and Performance pages for program-wide preferences.
**Help > About PVT** shows the product version and GPLv3-or-later/no-warranty
notice, and provides direct Project Website, Report a Bug, and Report a
Vulnerability links through the system browser.
The undo step limit, performance policy, window layout, and dialog locations are
stored with the platform's normal per-user settings service (`QSettings`), so
they persist across projects and relaunches and are never placed inside a
portable project. The General page can also capture the complete current
project as the template for future **New Project** commands or restore the
built-in template. New documents receive fresh project/layer identities, so
using a saved template never aliases histories or assets. They open on the
Project page with the project name focused and selected, encouraging an early
semantic name before the first save. The undo step setting
uses Qt's signed-int command capacity and supports Unlimited. Undo snapshots
grow with available memory; if allocation fails, history is cleared safely while
the document remains correctly dirty.
Saved-version history is separate from session undo.

## Synchronization and seamless loops

The default clock preserves the original behavior: the renderer samples `N`
frames over the half-open interval `[0, 1)` and omits the duplicated endpoint.
Synchronized waves/effects use the shared phase after swing modulation;
unsynchronized items keep their own periodic cycle count and phase. Within a
synchronized item, **Default** follows both the effective project/layer
category and source. Selecting Beat, Energy, or another feature opts the item
in and overrides the source. Advanced choices can force the profile source on
or ignore audio. The profile master and its Synchronized-only policy remain
authoritative safety gates.

The Clock block can instead define calculated pulse/keyframe positions:

- **Frame:** one pulse every validated `N >= 1` frames.
- **Time:** one pulse every `N` milliseconds of animation time, derived from the
  frame index and FPS rather than wall-clock/rendering speed.
- **Meter:** a tempo plus tempo-note unit and a validated meter expression. Simple
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
changing the project timeline or export length whenever advanced mixing is off.
When mixing is explicitly enabled, the independently offset/reversed/fitted
project and layer phases are combined by the selected policy before Swing; the
project remains the sole authority for duration and exported frame count. The
layer clock's duration policy maps the
local source over the project duration: **Smart loop fit** repeats the greatest
whole number of clips that fit and spreads the residual adjustment over that
aggregate; **Straight fit** makes one traversal; **Play once** holds the final
local visual state; **Play once then project** switches visual timing to the
project clock; and **Original-speed loop** repeats unchanged. The one-shot
policies also accept a local source longer than the project: **Play once**
shows the available prefix, while **Play once then project** simply does not
reach its transition before the project ends.

Wave propagation direction is continuous:

- `0.0`: horizontal propagation
- `0.5`: radial/all-directions behavior (the default)
- `1.0`: vertical propagation

Intermediate values blend between radial and the selected axis.

## Music analysis, synchronized playback, and native video

Import is asynchronous, cancellable, and transactional. The analyzer decodes
the full source, applies the authored high-pass, low-pass, and graphical EQ
before every analysis stage, derives sample-accurate duration, tracks time-varying beat and
tempo observations, reconciles them with an offline multiband onset/tempogram
pass, and stores the dense feature track up to signed-int container/API capacity.
It does not reduce a song to one fixed BPM. The source SHA-256, decoded format, channel/sample metadata,
beats, local tempo points, and normalized spectral/pitch features are cached in
the project; rendering never decodes or analyzes the song again.

An optional named-range table splits that filtered signal into independent
analysis streams. Each named range stores its own beat, tempo, onset, spectral,
and tonal results, and Music clocks select the full band or one stream by stable
UUID. Adding, removing, relabeling, or retuning ranges requires explicit
reanalyze; failed or cancelled analysis leaves the authored source and cache
unchanged.

Choosing the first project music source selects the Music clock and enables the
project-wide Audio Response profile once, so every inheriting layer responds
without silently creating layer overrides. The project profile and any explicit
layer override remain authoritative afterward: switching clock modes or
replacing the source does not force either back on. Swings remain governed by their own active-layer
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

**File > Export Current Frame…** renders the timeline's current frame at the
project's full canvas dimensions rather than the scaled live-preview size. It
uses the selected output quality exactly: 8/16-bit PNG or 32-bit FLOAT EXR,
RGB/RGBA, PNG compression, and integer-output dithering. The confirmed save
destination is written transactionally through the same encoder path as a
sequence frame.

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

The CLI defaults to **CPU + GPU**; the GUI's machine-local **Automatic** policy
selects that GPU-primary scheduler when Metal or Qt OpenGL is usable and CPU
otherwise. On macOS, Metal-supported layers are never displaced onto CPU merely
to keep a CPU lane busy. Independent unsupported layers use a bounded,
host-adaptive CPU pool beside a one-thread Metal submission pipeline, while
authored compositing order stays deterministic. Generated source ordering,
source alpha, starting-image palette selection, starting-image fitting,
particles, reusable path resolution, built-in placement, rotation, and scale
all keep Metal active. Floyd-Steinberg source dithering, custom OBJ depth
peeling, and displacement-Plane rasterization are ordered CPU stages inside
the accelerated pipeline rather than whole-layer fallbacks. An unexpected
Metal error is surfaced immediately instead of being hidden behind an
unacceptably slow CPU retry. On Windows and Linux, the Qt-hosted OpenGL service
accelerates ordinary Continuous hue generated layers plus analytic and mesh
surface stages for flat/displaced Plane, Cylinder, Sphere, and Cube. Strict
**GPU** reports unsupported source/stage combinations and never retries an
admitted OpenGL failure on CPU.
Final linear-light project compositing remains on the CPU on every platform. The
installed library's
legacy overloads retain CPU as their compatibility default; callers opt into
acceleration with
`FrameRenderOptions` or `SequenceRenderOptions::frame`.

Metal compiles the embedded shader source once per process, caches its command
queue and compute pipelines, and admits at most two frames by default before
allocating their three shared float-RGBA working buffers. Starting PNGs retain
the bounded decoded cache and are uploaded as an additional read-only source
buffer for GPU fitting. Explicit CPU-worker and GPU-in-flight controls are capped
at 256, a scheduler-safety boundary rather than an authored-project limit.
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
./build/pvt-render --render --workers 0

# Reproducible sequential reference, or an explicit bounded pool
./build/pvt-render --render --workers 1
./build/pvt-render --render --workers 12

# Manual backend selection and optional GPU admission bound
./build/pvt-render --render --backend cpu
./build/pvt-render --render --backend cpu+gpu --gpu-in-flight 2
./build/pvt-render --render --backend gpu
```

The requested value is capped by the frame count and the signed-int worker/API
capacity. Automatic selection also follows reported hardware concurrency. The
configurable aggregate memory budget derives admission from the checked
per-frame estimate; it is a user/host resource policy, not an input rejection
ceiling. A request is therefore an upper bound, not permission to exhaust RAM.

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
- Each layer's **Alpha Mode** is compositing order. Alpha Over keeps the layer
  above the accumulated lower stack; Alpha Under puts it below that stack. It
  does not enable procedural alpha or change whether the final file stores an
  alpha channel.

Adding a second layer enables final RGBA output automatically, as do features
that generate geometric/effect transparency. This does not silently enable
procedural modulation. Export validation rejects RGB combinations that would
drop real final-composite transparency; RGB remains valid when an opaque lower
stack guarantees an opaque result. Procedural modulation is neutral at its defaults
(`minimum == maximum == 1.0`); lower either bound to make it visible.

Built-in closed primitives and custom meshes are two-sided. With partial alpha,
the renderer samples the rear/exit surface and composites it behind the front;
it does not treat a translucent front as an opaque nearest-hit mask. Custom OBJ
rendering depth-peels until no deeper surface remains (with no fixed
eight-surface cutoff) and uses a faster nearest-surface path for fully opaque
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
./build/pvt-render --project-name "Midnight Bonfire" --save-default
./build/pvt-render --load "Midnight Bonfire.zip" --render
```

ZIP bundles and unpacked bundle directories contain the same human-readable
tree. **Open / Import** explicitly offers either a project file or an unpacked
project folder, and **Save As** offers an unpacked folder first because it avoids
rebuilding a compressed archive after every large-project edit. The first
Save/Save As fixes the sanitized archive/directory root. When a
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
    0.music_analysis.txt
    1.pvt
    1.music_analysis.txt
```

`render_output.txt` and each numbered `.pvt` hold the small global and per-layer
settings. Their correspondingly named `music_analysis.txt` files are checksum
references to shared, content-addressed analysis objects. A large time-varying
feature table is therefore stored once even when many layers and historical
versions use it, while every version remains independently reconstructable:
there is no base snapshot or delta chain that can be invalidated by deleting an
older version. Exact legacy snapshots with embedded global or layer analysis are
compacted transactionally on their next Save without discarding history;
direct/manual edits are either preserved or promoted through the normal
external-edit path. Version metadata stores the project display name, program/time,
group UUIDs, names, visibility and lock state, plus layer UUIDs, stable file
IDs, order, names, enabled states, group membership, blend and alpha modes,
opacity, attachment references, and SHA-256 digests. Root assets use
collision-safe content-identity directories and store exactly one physical copy
of each SHA-256 identity. Logical references keep their imported display
filenames and extensions even when two names refer to the same bytes. A valid
direct file replacement or unambiguous rename is loaded as a dirty external
edit; Save records fresh filename/digest/size metadata and promotes it to a new
version.
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
old snapshots are immutable and gaps are valid. A no-change Save verifies the
recorded tree identities and current snapshot and creates no version; the CLI
Versions menu's explicit Validate action remains the full-history content check.
When rewriting a ZIP, already validated,
unchanged entries retain their compressed bytes rather than being recompressed;
the completed temporary archive is still read back and compared with the exact
desired file set before installation. **Make Current** changes root bookkeeping
and the pointer but never alters a numbered snapshot;
**Revert as New** copies the selected snapshot into a new highest-numbered
version, so even a rollback can itself be rolled back. Semantic diffs follow
layer UUIDs instead of confusing renames/reorders with unrelated objects.
Starting **New Project** clears the prior bundle's version rows, selectors,
summary, and action targets immediately. Version comparison controls remain
disabled until the active document has at least two saved versions.

Every numeric directory is accounted for during full validation. Parseable
orphans can be promoted, while unrelated malformed/external trees are retained
byte-for-byte in an explicitly checksummed preserved-history table; lineage
aliases keep valid descendants connected even if an ancestor was edited or
deleted outside the application. Saves are serialized with a hidden sibling
advisory lock (`.<bundle>.pvt-save.lock`) and compare the complete expected
on-disk digest while holding that lock, so cooperating processes cannot erase a
newer save. The transient lock file contains no project data. It is removed by
file identity while still locked after the commit is complete, before the OS
lock is released; a contender verifies that its open handle still names the
current path before entering the transaction. Normal exit therefore leaves no
lock sidecar, while a stale blank sidecar from a crash or older release is
reused safely and removed by the next successful save.

The current loader validates and materializes only the selected snapshot during
normal Open; immutable history rows use their recorded metadata/tree identities
and are decoded on demand by Open, Compare, Revert, or explicit full validation.
The decimal music-analysis parser uses a locale-safe fast path, and split global
and layer analysis is decoded once rather than reconstructed repeatedly.
Normal GUI Open and Save are background transactions with an indeterminate
status indicator, so slow storage never blocks the event loop. Version diff is
also explicit and asynchronous instead of being launched merely by opening the
Versions tab.

On the expanded 41 MiB, 20-version Potato Fire reference directory, the released
1.1.3 CLI took about 5.3 seconds to load, 11.6 seconds for a no-change load/save,
and 13.2 seconds for a changed load/save. The 1.1.4 implementation measured
about 1.05, 1.77, and 3.38 seconds respectively on the same Mac Studio. These
figures include process startup and are local measurements, not cross-platform
guarantees; the asynchronous GUI path remains responsive when slower hardware
or storage takes longer.

Load is read-only and transactional. It tries the valid `current` snapshot first,
then numeric directories from highest to lowest until one validates. Missing or
broken pointers therefore do not destroy recoverable work. A checksum mismatch
is reported neutrally as an external change/integrity mismatch, not proof of who
or what changed it: parseable data opens dirty and is promoted to a first-class
new version on the next explicit Save. Typed setup recovery applies every safe
field, rebuilds missing or unusable fields from bounded defaults, keeps unknown
records verbatim, and stores rejected original values in a recovery envelope so
later builds can retry them. The CLI and GUI report actual repairs or preserved
data; a program version number alone never produces a false save-risk warning.
Future positive setup, layer, output, music-analysis, root-metadata, and version-
metadata headers are read through the newest understood schema when its required
structure is present. Unknown mutable root records are round-tripped; immutable
historical metadata remains byte-for-byte intact. Structurally unsafe snapshots
are skipped. Opening alone never rewrites the bundle.

Archive and directory input is treated as hostile. Readers bound entry count,
per-file/expanded size, path length, compression ratio, metadata records, layers,
and versions; reject traversal, absolute/drive/UNC paths, NUL or malformed UTF-8,
case-colliding duplicates, encrypted/multidisk archives, symlinks and special
files, unsupported compression, unexpected tree entries, CRC failures, and
structurally ambiguous records such as duplicate keys. Typed field damage is
recovered without weakening those archive/tree boundaries. Unpacked bundles ignore regular `.DS_Store`
files because Finder can create them merely by browsing a directory; ZIP bundles
remain exact and do not allow such extra entries. ZIP replacement and new directory-version commits use
checked sibling staging and atomic rename operations. Save also refuses a stale
or divergent destination rather than silently overwriting another history. An
exact copied/renamed bundle with the same UUID and observed state can be adopted
by Save As; a different UUID or advanced/divergent state is rejected.

Legacy deterministic line-oriented `.pvt` setup versions 1-18 remain importable;
current explicit legacy output is setup format 19. Format 4 added effect stage,
local-area data, localized swings, starting palettes, and layer transforms;
format 5 adds clock, music-analysis, audio-response, and embedded-source
identity data. Format 6 adds Data-only music, active-layer clocks, compact layer
motion, and particle settings. Format 7 adds starting images and reusable cubic
motion paths. Format 8 adds project-wide audio-response defaults, explicit
layer inheritance, and nullable per-wave/per-effect routing. Versions 1-7 keep
their historical layer-authoritative behavior on import. Format 9 adds explicit
per-wave/per-effect feature-source overrides while retaining format 8's force/
ignore meanings. Format 10 adds RGBA generated colors, source-alpha policy,
starting-image dithering, and Blur controls. Format 11 adds the disabled-by-
default clock-mixing switch and mode, Kaleidoscope and Domain Warp shaping,
Glitch/Starburst/Lens Distortion settings, and palette column/name/encoding
metadata. Format 12 adds layer-local RGB/alpha inversion and edge antialiasing,
plus project-portable Live roles, mappings, clock routes, scenes, calibration,
output preferences, and fail-safe policy. Format 13 adds generated Plane
displacement settings and the height-map attachment identity. Format 14 adds
pre-analysis audio filtering/EQ, named frequency-stream analyses and clock
selection, Live sleep prevention, Edge Detect/Twirl, and procedural particle
shape selection. Format 15 adds independent particle rendering profile, size
variation, definition, twinkle, seed, orientation, and rotation. Format 16 makes surface
projection, sizing, XYZ transforms/loop rotations, Euler order, camera, outside
fill, lighting model, rear compositing, and OBJ normalization explicit. Older
files receive visible compatibility values that reproduce their former
presentation without carrying hidden behavior forward. Format 17 adds numeric
parameter LFOs. Format 18 adds the independent red, green, and blue inversion
stages. Format 19 adds simultaneous RGBA channel routing and the exact
eight-stage post-processing order; project layer records use the corresponding
current layer format 17. Older records receive the historical stage order and
a disabled identity map.
Import creates a new unsaved
one-layer project with a new project/layer UUID and clears its save association,
so normal Save can never overwrite the source `.pvt`. New saves remain bundles.
The CLI exposes
`--save-legacy FILE.pvt` only as a clearly lossy escape hatch and rejects it when
more than one layer exists.

Project-version manifest format 5 adds layer groups and Alpha Mode. Manifest
formats 1 through 4 remain readable and load as ungrouped Alpha Over layers,
preserving their historical rendering. This manifest version is independent of
the legacy one-layer `.pvt` setup format described above.

A pre-product renderer revision also called 4.0.1 corrected its older 4.0.0
palette-stage bug without changing the setup schema: an enabled v4 palette
selects starting colors instead of rewriting the final effected image. Those
historical renderer revision numbers predate the product SemVer line. Setup
versions 5 through 8 add the synchronization/music/asset, local
clock/motion/particle, starting-image, reusable-path, and hierarchical
audio-routing data above.

## Scripted rendering

Common CLI overrides can be layered on defaults or on a loaded project:

```sh
./build/pvt-render --load "Midnight Bonfire.zip" --render \
  --width 640 --height 360 --block-size 4 \
  --frames 120 --fps 30 --waves 10 --workers 0 \
  --alpha --bit-depth 16 --png-compression 5 --dither blue \
  --output-dir preview --prefix ripple_
```

Layer selectors and modifiers are also processed left-to-right. `--layer 1`
selects the bottom layer; `--add-layer NAME` adds and selects a new top layer:

```sh
./build/pvt-render --load "Midnight Bonfire.zip" \
  --add-layer "Hot sparks" --blend add --layer-opacity 0.42 \
  --waves 5 --alpha-modulation --save "Midnight Bonfire.zip"
```

To wrap the generated image around a mesh from the command line:

```sh
./build/pvt-render --render --obj meshes/model.obj \
  --frames 120 --png-compression 5 --output-dir preview
```

`--obj` enables final RGBA output for the mapped exterior but does not alter the
active layer's procedural alpha modulation. It also imports the mesh into the
project's managed attachment cache immediately.

To generate and map a displaced Plane from a height map:

```sh
./build/pvt-render --render --height-map maps/terrain.exr \
  --height-min -0.3 --height-max 0.55 --height-midpoint 0.5 \
  --height-pixels-per-node 4 --frames 120 \
  --output-dir preview --prefix terrain_
```

`--height-map` selects Plane mapping, embeds the PNG or OpenEXR image, enables final RGBA, and
builds geometry at the effective render resolution. The four numeric controls
may be applied before or after the map because options are processed left to
right. `--no-height-map` bypasses displacement without discarding the embedded
asset or its authored settings.

To analyze a song, let it drive selected controls, and save a portable project:

```sh
./build/pvt-render --music tracks/live-tempo.flac \
  --music-tempo detected \
  --fps 30 --save "Live Tempo.zip"
```

The first `--music` import selects the Music clock and enables project-wide audio
response. Replacements preserve its current state. Use
`--project-audio-reactive` / `--no-project-audio-reactive` for the shared
profile, `--audio-reactive` / `--no-audio-reactive` for an explicit active-layer
override, and `--inherit-audio-reactive` to return that layer to the project
profile.

Clock overrides also include `--clock default|frame|time|meter|music`,
`--pulse-frames`, `--pulse-ms`, `--meter`, `--bpm`, `--tempo-note`,
`--clock-interpolation hold|linear|smoothstep`, `--clock-fit exact|sequence`,
phase/direction/beat-offset controls, and a selected-layer `--swings` master
toggle. Options are processed left-to-right, so put `--load` before overrides.
The interactive CLI editor additionally exposes active-layer clocks, their
duration and Data-only policies, layer motion presets, and particle controls;
scripted workflows can configure those fields in the GUI or load a saved bundle.

Run `./build/pvt-render --help` for all options. Existing matching output files are
protected unless `--overwrite` is explicit. A full sequence collision preflight
runs before frame zero, and each frame is installed atomically. Overwriting a
regular file preserves its explicit permission mode; overwriting a symlink replaces
the link entry rather than modifying its target.

Relative output directories are resolved against the process working directory.
On macOS, `make gui` runs the app-bundle executable directly so it inherits the
make working directory. If a desktop launcher supplies `/` (the source of the
old `.` export failure), the GUI rejects that unusable launch directory, anchors
relative paths in the user's home folder, and never treats `.` as filesystem root.

## Displacement Plane surfaces

Plane displacement samples the selected PNG or OpenEXR image across a regular output-space
grid. `pixels_per_node = 1` produces `(width x height)` vertices. For larger
ratios, each dimension uses `1 + ceil((dimension - 1) / ratio)` vertices so the
right and bottom edges remain exact even when the ratio does not divide the
resolution. Triangles share per-vertex smooth normals computed from the actual
height field. Image alpha is ignored; raw PNG samples and HALF/FLOAT OpenEXR
samples are treated as linear data. RGB is reduced to Rec. 709 luminance and
clamped to the normalized height-data range; a single EXR data channel is
replicated directly.

The minimum displacement is constrained to `[-2, 0]`, maximum to `[0, 2]`, and
midpoint to `[0, 1]`. A sample equal to midpoint has zero height; the two sides
interpolate independently, allowing asymmetric valleys and peaks. The existing
Surface **Curvature** control is an exact flat-to-mesh crossfade, and Surface
phase/rotations animate the generated object with the same projection used for
custom meshes. Curvature zero is neutral and does not require mesh work.

Decoded height images and generated meshes use bounded evicting caches. The
mesh key includes decoded-image identity, effective resolution, pixel-to-node
ratio, minimum, maximum, and midpoint. Preview and Live resolution scaling
therefore create smaller grids without changing the authored output settings;
full-resolution frame/video/Live paths request the corresponding full grid.
The GUI **Export output-resolution OBJ…** action writes the exact authored
resolution mesh atomically and never replaces an existing destination until
the complete OBJ has been generated successfully.

## Custom OBJ surfaces

The OBJ loader accepts ASCII/UTF-8 `v`, `vt`, `vn`, and `f` records,
including positive or negative indices and the standard `v`, `v/vt`, `v//vn`,
and `v/vt/vn` face-corner forms. Simple polygon faces are validated and
triangulated while preserving winding and per-corner attributes. Object/group,
smoothing, and material metadata is ignored: the procedural frame is the sole
surface image, and no `.mtl` or sibling file is opened.

Meshes are uniformly normalized from their referenced bounds and rendered with
perspective-correct texture/normal interpolation. A triangle uses its authored
texture coordinates only when all three corners provide them; otherwise it uses
dominant-axis box projection. Missing normals fall back to the geometric face
normal. Default capacity follows actual representation limits: indices use every
non-sentinel `uint32_t` value, Qt-facing text/counts fit signed `int`, and file,
line, triangle, and expanded-mesh storage is limited by checked `size_t`
arithmetic and successful allocation. Callers may inject smaller explicit
budgets. Malformed or allocation-failed loads are transactional. The last successfully loaded
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

- fully owned legacy render and project/layer/group configuration values;
- default factories, stable ID allocation, and validation;
- float RGBA layer/project rendering by frame index or normalized phase;
- bounded linear-light blend compositing, individual PNG/EXR writing, and
  composite sequence export;
- backend-neutral CPU/CPU+GPU/GPU frame options plus Metal and OpenGL
  generated-source/surface capability reporting;
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
documentation—and the GUI when it was enabled—with:

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

For a normal system Qt installation on Linux, the application can be built and
installed conventionally:

```sh
cmake -S . -B build-linux -G Ninja \
  -DPVT_BUILD_QT_GUI=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel
cmake --install build-linux --prefix /usr/local
```

The default install relies on the system Qt runtime, as Linux package managers
expect. Set `-DPVT_DEPLOY_QT_RUNTIME=ON` for a relocatable staging prefix on a
supported Qt kit. The same option runs Qt's deployment script on Windows; it
does not download dependencies.

To bump the public version, run `scripts/bump-version.sh MAJOR.MINOR.PATCH[-PRERELEASE]`,
then reconfigure. The script updates `VERSION` and its displayed/package metadata
from that one value while leaving release notes, tags, builds, and uploads explicit.

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
palette ordering and toggle bypass, post-effects inversion/edge antialiasing/
quantization,
embedded starting-image decode/fitting/cache and CLI rendering, transforms,
closed presets plus reusable cubic-path geometry/bindings/loop closure,
particles, direction modes, alpha range and
straight-alpha/glow composition,
primitive mappings, rear-surface alpha/color compositing, representation-bounded OBJ
parsing/caching and
two-sided perspective rendering, animated smooth/stepped block grouping and
effect ordering, default glow visibility, memory and value limits, setup round
trips and transactional failure, project/layer/group validation, every blend
mode including destination-out erasers, Alpha Over/Under ordering, group
visibility, opacity and paint order, full-resolution current-frame GUI export,
ZIP/directory bundle round trips, immutable version
append/no-change validation, semantic diffs, current/revert behavior, legacy
promotion, checksum/fallback handling, readable attachment names and direct-edit
promotion, deleted-original recovery, invalid replacement rejection, hostile archive/tree
rejection, adaptive beat/tempo changes, dense transient and spectral/pitch
features, global/active-layer clock interpolation, duration mapping and response
routing, native PNG/ProRes/HEVC movies, synchronized multi-source audio mixing,
Data-only exclusion, hardware-required encoder
selection, video cancellation/collision safety,
8/16-bit RGB/RGBA PNG data, raw 16-bit PNG height samples, compression levels 0
and 9, and HALF/FLOAT single-channel or RGB/RGBA EXR input plus FLOAT EXR output,
deterministic dithering, byte-identical one/four-worker sequence output,
callback/cancel behavior, sequence collision preflight, Unicode paths,
CPU/Metal base/effect/analytic-surface image and straight-alpha parity,
near-seam parity, generated-source/image/particle/OBJ Metal coverage, strict
backend errors, bounded CPU/Metal lane admission,
transactional cancellation, and the public library API. It also exercises CLI
help, option rejection, and the
multi-layer CLI self-test. With the GUI enabled, CTest launches it through Qt's
offscreen platform, exercises project/layer/bundle/synchronization and inherited
audio-routing state, verifies
that Play installs advancing completed preview frames, opens and inspects About
PVT and the reusable-path editor, and checks adaptive UI layout behavior.

## Live performance direction

The Qt application includes a sibling **Live mode** that shares the renderer,
effects, project schema, validation, and undoable authoring state with the Flow
Workbench. It adds bounded low-latency audio-device capture and causal feature
analysis, CoreMIDI control and 24-PPQN clock routing on macOS, OSC input, stable
setting mappings with MIDI Learn, project/layer live clocks, scene recall,
full-screen secondary-display output, freeze/blackout, last-good-frame safety,
and a frame-time watchdog. **Edit Project** opens the complete authoring
workspace without stopping that runtime; returning to Live shows the same
uninterrupted session. Machine-only audio, MIDI, OSC, and screen bindings are
kept in local application preferences.

Live audio applies the same authored high-pass, low-pass, and graphical EQ model
before causal analysis, then analyzes each named frequency range independently.
Project and layer routes can select those streams explicitly. Choosing **LIVE**
opens the workspace automatically in a dedicated companion window instead of
replacing the editor; while it is active, its routed renderer frames also feed
the ordinary editor preview. A portable **Prevent device sleep while Live is
active** safety option uses the native macOS or Windows power assertion when
available and releases it immediately when Live stops or its window closes.

Projects persist logical endpoint roles, portable mappings, scenes, signed
latency compensation, clock routes, and safety preferences. They never persist
an operating-system device ID, network address, display identity, captured
buffer, current scene, freeze state, or blackout state. Incremental analysis is
an ephemeral renderer overlay rather than a fabricated analyzed music file.
This boundary lets the same show file move between rigs without silently
binding to the wrong hardware.

An audio-interface aux or post-effects send lets a performer choose whether
the visuals react to the acoustic instrument, the effected pedal/looper chain,
or both. Syphon/Spout/NDI-style routing remains a later optional adapter after
the local performance path has completed real-device and venue soak testing;
see `LIVE_PERFORMANCE_STATUS.md` for the exact handoff checklist.

## Post-1.0 roadmap

- **PVT-Live routing follow-up:** keep the shared renderer and project model;
  validate the integrated Live mode on stage hardware, add native MIDI adapters
  beyond macOS, then add optional Syphon/Spout/NDI routing without making those
  transports renderer dependencies.
- **Linux distribution / 1.1.x:** build and debug Snapcraft packages in an
  Ubuntu VM, then Debian packages and a Launchpad PPA in appropriate Debian and
  Ubuntu test systems. These formats should not be guessed from macOS.
- **GUI automation / 1.1.x:** expand beyond bounded smoke paths into every
  editor, undo merge boundary, semantic diff, bit-depth transition, progress
  path, and cancellation race.
- **Creative controls / 1.1.x:** add more effect types and deeper parameters,
  custom asset-backed particle sprites, plus optional depth-map and normal-map
  inputs for selected layer regions.
- **Platform parity:** Metal remains the broader macOS accelerated backend. The
  portable Qt-hosted OpenGL path now covers ordinary Continuous hue generated
  layers and supported surface mapping in Windows and Linux product builds.
  Full effect/source coverage and physical
  Intel/AMD/NVIDIA driver qualification remain follow-up work described in
  [`PORTABILITY_ROADMAP.md`](PORTABILITY_ROADMAP.md). Media Foundation remains
  the preferred future Windows movie path and optional GStreamer the Linux
  path; PNG/EXR sequences remain the dependable cross-platform export today.

## Current boundary

Continuous hue generated sources and flat Plane, displaced Plane, closed
cylinder, sphere, and cube mappings have Windows/Linux OpenGL paths alongside
their reference CPU implementations. Custom OBJ mapping remains an ordered CPU
stage inside CPU/Metal policies and CPU + GPU portable rendering. OBJ materials and
textures are intentionally not loaded because the procedural frame supplies the
surface image. Cooperative cancellation is checked within CPU rendering, GPU
admission, effects, surface mapping,
quantization, layer compositing, mesh generation/rasterization, and every
PNG/EXR output scanline. An in-flight GPU command
buffer may finish, but its result is discarded before installation when
cancellation is observed. Music, custom OBJ, displacement-height, and
starting-image attachments are embedded under their content identity and exact
original filename. Live
audio/data capture and non-macOS native movie containers remain future work. See
`IMPLEMENTATION_STATUS.md` for the detailed hand-off ledger.

This project is licensed under GPLv3. Applications distributed with the library
must account for the GPL's linking and source-distribution requirements.
