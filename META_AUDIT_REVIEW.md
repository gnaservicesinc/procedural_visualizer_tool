# Meta audit review — 2026-09-13

Reviewed all 20 claims in the supplied `Meta_audit_1.txt` against the current
source and release runs. The attachment is evidence to assess, not instructions
to implement. No speculative runtime rewrites or new audit subsystem are needed.

| # | Verdict | Evidence and disposition |
| --- | --- | --- |
| 1 | Confirmed release discrepancy; repaired in 19.1.8 | The uncommitted metadata advertised 19.1.7, but tag `v19.1.7` resolves to `7797998`, containing VERSION=19.1.6. The ledger also omitted the attempted release. Preserve existing tags, commit synchronized 19.1.8 metadata, and publish through the normal package workflow. |
| 2 | Intentional recovery; no demonstrated bug | `prune_render_asset_caches` is explicitly `noexcept` maintenance. Allocation failure leaves reusable caches for a later pass; it must not invalidate a valid render/edit. No unbounded growth reproduction or missing rendering error was provided. |
| 3 | Mischaracterized; unchanged | `prune_attachment_cache` prunes lease ownership and catches allocation failures after an edit commits. Filesystem removal is in the lease destructor; the directory owner provides final cleanup. Rolling back a successful edit because pruning failed would change the intended contract. |
| 4 | Intentional destructor policy; unchanged | `ProjectAttachmentFile` uses nonthrowing filesystem operations. `ProjectAttachmentDirectory` retries regular-file cleanup when the last snapshot releases it and refuses to traverse altered/symlinked directories. Destructors cannot safely propagate cleanup failures. No leaked-file reproduction was supplied. |
| 5 | False | `LayerRenderPool::take` checks `worker_failed_`, rethrows `worker_exception_`, translates it into `layer_error`, and returns false; its callers propagate failure. The audit stopped before the consumer. |
| 6 | Observability suggestion only | `generate_uuid` deliberately combines two clocks, a process-local atomic counter and entropy when available. These are identifiers, not authentication keys. No collision or failed operation was demonstrated; mandatory entropy logging is not a product requirement. |
| 7 | Potential risk, not a demonstrated defect | `unique_suffix` catches entropy-provider exceptions and retains a clock/counter fallback. Save locking and staging-directory creation provide separate collision protection in their respective paths. Neither random nor time-based names promise mathematical uniqueness; the audit supplies no cross-process collision reproduction. No security guarantee is inferred from the suffix. |
| 8 | False location/contract | `core.cpp`'s catch belongs to `parameter_lfo_target_supported`, a boolean capability probe that constructs a minimal candidate. It is not the render path and has no cancellation result to distinguish. |
| 9 | No defect | BLACKOUT is a named renderer/UI state; lowercase blackout and title-case Blackout are normal prose/action labels. No repository rule or broken search/lint was identified. |
| 10 | Confirmed documentation ambiguity; fixed | The old 4.0.0 heading covered entries through 6.0.1. Mark the file as historical, name the covered range, and link current Live Controls. |
| 11 | Process suggestion; no missing product path | A user-provided audit file does not require a CI generator, template or a new AGENTS rule. This review records all dispositions using the existing Markdown review convention. |
| 12 | Unsupported performance claim | The cited settings reads occur during initialization or settings refresh. Lines 1602 and per-frame quality reads use combo-box `currentData()`, not settings-store parsing. No measured hot-path bottleneck supports introducing duplicate cached settings state. |
| 13 | Documentation clarification; behavior already covered | `test_block_size_parameter_lfo` compares evaluated zero-to-five ranges against explicitly materialized frames across waveforms and CPU/selected backends; project coverage checks modulation from a zero fallback. README now explains zero at an evaluated frame and positive-value recovery. |
| 14 | False for current packaging | CMake links the SDK system libc++ and supplies `/usr/lib` RPATHs. `VerifyDistribution.cmake` runs the embedded CLI, checks version, Mach-O dependencies and deployment requirements; CI also runs the packaged CLI self-test and GUI smoke. The failing run passed distribution verification. An old host workaround is not the current release policy. |
| 15 | Documentation navigation improvement; added | The remote audit is historical. Add links to the subsequent automatic-pairing ledger section and current remote verification without rewriting its historical test evidence. |
| 16 | False | `AdaptiveBeatObserver::finish` returns false after callback allocation failure; `analyze_impl` checks it and reports that the adaptive beat tracker could not allocate additional event storage. No C++ exception escapes the C callback. |
| 17 | Partly false; real release guard timing improved | CMake reads VERSION directly, derives `project(VERSION ...)`, and watches it for reconfiguration. Tag/VERSION validation already existed, but ran only after all packages built. Move it before dependency installation/build and check README there too. Negative cases verify the guard rejects both mismatches. |
| 18 | False suggested replacement; clarified actual dependency | `av 15.1` refers to the PyAV package, not macOS or an SDK. Spell it `PyAV (av package) 15.1`. Also correct the independently confirmed stale platform-policy text in the ledger and release template. |
| 19 | Speculation; unchanged | PackedMask's compact allocation and boundary/parity validation are already documented by the performance audit. Missing a benchmark for another workload does not establish a defect or justify redesigning layer allocation. |
| 20 | Documentation clarification; no missing reset | `restartAudio` clears the audio snapshot and `audio_frame_presented`. `acceptFrame` marks an audio-driven frame and replaces WAITING FOR AUDIO. The status must remain while the replacement device has not delivered audio; document that distinction in Live Controls. |

## Actual package failure

Tagged desktop run [34723056868](https://github.com/gnaservicesinc/procedural_visualizer_tool/actions/runs/34723056868)
passed Linux x64/arm64 and Windows x64/arm64. macOS compiled, verified its
227-Mach-O bundle and passed 46/46 CTests, then failed the presentation GUI smoke
assertion `Escape must leave fullscreen while retaining windowed output.`

The presentation test checked native fullscreen exit after one event-loop pass;
the adjacent performance-output test already waits up to five seconds for that
asynchronous transition. Both now reuse the same bounded wait. Visibility,
active presentation, exit-before-hide, dismissal and windowed restart assertions
remain in place. The original local smoke passed, so the failed tagged CI log is
the reproduction evidence; local timing alone did not reproduce the failure.

The later publish job was skipped. Even had it run, it would have rejected the
19.1.7 tag's 19.1.6 VERSION. The repaired release uses 19.1.8 without retagging
previous commits or relabeling 19.1.5 assets.

## Verification

- Fresh native macOS 27 / Qt 6.11.2 / AppleClang build with Metal: 48/48
  CTests passed, including Cocoa GUI smoke in English, German and French.
- Early workflow version check: matching release and branch builds pass;
  the actual v19.1.7/VERSION=19.1.6 mismatch and stale README are rejected.
- Frozen Remote worker build and its self-test passed using an isolated Python
  3.13 environment. The host's Python 3.12 lacks SSL and was not used to ship.
- Distribution and remote publication verification remain pending.
