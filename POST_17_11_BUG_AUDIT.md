# Build repair and audit of changes after 17.11.0

Date: 2026-09-10. Reviewed range: `v17.11.0` (`c52112c`) through
`d3307169bfaa9f8cad5e342acfcd9c28393b573f`, plus the fixes in this working tree.

## Reported build failures

Both jobs in [workflow 34518287863](https://github.com/gnaservicesinc/procedural_visualizer_tool/actions/runs/34518287863)
compiled successfully. Windows x64 failed the French GUI smoke test; macOS
arm64 passed its CTests and then failed the packaged English GUI smoke test.
Both reported that playback did not install advancing preview frames.

The editor could replace a completed future before Qt delivered its queued
`finished` signal: `isRunning()` becomes false before result delivery. The same
latent race existed in the Live controller. A timer tick in that interval lost
the frame and could starve display updates. Both controllers now reserve the
future through completion delivery. Live Stop also discards queued completions.
The tests force the worker to finish while withholding event processing, then
request another frame. The editor check still requires advancing frames during
playback; its timeout was not increased.

## Additional defects fixed

| Area | Failure and correction | Regression evidence |
| --- | --- | --- |
| Numeric fields with units | The custom validator tried to parse prefixes/suffixes as part of the number, preventing typing into controls such as gain in dB. Validation and conversion now strip the decorations. | Typed decorated fractions in English, German, and French locales. |
| Negative fractions | `-0 1/2` became positive and `-½` was rejected. Preserve the written sign, including negative zero. | All negative representations resolve to -0.5. |
| Automatic settings | Focusing exposed the resolved number as the actual setting; typing that same number emitted no edit, and step changes could revert silently on focus loss. Show the number without changing the stored sentinel, then commit intentional typing/steps with the normal signal. Refresh labels even when an explicit value is selected. | Sentinel, edit counts, same-number typing, stepping, clearing, and label refresh. |
| Named zero settings | Stepping away from BLACKOUT could be silently reset to zero when focus left. Keep intentional changes and restore only untouched/cleared entries. | Step away from BLACKOUT and lose focus. |
| Truncated raw strings | Missing complete trailing string records could still produce a successful load with a partial configuration. String-read errors now latch and stop further reads. | Four truncations fail with a diagnostic and preserve the destination. |
| Raw collection counts | A damaged count could resize a huge collection before checking the available bytes. Bound counts by remaining numeric input before allocating. | Corrupt one count to one million; reject before a 1 MiB allocation request. |
| Invalid raw scalar output | Nonfinite values in disabled settings could omit a numeric slot without marking serialization failed. Invalid enums could also produce a snapshot its reader rejects. Reject both with diagnostics. | Disabled-LFO NaN serialization fails; existing enum/round-trip tests retained. |
| Unknown-field preservation | Binary snapshots omitted retained forward-compatibility records. Store those particular snapshots in the existing text format, retaining Binary as the project preference. The status describes actual text use; direct raw serialization rejects unsupported preserved records. | Save, reopen, and validate a Binary-preference project containing unknown global and layer fields. |
| BLACKOUT and modulation | A zero base block size bypassed an enabled block-size LFO. The fast path also accepted invalid dimensions, phase, and backend options. Only unmodulated zero uses the shortcut, with canvas/options/phase checks and transactional image allocation. | Exact standalone/project CPU image comparisons, invalid-input rejection, destination preservation. |
| Preview/Live LFO scale | Downscaled canvases kept full-resolution LFO bounds, causing incorrect block sizes or validation failures. Zero-sized sources also generated an invalid partial color-reference tuple. Scale the LFO range and keep absent references consistently zero. | Editor scaling checks and a rendered 64-to-32 Live canvas with an LFO starting from zero. |
| Supersampled project memory | Per-layer validation reused the canvas validation result but estimated layer workloads at output resolution. Use the validated supersampled dimensions for each layer's memory/workload estimate. | A 0.25-block project estimate covers its equivalent standalone layer estimate. |
| Faint subpixel colors | Both area-downsample implementations erased RGB below an arbitrary alpha threshold, despite retaining nonzero alpha. Unpremultiply every positive accumulated alpha. | At alpha 1e-25, RGB survives and both CPU entry points produce identical float buffers. |
| Invalid history policy | Invalid public API enum values could fall through to the Disabled retention behavior. Reject malformed file-I/O settings before mutating the document or files. | Invalid history mode leaves root metadata and the version list unchanged. |

## Verification

- Clean AppleClang Release build with Qt 6.11.2, Metal and OpenGL: **43/43 CTests**.
- AddressSanitizer, UndefinedBehaviorSanitizer and float-cast-overflow checks:
  **5/5 focused suites** (core, project composite, bundle, numeric editor, Live
  controller). Leak detection disabled for this run.
- Playback: three consecutive runs each of English, German, French and the
  focused Live controller test: **12/12 executions**, including deterministic
  queued-completion coverage.
- Self-contained macOS package: distribution verifier passed for **50 Mach-O
  files**, packaged en/de/fr GUI smoke checks passed, and the packaged CLI
  self-test passed. Metal and OpenGL are ready on the Apple M2 Max; the OpenGL
  test also passed with `PVT_REQUIRE_OPENGL=1`.
- German/French catalogs remain **3,060/3,060** complete. `git diff --check` passed.
- Negative control: built the new tests against an isolated archive of unchanged
  `d330716`. All four focused suites failed on the intended defects (13 bundle
  assertions, 9 project assertions, numeric editor failures, and a lost Live
  completion). The additional core faint-alpha test also failed on that code.
  The same tests pass with the fixes.

Local evidence is under `/private/tmp/pvt-audit-native`,
`/private/tmp/pvt-audit-sanitized`, and `/private/tmp/pvt-audit-before`; detailed
logs use `/tmp/pvt-audit-*.log`. These temporary files are not release artifacts.

## Scope and remaining qualification

The pass covered the new storage readers/writers, revision-policy and delta
paths, numeric controls, layer-source import staging, fractional rendering,
preview scaling, and their validation/backend boundaries. Existing bundle tests
cover stale writes, partial revision replacement, retention/pinning/deletion,
text deltas, and old-format recovery. This is not a proof that every possible
configuration is defect-free. Power-loss fault injection for multi-file partial
revision updates and every possible numeric GPU lattice boundary were not
exhaustively tested.

The fixes are committed and pushed to `main` as
`75e71c84fc3c8493a07b1c241edc9ac65d718c41`. On 2026-09-10,
[desktop CI run 34528905536](https://github.com/gnaservicesinc/procedural_visualizer_tool/actions/runs/34528905536)
passed all five platforms: macOS arm64, Windows x64/arm64, and Linux x64/arm64.
This includes native tests, both Linux shared-library packaging configurations,
macOS package verification, and package uploads on every platform. The two
previously failing macOS and Windows x64 jobs now pass.
[CodeQL run 34528904870](https://github.com/gnaservicesinc/procedural_visualizer_tool/actions/runs/34528904870)
also passed for that exact commit.

Local Qt is 6.11.2; desktop CI uses Qt 6.8.3. The public raw layout and existing
format/version identifiers are unchanged. No version bump, tag, or publication
was performed; the tagged-release job was correctly skipped on this branch run.
