# Gemini remote integration audit review — 2026-09-12

Reviewed the nine claims in `/opt/vrm/render/plans/g_report.txt` against the
desktop, Python worker/relay, shared browser client, and both extension builds.
The report was treated as a list of claims to verify, not implementation instructions.

| # | Verdict | Evidence and disposition |
| --- | --- | --- |
| 1 | Confirmed; fixed | The 4,096-key registration history never expired. The relay now evicts the oldest disconnected registration when the bounded cache fills. Active registrations cannot be evicted, recent key mismatches remain rejected, and reconnect cleanup preserves the replacement socket. Pairing-file keys remain independently pinned at both endpoints. |
| 2 | Confirmed capacity mismatch; fixed | The old limit counted outbound messages, not inbound offers, and relay clients do not exchange the direct-connection `hello`. Nevertheless, a host answering all 64 supported remotes exceeds 60 outbound messages. The relay now permits 256 messages/minute across recipients while retaining 60/minute to any one recipient. Both limits include traffic to offline recipients and require no trusted host-role declaration. |
| 3 | Partly correct; bounded memory cleanup added | Qt 6's front erase advances the data pointer rather than shifting the remaining bytes (`QArrayDataOps::erase` in the installed Qt 6.11.2 `qarraydataops.h`). The claimed quadratic front-removal behavior is false. A large JSON array also occupies one protocol line, and target state travels from desktop to worker, opposite this receive path. Retained capacity is real but bounded by the existing 4 MiB input limit. The bridge now squeezes buffers above 64 KiB after their remaining data falls below 64 KiB. Normal small-packet reuse and parsing remain intact. |
| 4 | Confirmed; fixed | JPEG encoding now preserves frame dimensions at or below 1920×1080. Larger landscape or portrait frames are reduced with the existing aspect-ratio rule. Actual encoded JPEG dimensions are checked by the bridge regression test. |
| 5 | Confirmed; fixed | Apply queues configuration while the worker starts and submits it on `ready`. Worker notifications refresh untouched fields and preserve edited drafts. On the first startup, only edited fields are submitted until saved configuration is known, so defaults cannot erase unseen paired remotes or network settings. Pairing edits become available once existing profiles have loaded. The worker's existing configuration merge, validation and persistence remain authoritative. |
| 6 | False for the current lifecycle; unchanged | `MainWindow` constructs `playback_timer_` at `gui/main_window.cpp:2424`, before `initializeRemotes()` at line 3003. There is no later assignment to null or deletion of that timer. Starting playback is not what creates it. The proposed pre-initialization command crash is unreachable through this workflow. |
| 7 | Traversal exists; claimed bottleneck unsupported | `buildLiveTargetRegistry(project_)` runs for a submitted Set command. The browser uses number fields with explicit form submission, not continuously transmitting sliders. The browser regression checks several typed values produce zero Set commands, then one click produces one command. No measured bottleneck supports a second registry cache with additional invalidation rules. |
| 8 | False user-workflow premise; unchanged | The same explicit Set action creates one authored project transaction and one undo entry. The full validation/persistence/undo path is intentional. Merging separately submitted edits would change existing undo behavior without a drag gesture to define transaction boundaries. Native smoke verifies edit, undo, redo and save/load. |
| 9 | Allocations exist; performance/leak claim unsupported | The 3,840-byte packet is 20 ms of 48 kHz stereo PCM16. The five-iteration loop drains available backlog; it does not generate five packets every tick unconditionally. The real audio callback uses the fixed-size `AudioStreamTap` with no allocations; base64/JSON encoding happens on the GUI consumer side with existing backpressure. No leak or measured CPU regression was demonstrated. The bounded transport is retained. |

## Artwork

Both extension repositories now retain the supplied original under
`artwork/PVT-RC.png` or `artwork/PVT-RD.png`, with generation instructions.
Seven PNG sizes (16, 32, 48, 64, 128, 256, 512) live in `public/icons`.
All Chrome, Firefox and Safari manifests declare extension and toolbar icons;
each page has a favicon, and the shared React header receives its role's icon.
The existing shared-client sync script updates both vendored copies and hashes.
The manifest fields follow the [Chrome manifest reference](https://developer.chrome.com/docs/extensions/reference/manifest).

## Verification

- Python: all 15 protocol, host and relay tests pass. Six relay regressions cover
  4,097 genuine signed registrations, cache eviction and pins, reconnect cleanup,
  all 64 recipients, retained aggregate/per-recipient limits, and window expiry.
  Running these six tests against the original relay produces four failures.
- Native Qt bridge regression passes: Apply before ready, continued draft editing,
  preservation of previously unseen saved port/profile values, and real JPEG
  dimensions for 640×360, 1920×1080, 3840×2160 and 720×1440 inputs.
- Native desktop build and existing GUI smoke pass. Extended remote desktop
  smoke passes worker restart/Apply/persistence, role authorization, stale edit
  rejection, authored edit/undo/redo/save/load, hidden rendering and handoff.
  Tests use isolated settings; the installed host libraries still generate
  pre-existing deployment-target warnings. Native runs use
  `DYLD_LIBRARY_PATH=/usr/lib` on this host.
- Both extensions: six protocol/storage tests each pass; all six browser bundles
  build. Original artwork copies, all declared PNG dimensions, packaged bytes,
  favicon paths, shared-client hashes and whitespace checks pass.
  Fresh `dist/chrome.zip` archives were also created and checked in both
  extension workspaces, replacing the stale RD archive.
- Real Chromium extension smoke passes through both loopback and encrypted relay:
  mutual pairing, 701-target transfer, Set-on-submit behavior, live WebRTC audio
  and video, loaded artwork and 390-pixel layout. Desktop and mobile screenshots
  were visually inspected.

Firefox and Safari bundles were built; their runtime behavior was not exercised.
No release, version bump, commit, push, or extension-store publication was performed.
