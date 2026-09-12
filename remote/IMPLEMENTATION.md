# Implementation and deployment status

Implemented in the three local repositories:

- Separate React/Vite PVT-RC and PVT-RD extensions with Chrome MV3 manifests.
- Persistent WebRTC ownership in a dedicated pinned tab, also used by the
  generated Firefox/Safari targets; WebExtension storage API polyfill.
- Mutual public-file pairing, Ed25519 signatures, X25519/AES-GCM encrypted
  signaling, challenge authentication, replay rejection and transport bounds.
- Multi-host import/edit/remove/switch, local identity, opt-in public-profile
  browser sync with preference restoration, view/disable/clear controls.
- Real WebRTC video/audio and data channels, authenticated loopback WebSocket
  control, opt-in LAN listener/mDNS advertisements, host-file LAN endpoints.
- Self-hostable opaque signaling relay and configurable STUN/TURN support.
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
- The optional Python/aiortc transport is separate from Qt/device/render ownership.
  Its runtime must be installed; it is not silently downloaded on application launch.

Deployment/validation still requiring a target environment:

- Public relay hosting, TLS/domain setup and TURN provisioning.
- Connectivity across actual external NAT/firewall combinations.
- Firefox and Safari runtime acceptance; Safari conversion/signing and store
  packaging. Generated manifests and bundles alone do not establish runtime parity.
- Release packaging of the optional Python runtime if a single-download installer
  is desired. The included installer currently prepares an isolated local runtime.
- Independent cryptographic review of the application-level signaling protocol.

Public relay deployment, version/release publication and browser store
submissions are outside this local verification. See VERIFICATION.md for the
completed checks.
