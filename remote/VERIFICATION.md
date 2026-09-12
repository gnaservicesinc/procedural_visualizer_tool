# Local verification — 2026-09-12

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
