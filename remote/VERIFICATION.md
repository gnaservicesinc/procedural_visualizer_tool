# Remote setup repair verification — 2026-09-15

Supported scope: PVT and its remotes run on the same computer, share a nearby
network, or use an already-routed managed network whose administrator applies
the generated rules. A hosted public-internet connection service, NAT traversal,
independent firewall certification and independent cryptographic review are not
release requirements.

| Requirement | Implemented behavior | Evidence / remaining acceptance |
| --- | --- | --- |
| Pair once without technical prompts | Existing mutual pairing files; multi-file desktop import; automatic enable and first-controller selection; no interpreter, hostname, connection-port, signaling or media-server fields | Native manager test exercises a two-file selection and asserts the technical port controls are absent; actual file dialogs import/export/remove |
| Keep settings predictable | Remote choices apply as they are entered; one outer OK/Cancel button row; Cancel restores the opening worker configuration, enabled state, background state and close behavior | Native manager test changes live settings, observes the worker update, cancels, and verifies rollback; no nested Apply button is present |
| Connect across managed networks | Plain-language help appears only beyond nearby networks; custom ranges appear only for the custom choice; generated administrator handoff lists actual stable PVT ports and routed TCP/UDP needs; local setup files cover Windows, macOS and Ubuntu | Pure generator checks cover all three systems and stable identity-derived ports; manager visibility checks cover nearby/custom/private choices |
| No separate runtime installation | Frozen shared worker alongside PVT; no production PATH/interpreter fallback; desktop packaging requires it | macOS worker self-test passes with an empty environment and unusable PATH; native GUI integration uses bundled executable; release package results are recorded below |
| Keep normal project authoring | Existing Live targets, validation, document edits, undo/redo and persistence | Native remote smoke passes controller/display permissions, stale revisions, edit/undo/redo/save/load and hidden rendering |
| Remember pairing and recover | Saved selection, automatic connect on open/import, bounded retry, worker restart, explicit persistent Disconnect | Real Chromium audio/video test covers tab reload, desktop disable/enable recovery, and pause/reload/resume; native subprocess crash test passes |
| Automatic local discovery | Stable identity-derived name/port alternatives, interface enumeration and multicast refresh | Browser test uses the stable `.local` name; unit tests simulate interface/address replacement and occupied port |
| Same machine without external network | Loopback remains available without discovery | Unit test simulates no external interfaces and listener conflict; standalone loopback authentication/codec test passes; no physical network interface was disabled |

macOS 27 update checks: the 49-test native suite passed; lower deployment targets
are rejected at configuration; the direct VideoToolbox backend passed H264
encoding, color, resolution-change and keyframe recovery checks. Real Chromium
received H264 frames and passed stable-name connection, reload, interruption
recovery and explicit pause/resume. Final package publication is recorded after
release verification. The native Cocoa pairing smoke also passes against the
bundled worker, including file import/export, removal/re-import, roles, undo/redo,
save/load and background rendering. macOS uses Qt 6.11.2: Qt 6.8's legacy AGL
link dependency is absent from the current macOS SDK.

Earlier local checks:

- Protocol, host and relay unit tests cover legacy pairing migration, removal of
  obsolete remote-source port limits, failed-import rollback, stable discovery
  updates and automatic connection-port fallback.
- Both extension repositories pass all 6 protocol/storage tests and generate
  Chrome, Firefox and Safari bundles from the same shared client hashes.
- Focused native CTest passes bundled-worker, Remote Manager (including worker
  crash/restart), Live workspace and allocation-free audio tap checks.
- Actual native pairing-file dialogs pass against the bundled worker. It handles
  public export, removal/revocation and re-import/automatic enable correctly.
- Real isolated Chromium tests pass loopback, stable-name LAN and encrypted
  relay fixtures with 701-parameter state, control commands, actual WebRTC
  encoding/decoding, automatic reload/reconnect, persistent pause and mobile layout.
- CMake installation into a temporary prefix passes after correcting symlink
  preservation during staging. All 37 runtime symlinks match the source bundle,
  and the installed transport passes its self-test with an empty environment
  and unusable PATH. This is an installed development artifact, not a signed
  distributable application acceptance result.
