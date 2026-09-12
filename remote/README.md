# PVT Remotes

PVT-RC controls authored project parameters; PVT-RD receives the existing stage's
video and audio. The extensions live in their own repositories. PVT owns the
shared protocol/client source, desktop integration and bundled WebRTC worker.
Networking is off by default. No listener, discovery announcement, or signaling
connection starts until the desktop preference is enabled.

## Pair once

1. In Remote Display or Remote Control, save the `.pvtremote` pairing file.
2. In PVT, open **Settings → Networking & Remotes** and import that file.
   Importing enables remotes. The first imported controller becomes active.
3. Save PVT's `.pvthost` pairing file and open it in the remote.

The remote connects automatically. Keep PVT running and the remote tab open.
Saved pairings and the selected PVT survive restarts; temporary interruptions
retry automatically. Disconnect pauses automatic connections until Connect is
selected again. Remove a paired device in the normal manager to revoke it.
Network and runtime configuration are not part of setup.

## Developer builds and packaging

Release applications include the transport executable and dependencies alongside
the desktop executable, under `pvt-remote/` (inside `Contents/Resources` on macOS). No end-user interpreter, package
installation, or download-on-first-use is used. Build the transport on each
target platform with a dedicated build environment:

```sh
python -m pip install ./remote 'pyinstaller==6.22.0'
python scripts/build-remote-worker.py --output /absolute/build/pvt-remote
cmake -S . -B build -DPVT_REMOTE_WORKER_DIR=/absolute/build/pvt-remote
```

The script freezes the shared worker, includes dependency license metadata, and
runs authenticated-loopback and actual system VideoToolbox H264 (macOS) or VP8 (other platforms), plus Opus encoding checks. CMake stages the
bundle with the GUI; installation refuses to create a desktop package without
it. The desktop CI and Snap build recipe produce the worker during their builds.
Debian packaging uses `PVT_REMOTE_SYSTEM_RUNTIME` to install this same worker
with distribution-managed dependencies. The package manager supplies them as part
of PVT installation; no runtime configuration or separate setup is needed.
Debian builds do not fetch dependencies from the network.

For source debugging only, `PVT_REMOTE_TEST_PYTHON` selects a developer interpreter.
The old persisted interpreter setting is ignored. The standalone extensions still
build with their documented developer toolchain; store distribution is separate.

## Desktop behavior

Remote parameter edits use `buildLiveTargetRegistry`, `pvt::validate`, and the
normal authored document/undo path. They survive saves and appear in existing
editors. Runtime-only Live overlays are still managed by Live Controls; remote
state reports authored scalar values rather than inventing another scene model.
Structural changes, file paths, asset uploads, exports and arbitrary scripting
are intentionally outside the scalar control protocol.

The existing MIDI Control Map target picker includes **Active Control Remote**
under Networking & Remotes. Slot 0 means None; subsequent slots follow the imported
control-profile order. The picker displays the names in that order. Revisit mappings
when removing profiles, since subsequent slots shift. A mapping can only select
an existing control profile. It cannot import, remove, or invent identities.
Controller handoff takes effect at every command check and preserves viewer
connections. Revoking a profile closes its WebSocket and WebRTC sessions.

**Host in background** hides the desktop and stage while preserving the existing
image renderer and GPU resources. A system tray/menu-bar menu restores the app
or quits through the normal unsaved-changes prompt. Platforms without a working
system tray reject hiding so the app remains locally recoverable. Closing to
tray is a separate opt-in preference. Hidden output uses the normal stage
freeze/blackout decisions, including native full-screen-space teardown.

Remote video is limited to 1280×720 at 30 FPS on macOS and 1920×1080 at 30 FPS elsewhere, using a latest-frame
JPEG bridge followed by WebRTC encoding. This adds a compression stage; it is not
a lossless reference-output channel. macOS 27.0+ uses VideoToolbox directly for H264 compression; PyAV supplies only the packet container and audio transport on Mac, and no bundled software video encoder is used. RC controls use bounded JSON messages,
with a raw WebSocket fast path only after authentication on actual loopback.
All non-loopback control travels over the WebRTC data channel.

Audio uses a bounded, allocation-free tap of the existing 48 kHz stereo mix:
normal playback audio when Live is off; the **first configured Live output mix**
when Live is on. Configure that mix in the existing Audio/Video routing matrix.
No new microphone capture or audio device is opened by remote streaming. No
routed output means silence. Browser audio starts muted and requires Enable audio.

## Automatic reachability and release boundary

