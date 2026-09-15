# PVT 19.1.15 / extensions 0.2.3: disconnect hotfix

PVT-RC and PVT-RD 0.2.2 removed the user-facing Disconnect action when persistent
pause states were retired. Do not use 0.2.2 until updated to 0.2.3 or later.

PVT now displays this warning in Networking & Remotes and connection details,
and marks extensions older than 0.2.3 (or without reported versions) unsupported.
Chrome update buttons open the existing store listings. Firefox update buttons
open each extension's release page while Mozilla store review is pending.
Check the offered version before installing; store approval is separate from
publishing the source and release archives.

## Restored connection controls

Both extension roles share the existing Connection transport. Disconnect closes
that transport and clears the display video. The tab remains disconnected until
Connect is clicked or a different host is selected. Reloading resumes automatic
connection; no persistent pause setting is introduced. Normal network interruption
recovery and legacy pause migration remain enabled.

Both Firefox 0.2.2 submissions were withdrawn through Mozilla's version API;
the authenticated version lists confirmed no remaining versions immediately
after withdrawal. The add-on listings and identities were retained.

19.1.15 completes the French and German translations required for desktop
publication; it supersedes the initial 19.1.14 warning-only tag.