- The worker's clean-environment self-test authenticates a real local socket and
  encodes VP8 video and Opus audio without a system interpreter or package path.
- Windows ARM64 dependency preflight found missing accelerated discovery/CRC
  wheels. CI selects those dependencies' supported pure-source builds. The
  discovery source wheel was built locally and verified to contain no native
  extension; this does not replace the Windows ARM64 runtime gate.
- French/German catalogs updated through normal extraction; the build's released
  translation completeness check passes. Shared-client drift/whitespace checks pass.

Local native builds use Qt 6.11.2 on macOS. The existing development checkout
requires `DYLD_LIBRARY_PATH=/usr/lib` for its C++ test binaries and emits existing
host-library deployment-target warnings. This is not evidence of a verified
signed distribution or minimum-OS compatibility. Browser video is a generated
640×360 fixture; native smoke separately verifies the real renderer and document.

Release validation uses the existing automated desktop package jobs and extension
builds. The native and browser tests cover the supported shared-network workflow;
these are engineering checks performed by the project, not setup steps for users.

## Historical verification before this repair


- Native desktop build: passed, Qt 6.11.2/macOS, with existing host-library
  deployment-target warnings. Used DYLD_LIBRARY_PATH=/usr/lib for native tests.
- Native Remote Manager smoke: passed opt-in, role enforcement, authored edits,
  stale-revision rejection, undo/redo, save/load, hidden rendering and controller
  deselection using temporary settings and identities.
- Existing GUI smoke: passed in English, German and French.
- Live FPS regression: red before fix (364 frames/650 ms at configured 60 FPS);
  green after fix for Live and presentation at 60 and 23.976 FPS under repeated
  2 ms refresh requests. Paused refresh and existing sub-Hz editing/startup checks
  pass. Intentional edits can still request immediate frames; routine playback
  refreshes share the output timer.
- Live workspace, scene morph, audio routing and audio stream tap tests: passed.
- Python protocol/host tests: 9 passed.
- Extension protocol/storage tests: 6 passed in each repository.
- React/Vite builds: both extensions generated Chrome, Firefox and Safari bundles.
- Real Chromium extension tests: passed direct loopback and encrypted relay
  connections, JS/Python crypto interoperability, public-file import, separate
  identities, control commands, real WebRTC audio/video tracks and 390 px layout.
- Large project: 701 parameters transferred successfully, including chunked
  replies over the relay-connected RTCDataChannel.
- Shared client hash/drift check and git whitespace checks: passed.

The browser uses an isolated test profile; native smoke uses temporary settings.
Video in the transport browser test is a generated 640×360 fixture, while native
smoke separately verifies actual PVT rendering and edit/persistence integration.
Public NAT/TURN deployment and Firefox/Safari runtime certification are not
established by these tests. See README.md and IMPLEMENTATION.md for setup and
remaining deployment requirements.

## Final macOS 27 build repair

The old AGL link dependency is eliminated by using Qt 6.11.2. The native
VideoToolbox library now has a portable install name. GitHub run 34713543947
built and verified the macOS 27 package and passed all 46 non-GUI native tests;
its later GUI smoke exposed a native full-screen transition race. All four
Windows/Linux jobs passed.

A deterministic regression then reproduced a real startup race: output metrics
cancelled a submitted frame but the FPS gate delayed its replacement. Geometry
changes and explicit frame resets now request their replacement immediately.
The regression passed after failing before the fix; all three translated GUI
smokes passed locally. The full-screen test waits for native window state.

PVT-RC and PVT-RD 0.1.1 are published. All six downloaded extension archives
match their published SHA256 checksums. Desktop tagged CI and publication are
tracked by the v19.1.6 release workflow.

## Support policy clarification

macOS 27+ is the supported/tested baseline, not a deny-list. Explicit OS-version
checks and the policy-only 27 deployment floor have been removed. The native
API floor is 15 (unguarded ProRes 4444 XQ); package checks still reject actual
dependencies newer than the declared deployment target. No old-OS fallback or
dependency pin is introduced. Earlier enforcement statements are superseded.
