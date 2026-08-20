# Realtime Output Follow-up

Implemented in this pass:

- **Live Preview Output** presents the editor/Preview Solo frame on a chosen display without starting performance Live, audio capture, MIDI, OSC, scenes, routes, or sleep prevention.
- Presentation and performance Live share the machine-local display and render-quality choices. Presentation-only full-screen and pointer choices are also machine-local.
- Output rendering uses the selected CPU/Metal policy, physical output pixels (including device pixel ratio), a single in-flight plus latest-pending queue, stale document/session rejection, and last-good-frame behavior.
- Escape or closing presentation output stops presentation rendering. Escape or closing the performance stage hides that surface while leaving the Live runtime running.
- Frame/video export and realtime output are mutually excluded in both the UI and export entry points.

Deferred audit/follow-up work:

1. **Multichannel / multi-device audio clocks** — Mic authoring, duplicate-safe runtime device IDs, stale-binding handling, calibration, and continuous beat routing are implemented. The capture engine remains intentionally mono and single-device. Making `audio_channel` and simultaneous independent Audio roles real requires a per-endpoint multichannel capture manager, not additional labels over one stream.
2. **Asynchronous realtime teardown** — both presentation dismissal and full-performance shutdown currently wait for cooperative render cancellation before their output state is released. This keeps export exclusion and renderer ownership correct, but a pathological backend can make dismissal pause. A future implementation should add an explicit stopping/draining state before moving teardown off the UI thread.
3. **HDR / wide-gamut stage output** — current preview/stage buffers are explicitly tagged sRGB 8-bit. A future 10-bit/HDR mode needs an end-to-end surface, compositor, export-preview, and display-capability policy rather than only changing the window color space.
4. **Stable identity for indistinguishable displays** — serial numbers are preferred and duplicate labels/within-session screen objects are disambiguated. Two truly identical displays that expose no serial/EDID-stable identifier can still swap ordinal fallback identities after primary-display reordering; platform display UUID support is the durable fix.
5. **Display-lab coverage** — keep manual coverage for unplug/replug, primary-display changes, mirrored displays, mixed-DPR screens, full-screen Space changes, and two identical monitors that expose no serial number. Automated smoke covers policy/state transitions but cannot synthesize all platform display events faithfully.
6. **Long performance soak** — run CPU, CPU+GPU, and strict GPU realtime output for multi-hour edit/playback/display-change sessions while observing memory, dropped-frame convergence, and backend errors. The automated native, C++20, sanitizer, shared-library, packaging, and Cocoa smoke matrices pass for 7.0.0, but they do not replace a multi-hour physical-display soak.
