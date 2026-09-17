# PVT 19.1.15 / extensions 0.2.3: resolved disconnect hotfix

PVT-RC and PVT-RD 0.2.2 temporarily removed the user-facing Disconnect action
when persistent pause states were retired. Version 0.2.3 restored the action.

The temporary desktop warning and version-based unsupported marker have since
been removed. PVT keeps protocol-compatible current extensions connected and
enables newer host-managed media and connection actions only when an extension
advertises the corresponding capability. Update buttons remain available without
presenting an old release incident as a current safety warning.

## Restored connection controls

Both extension roles share the existing Connection transport. Disconnect closes
that transport and clears the display video. The tab remains disconnected until
Connect is clicked or a different host is selected. Reloading resumes automatic
connection; no persistent pause setting is introduced. Normal network interruption
recovery and legacy pause migration remain enabled.

19.1.15 completes the French and German translations required for desktop
publication; it supersedes the initial 19.1.14 warning-only tag.
