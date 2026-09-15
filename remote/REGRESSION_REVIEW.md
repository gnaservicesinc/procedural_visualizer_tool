# Remote regression repair — 2026-09-15

## Recommendation

Repair the shared remote workflow rather than simply reverting release 19.1.12
(`2573be5`). Exclusive controller selection dates to `c2e6612`; persisted device
pauses were introduced in `41a31df`. Reverting the final release commit alone
leaves those failures in place. Reverting the broader feature work would also
remove unrelated photo functionality. No release, tag, commit or push was made
as part of this repair.

## Report dispositions

| Report | Repair |
| --- | --- |
| 1. Unidentifiable saved devices | Saved entries show remote type, browser, system, last endpoint and full identity; generic editable names are not used as device identity. New Remote exports include browser metadata. |
| 2. Disconnected devices do not reconnect | Remove persisted connection blocks; retain automatic retry after interruption, process restart and reload; recover repeated unanswered commands. Invalid saved browser host selection falls back to an available imported host. |
| 3. “Allowed to control PVT” | Remove the selector, native/worker authorization gate and MIDI selection target. All imported control profiles can edit. Display roles, removed-profile revocation and normal project validation still apply. |
| 4. RC describes Remote Display | RC settings describe control of project/layer parameters. The separate companion-store link is explicitly labeled “Also available”. |
| 5. Manual reconnect | Remove reconnect/resume controls. Removing the saved Remote profile revokes access. |
| 6. Vague nearby-network wording | Use “This computer and devices on the same IP subnets”, matching actual interface masks and loopback admission. |
| 7–8. Hidden connection details and misplaced emphasis | Keep a one-click Remote connection details button visible in the status bar. The same live table appears first in Remotes; setup and saved profiles are under a collapsible management section. |
| 9. Browser/system displayed as slashes | Merge nonempty metadata from the session, authenticated handshake and saved profile. Older direct clients use their WebSocket browser headers as a descriptive fallback. Missing metadata is explicit; it is never invented. |
| 10. Raw data-use numbers | Show rates in kb/s or Mb/s, cumulative sent/received data in B/KB/MB/GB, and latency in ms. Five wrapped columns keep the useful values visible together. |
| 11. Misleading support wording | Name the view Remote connection details and describe it as live measurements. |
| 12. Invalid Paused state | Remove the state and controls in both desktop and extensions. Discard old desktop selections/pauses and browser pauses on load. Legacy pause messages cannot recreate the state. Connection status continues to reflect the actual transport, rather than pretending a disconnected socket is connected. |
| 13. Repeated disconnects | Replace per-profile WebRTC ownership with per-session ownership. Multiple tabs sharing one imported profile no longer replace one another’s connections. Give transient ICE interruptions time to recover and retry failed/unresponsive transports. |
| 13 (file labels). Missing/confusing import | Restore explicit Export Remote file (.pvtremote) and Import PVT host file (.pvthost) labels in extensions, with reciprocal labels in PVT. Existing file formats and public keys remain compatible. |

## Related reliability findings

- Renaming or refreshing descriptive profile metadata no longer revokes an active
  session. Removing a profile or changing its keys/role still closes its sessions.
- Client metadata persists through the existing profile storage so disconnected
  devices remain recognizable; it is descriptive and never an authorization key.
- Late encrypted signaling replies from a replaced socket are ignored.
- All controller edits still use the native Live registry and normal revision,
  validation, undo/redo and document persistence paths.
- The old `active_control` response field is retained solely for older extension
  compatibility, returning the requesting control identity. There is no saved
  selection or exclusive-controller authorization logic.

## Verification

- Worker protocol/access/relay/codec suite: 31 tests pass, including legacy-state
  migration, all imported controllers, removed-profile revocation, browser-header
  fallback, real ICE admission and VideoToolbox encoding.
- Native build and released German/French translation completeness checks pass.
- Five focused native tests pass: remote manager/process recovery, Live workspace,
  audio routing, audio stream tap and localization. The manager test verifies
  both imported controllers, readable units and removal of the control selector.
- Actual desktop remote smoke passes import/export, remove/re-import, automatic
  enable, role checks, stale edits, undo/redo, save/load and hidden rendering.
- Real Chromium tests pass loopback, stable `.local` discovery and forced relay
  signaling with direct paths disabled, control editing,
  audio/video, reload, desktop interruption/re-enable, legacy browser pause removal,
  and two same-profile display tabs sustained beyond two reported 14-second cycles
  with no replacement peer connections. Settings draft/save/discard and mobile
  layout checks also pass.
- Both extension repositories build Chrome, Firefox and Safari bundles from the
  same shared source; each repository’s 13 existing tests pass.
- Native and browser screenshots were inspected. This is local verification on
  macOS, including same-machine `.local` routing, not physical multi-machine or
  Firefox/Safari runtime certification. Remote CI and published packages were not
  evaluated by this repair.