Enabling remotes binds the listener automatically. The host enumerates active
interfaces, advertises an identity-derived `.local` name, and refreshes multicast
sockets and address records after interface changes. Loopback remains available
without an external network or successful multicast discovery. A small stable
set of identity-derived connection ports provides automatic conflict fallback;
both ends derive the same alternatives. Existing identities, pinned keys and
control permissions are preserved. A pre-repair pairing file may need a one-time
replacement to gain the stable identity-derived name after its old address changes.

The browser tries the paired endpoints and stable alternatives automatically,
then retries with bounded backoff. Opening the saved remote tab, selecting another
paired PVT, and restoring a connection need no manual Connect action. The operating system resolves the stable paired-device name; no network names
or connection settings are entered by the user.

The supported use case is PVT and its remotes on the same network, or on the
same computer without an external network. No public connection service is
required. The old optional relay protocol remains for compatibility with existing
profiles; it is not part of setup or a requirement for using PVT Remotes.

## Pairing and privacy

Version-1 pairing files contain a UUID, a label, Ed25519 and X25519 public keys,
and the role or host endpoints. Private identity keys remain local; they are never
exported or passed through sync. Desktop identity/config files use owner-only
permissions on POSIX; on Windows they inherit the user's application-data ACL.
Pairing files should be exchanged through a channel trusted to identify the other
device. Treat imports as trust grants. A profile's UUID cannot silently replace
pinned keys in the extension; remove/re-pair explicitly when rotating keys.

Ed25519 signs messages; it is not an encryption algorithm. X25519 plus
HKDF-SHA256 derives directional AES-256-GCM keys for SDP envelopes. Signed
recipient/sender IDs, timestamps, random nonces and a bounded replay cache reject
modified, misaddressed or repeated envelopes. Initial local connections also
prove possession against a fresh server challenge. Media uses WebRTC DTLS-SRTP.
The relay sees public UUIDs/registration keys, traffic timing and envelope sizes;
it cannot decrypt SDP. The protocol has automated negative and interoperability tests.

Browser sync is opt-in and contains only validated public host profiles and the
sync preference. It has view, edit, disable and clear controls. Reinstallation
restores synced hosts after a previous opt-in, but creates a new local private
identity: import the new `.pvtremote` in PVT before reconnecting. Pairing trust is
not magically restored by syncing a public host file. Explicit local-only devices
do not load host profiles from browser sync.

## Browser targets and shared code

Both repositories are standalone buildable checkouts. Their
`vendor/pvt-remote-client` directories are generated from `remote/client`:

```sh
python3 scripts/sync-remote-clients.py
python3 scripts/sync-remote-clients.py --check
```

`SOURCE.json` records SHA-256 values. Keep common protocol/storage/transport/UI
changes in the shared source, then sync and check both extension repositories.
The pinned-tab connection owner is used on all browsers. MV3 service workers
only open/focus the tab and never own an RTCPeerConnection or MediaStream.
Firefox loads `dist/firefox/manifest.json` through `about:debugging` for temporary
testing. Safari's `dist/safari` must be converted to a Safari Web Extension with
Xcode, signed and installed. Build parity is implemented; The release includes browser build archives. Browser-store publication is separate
from these downloadable builds.

References: [Chrome offscreen lifecycle](https://developer.chrome.com/docs/extensions/reference/api/offscreen),
[Web Crypto key agreement](https://developer.mozilla.org/en-US/docs/Web/API/SubtleCrypto/deriveKey),
[aiortc API](https://aiortc.readthedocs.io/en/latest/api.html).

## Verification

```sh
python -m unittest discover -s remote/tests -v
# Each extension:
pnpm test
pnpm build
# Desktop development integration fixture:
PVT_REMOTE_TEST_PYTHON=/path/to/python pvt-desktop --remote-smoke-test
# Installed Playwright and both built extensions:
PVT_PLAYWRIGHT_MODULE=/path/to/playwright/index.mjs PVT_REMOTE_PYTHON=/path/to/python \
  node remote/tests/browser_smoke.mjs
# Exercise encrypted relay signaling and the RTCDataChannel instead of localhost:
PVT_PLAYWRIGHT_MODULE=/path/to/playwright/index.mjs PVT_REMOTE_PYTHON=/path/to/python \
  node remote/tests/browser_smoke.mjs --relay
```

The desktop executable name depends on the platform/package (on macOS it is
`Procedural Visualizer Tool.app/Contents/MacOS/Procedural Visualizer Tool`). The
remote smoke uses temporary identities/settings and tests manager opt-in,
authorization, authored edits, stale-revision rejection, undo/redo, save/load,
hidden rendering, and controller deselection. Browser tests load real unpacked
extensions into an isolated Chromium profile and use real WebRTC audio/video.
