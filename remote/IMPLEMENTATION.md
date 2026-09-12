# Implementation and deployment status

PVT Remotes supports paired devices on the same network and same-computer use
without an external network. Initial setup exchanges two pairing files; subsequent
connections and recovery are automatic. No public service is required.

Implemented in the three local repositories:

- Separate React/Vite PVT-RC and PVT-RD extensions with Chrome MV3 manifests.
- Persistent WebRTC ownership in a dedicated pinned tab, also used by the
  generated Firefox/Safari targets; WebExtension storage API polyfill.
- Mutual public-file pairing, Ed25519 signatures, X25519/AES-GCM encrypted
  signaling, challenge authentication, replay rejection and transport bounds.
- Multi-host import/edit/remove/switch, local identity, opt-in public-profile
  browser sync with preference restoration, view/disable/clear controls.
- Real WebRTC video/audio and data channels, authenticated loopback WebSocket
  control, automatic LAN discovery with stable identity names and interface refresh.
- Existing opaque relay protocol preserved for provisioned deployments.
- Desktop Remote Manager exposed in Settings and Application Settings.
- Single active control identity with immediate permission enforcement,
  multiple displays, revocation, and selection in the normal MIDI Control Map.
- Shared Live scalar target registry for remote editing with normal validation,
  undo/redo, save/load and existing editor visibility.
- System-tray/menu-bar background operation using the existing image renderer,
  and an audio tap using existing output mixes.
- Regression fix for the additional timeline-driven render submissions that
  caused ~118 FPS in a 60 FPS project. Routine requests now share the Live timer;
  intentional authored edits remain immediate even at sub-Hz frame rates.

Engineering adjustments to the proposal:

- Public-key encryption needs X25519 in addition to Ed25519 signatures.
- An ordinary extension cannot browse arbitrary native mDNS services directly.
  Pairing files supply endpoints and `.local` resolution uses the operating system.
- The existing renderer already produces images independent of window visibility;
  a second render engine or new graphics-context subsystem is unnecessary.
- A pinned tab is the cross-browser WebRTC owner; no offscreen-to-popup
  MediaStream serialization is attempted.
- The shared transport is frozen at build time and bundled with PVT.
  User setup contains only the existing mutual pairing-file exchange.

Distribution:

- The desktop release includes the worker and its dependencies. Debian packages
  supply the same worker through automatically installed package dependencies.
- PVT-RC and PVT-RD publish Chrome, Firefox and Safari build archives.
- Cross-internet hosting and browser-store publication are not prerequisites
  for the supported same-network workflow.

See VERIFICATION.md for concrete automated checks and release evidence.
