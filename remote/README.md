# PVT Remotes

PVT-RC controls authored project parameters; PVT-RD receives the existing stage's
video and audio. The extensions live in their own repositories. PVT owns the
shared protocol/client source, desktop integration and optional WebRTC worker.
Networking is off by default. No listener, discovery announcement, or signaling
connection starts until the desktop preference is enabled.

## Run locally

1. Build PVT normally. Install the optional transport with Python 3.11 or newer:
   `python3 scripts/install-remote-worker.py`. This creates an isolated environment
   under `~/.local/share/pvt-remotes/venv`, which the desktop detects automatically.
   For another environment, install `./remote` with pip and enter its Python
   executable in Networking & Remotes. Installing does not enable networking.
2. In each extension repository, run `pnpm install --frozen-lockfile` and
   `pnpm build` (Node 22+ and the pinned pnpm version from package.json).
   Chrome: enable Developer mode at `chrome://extensions`, choose **Load unpacked**,
   and select that repository's `dist/chrome` directory.
3. Open the extension from its toolbar action. It creates or focuses one pinned
   tab. In **Hosts & settings**, export its `.pvtremote` public identity.
4. In PVT, open **Settings → Networking & Remotes** (also available from the
   Remotes tab in Application Settings). Start the worker if necessary, import
   each `.pvtremote`, choose the **Active Control Remote**, and Apply.
5. Enable **Networking & Remotes**, Apply, then export `.pvthost`. Import the host
   file in each extension and Connect. RC can view all shared Live registry
   parameters; only the selected controller can edit, play, change Live state,
   or undo/redo. RD receives audio/video and can toggle background mode.

Host profile export reflects the last successfully applied settings. Updating
host endpoints or the relay URL requires exporting/importing the host file again
(or editing those public endpoint fields in the extension).

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

Remote video is currently limited to 1920×1080 at 30 FPS, using a latest-frame
JPEG bridge followed by WebRTC encoding. This adds a compression stage; it is not
a lossless reference-output channel. RC controls use bounded JSON messages,
with a raw WebSocket fast path only after authentication on actual loopback.
All non-loopback control travels over the WebRTC data channel.

Audio uses a bounded, allocation-free tap of the existing 48 kHz stereo mix:
normal playback audio when Live is off; the **first configured Live output mix**
when Live is on. Configure that mix in the existing Audio/Video routing matrix.
No new microphone capture or audio device is opened by remote streaming. No
routed output means silence. Browser audio starts muted and requires Enable audio.

## LAN and internet

LAN opt-in binds the signaling listener to IPv4 interfaces and advertises
`_pvt._tcp.local.`. Exported host profiles carry loopback, `.local`, and discovered
IPv4 endpoints. Ordinary extension pages cannot enumerate native mDNS services;
this implementation uses OS `.local` resolution and the paired host's endpoints.
No public service or STUN server is needed on the same LAN. Changed IP addresses
can be repaired through the existing profile editor or a fresh host export.

For internet access, deploy the included opaque relay on your own server:

```sh
python -m pvt_remote.relay --bind 127.0.0.1 --port 8787
```

Put a TLS WebSocket reverse proxy in front of it, configure its `wss://` URL in
PVT, and re-export the host profile. The relay is a single-process deployment;
it supports 256 connections and remembers up to 4096 registered identity keys
in memory. Restarting clears relay registrations, not desktop pairing. Configure
reverse-proxy per-IP connection/rate limits and do not expose the plaintext
backend port publicly. Public hosting and TLS/domain provisioning are deployment
steps, not performed by this repository.

Configure STUN/TURN servers in PVT as an ICE-server JSON array, for example:

```json
[{"urls":"turns:turn.example.org:5349","username":"allocated-user","credential":"allocated-secret"}]
```

Use your provider's actual endpoint and credentials. TURN credentials stay in the
host's private configuration and are not exported or synced. TURN is required
for networks where direct ICE connectivity is blocked. Public NAT traversal and
third-party TURN services require testing in the intended deployment environment.

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
it cannot decrypt SDP. This application-layer protocol has automated negative
and interoperability tests, but has not received an independent cryptographic audit.

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
Xcode, signed and installed. Build parity is implemented; Firefox and Safari
runtime/store certification are separate validation gates. No stores are published.

References: [Chrome offscreen lifecycle](https://developer.chrome.com/docs/extensions/reference/api/offscreen),
[Web Crypto key agreement](https://developer.mozilla.org/en-US/docs/Web/API/SubtleCrypto/deriveKey),
[aiortc API](https://aiortc.readthedocs.io/en/latest/api.html).

## Verification

```sh
python -m unittest discover -s remote/tests -v
# Each extension:
pnpm test
pnpm build
# Desktop (with the optional package installed in this interpreter):
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
