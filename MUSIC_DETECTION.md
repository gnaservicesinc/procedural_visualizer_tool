# Maintainable Music detection

Project and active-layer Music controls now expose **Detection, Filters, EQ +
Frequency Streams… → Detection type**. Each choice applies to the main Music
source and every named frequency range:

| Choice | Behavior | Tradeoff |
| --- | --- | --- |
| Hybrid (default) | Historical spectral changes plus positive energy changes | General-purpose starting point |
| Spectral flux | Positive spectral changes without energy blending | Finds note attacks with little overall loudness change |
| Neighbor flux | Compare each bin against the previous bin and its neighbors | Suppresses small pitch movements; can suppress nearby notes too |
| High-frequency flux | Weight positive changes by normalized frequency | Emphasizes bright percussion; can underweight bass drums |

These are onset detection choices, not instrument classifiers or guarantees of
beat accuracy. The existing adaptive tempo/beat grid follows the resulting
onsets and local tracker observations. Live capture continues using its separate
causal analyzer; these Music-only controls are hidden in the Live input dialog.

Changing detection on an analyzed source starts transactional reanalysis. The
project receives the new choice and cached tables only after analysis succeeds;
cancellation leaves the current project untouched. The setting is stored in
`AudioInputProcessingConfig::music_onset_detection` and in each cached analysis.
A mismatch invalidates that cache. Rendering/export consumes the saved tables.

Setup/layer/output/music text codecs share the optional `music_onset_detection`
record (`hybrid`, `spectral-flux`, `neighbor-flux`, `high-frequency-flux`). Missing
records restore Hybrid. Existing unknown-field preservation continues to apply.
Raw layout 2 appends five enum values after the unchanged layout-1 byte stream;
the loader accepts complete layouts 1 and 2, defaulting old snapshots to Hybrid.
The analyzer version is `pvt-adaptive-onset-audio-5`. Already analyzed songs retain
their saved beat tables until explicitly reanalyzed. Reanalysis can change timing
slightly because the observer's callback timestamps now follow actual FFT hops.

The new PVT-owned C onset module lives in the maintained tracker repository and
is shared by its observer and PVT's high-resolution Music analyzer. See
`third_party/btt/README.pvt.md` for synchronization and provenance.

Validation includes analytic spectra, per-sample versus irregular-block callback
identity, reset/fresh-stream identity, bounded startup timestamps, synthetic
Music click tracks across all four choices and named ranges, cache rejection,
transactional cancellation, raw/text/bundle round trips, and the actual Qt dialog.
Broad musical-corpus quality rankings and Live spectral detector integration
remain future work; the defaults do not imply those have been established.

## Completion and compatibility record — 2026-09-10

The tracker and PVT integration are complete for this iteration. Completion
review restored the Windows `rand()` branch that the initial vendor sync had
overwritten, and the standalone CMake build now explicitly requests C11. Linux
enables the feature declarations needed by the retained POSIX statistics/window
helpers. All 16 source/test files in the vendor manifest match the maintained
tracker checkout.

The public `AudioInputProcessingConfig` gained an enum member. On this ARM64
build it grows from 72 to 80 bytes, `ClockConfig` from 544 to 560 bytes,
`RenderConfig` from 3208 to 3248 bytes, and `ProjectConfig` from 1192 to 1216
bytes. The combined pending release is therefore **19.0.0 / SONAME 19**, replacing
the unpublished 18.0.1 preparation. Library consumers must rebuild; project
compatibility is handled independently through the legacy raw reader and
optional text records.

Completed checks reused without repeating local tests:

- Standalone tracker Release: 1/1 passed, including analytic detector results,
  irregular-buffer callback identity, reset identity, and metronome behavior.
- Standalone tracker ASan/UBSan: 1/1 passed.
- PVT audio analysis, core, and bundle tests: 3/3 passed, including all four
  detectors, named ranges, cancellation, cache mismatch, and raw/text round trips.
- Preview-fix core, composite, Metal, Live-controller, and Cocoa GUI smoke:
  5/5 passed with the detector integration present in the working tree.
- German and French catalogs: all 3071 entries finished. All 12 new or changed
  detector messages were reviewed; English uses source-text fallback.

Additional completion checks:

- The previously unrun Qt detector-dialog test passes: initial selection,
  editing, acceptance, caller-state isolation, and hidden Live controls. Its
  captured image was reviewed for visible labels, help, and action buttons.
- Native standalone C11 library build and rebuilt PVT GUI/CLI succeed.
- Compile-only ABI comparison confirms the sizes above; a compile of the
  `_WIN32` statistics branch references `rand`, not unavailable POSIX `random`.
  This is a branch check on macOS, not a claim of a native Windows build.
- The rebuilt CLI reports 19.0.0. Debian runtime metadata selects library 19.

Existing evidence is in `/tmp/pvt-tracker-build/Testing/Temporary/LastTest.log`,
`/tmp/pvt-tracker-sanitized.log`, `/tmp/pvt-detectors-focused.log`, and
`/tmp/pvt-wood-work/regression-tests.log`. Completion build, ABI, and UI evidence
is under `/tmp/pvt-tracker-finish`. The rebuilt app is
`/tmp/pvt-wood-work/native/pvt/Procedural Visualizer Tool.app`.

At completion, no changes had been committed, pushed, or tagged and release was
paused. Release preparation now publishes the maintained tracker and PVT changes
together; subsequent CI and publication evidence is recorded in
`IMPLEMENTATION_STATUS.md`. Local runtime suites are not repeated.
