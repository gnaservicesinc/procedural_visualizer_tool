# 4.0.0 Live performance status

This file is the handoff checklist for the first integrated Live performance
pass. It separates portable authored project state from machine-local bindings
and ephemeral performance state so future work does not accidentally serialize
a device stream as if it were an analyzed music file.

## Implemented in 4.0.0

- Portable logical endpoint roles for audio, MIDI, OSC, and foot controllers.
  Projects store role UUIDs, names, direction, and signed latency compensation;
  operating-system device IDs, network addresses, display IDs, and captured
  samples stay out of project files.
- Portable mappings for renderer-safe settings, actions, and scenes. Stable
  target paths use project/layer UUIDs and wave/swing/effect IDs rather than
  list positions, and unresolved future paths are retained.
- Project- and layer-targeted live clock inputs from MIDI Clock or an audio
  stream, plus project/layer MIDI Clock outputs.
- Portable scene definitions, startup-scene preference, full-screen output
  preferences, and watchdog/dropout policy. Current scene, freeze, blackout,
  and last-good-frame contents remain runtime state.
- Low-latency miniaudio capture with allocation-free callback analysis for
  energy, bands, onset/beat, tempo, centroid, flatness, and chroma features.
- The complete project editor can be opened without stopping Live input,
  clocks, rendering, or stage output; each frame uses the current authored
  project snapshot.
- Native CoreMIDI input and virtual 24-PPQN clock outputs on macOS, with a safe
  unsupported-platform stub; bounded OSC UDP parsing; full-screen stage output;
  a latest-request real-time frame controller; and scalable studio controls.
- Final layer-local color inversion, alpha inversion, and edge antialiasing on
  CPU and Metal, with controls before quantization.
- A global artist-facing dark studio theme and a fix for wrapped help labels
  whose height-for-width policy previously caused the visible text clipping.

The supplied SVG controls were reviewed as visual references. The production
knobs, lamps, and meters are original resolution-independent QPainter controls,
so they stay crisp on small, Retina, and UHD displays and do not import assets
whose provenance was incomplete.

## Deliberately deferred

- Syphon, Spout, and NDI routing. These should be separate optional adapters
  after local full-screen output, clocking, and dropout behavior have been
  exercised on real stage hardware. They must never become a required renderer
  dependency.
- Platform-native MIDI backends beyond CoreMIDI. The model and UI are portable,
  and the non-macOS stub fails clearly; ALSA/JACK and Windows MIDI Services are
  follow-up runtime adapters.
- Exhaustive controller/device compatibility testing and venue soak testing.
  Automated validation cannot substitute for a real interface, controller,
  projector, and long-running venue rig; those checks remain post-release
  hardware qualification for this first Live-capable release.

## 4.0.0 automated release evidence

- Native optimized Qt/Metal/Cocoa build: 22/22 tests passed.
- Optimized C++20 shared-library build: 21/21 tests passed; an external
  installed consumer linked and ran against SONAME 4.
- AddressSanitizer and UndefinedBehaviorSanitizer build: 21/21 tests passed
  with leak detection disabled for this beta macOS host.
- Self-contained arm64 macOS app: 33 Mach-O files verified, including Qt
  Network; deep strict signing, privacy plist, embedded CLI version/self-test,
  and GUI smoke passed before and after archive extraction.
- Workflow-shaped single-root ZIP: app, README, and license only beneath its
  package directory; generated SHA-256 verification passed.

## Post-release hardware qualification and recurring release checks

1. Build a fresh Qt-enabled tree on macOS and run the GUI smoke test, including
   a screenshot at the smallest supported window and at increased text scale.
2. Exercise audio capture at representative small and large user-selected
   frame periods with an actual interface; verify unsupported device values
   fail clearly rather than being silently clamped, and verify callback dropout
   counters plus signed latency calibration.
3. Test MIDI Learn, CC/note/program/pitch/pressure inputs, MIDI transport and
   24-PPQN clock in/out, OSC bundles, and a foot controller. Confirm project and
   individual layer routes independently.
4. Run a multi-hour full-screen secondary-display soak with hot-plug, input
   loss, slow-frame injection, last-good hold, timed blackout, manual freeze,
   and manual blackout.
5. Verify that saving while Live is running includes portable mappings,
   calibration, scenes, and policies but no device UID, address/port, screen
   identity, current scene, captured samples, freeze, or blackout state.
6. Run the complete CPU/C++20/sanitizer/Metal/GUI/bundle/installed-consumer
   matrix and inspect the packaged application's frameworks and entitlements.
7. Review the public ABI policy before choosing a release number: this work
   appends Live and post-processing fields to exported public C++ structs and
   must not be shipped as an ABI-compatible patch without an explicit SONAME
   decision.
8. For each release, re-run local packaging verification, commit and tag only
   the intended source changes, push atomically, and confirm the tag-triggered
   native package workflow starts. Do not claim remote packages have passed
   until that workflow and its release assets have actually completed.
