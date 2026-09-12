# Remote setup repair verification — 2026-09-12

Supported scope: PVT and its remotes share a network, or run on the same computer
without an external network. A hosted connection service, external NAT/firewall
certification and independent cryptographic review are not release requirements.

| Requirement | Implemented behavior | Evidence / remaining acceptance |
| --- | --- | --- |
| Pair once without technical prompts | Existing mutual pairing files; automatic enable and first-controller selection; no interpreter, hostname, port, signaling, LAN permission or ICE fields | Native manager test asserts no editable text/network fields; actual file dialogs import/export/remove; generated manager screenshot inspected |
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
release verification.

Earlier local checks:

- 19 protocol, host and relay unit tests pass, including legacy pairing migration,
  failed-import rollback, stable discovery updates and automatic port fallback.
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
