# Palette Remix translation review

Reviewed 2026-09-08 against the new English messages in `MainWindow` and
`PaletteRemixDialog`.

French and German cover the launch button, dialog controls and accessibility
names, comparison, reset, randomized settings, apply action, and explanatory
text. Review checked natural phrasing and consistency with the existing palette,
starting-color, alpha, dithering, and exposure terminology. French uses *IL*
for exposure stops; German uses *Blendenstufen*. Names authored by the artist
remain unchanged.

The text distinguishes temporary preview from an undoable applied edit, exact
reset from repeated color processing, display clipping from retained HDR values,
and unavailable artwork preview from still-usable palette swatches during LIVE
or a paused export. Automated checks cover source synchronization and completeness;
the native GUI smoke tests exercise Apply, Cancel, comparison, reset, and undo/redo
in all three shipped languages.

Validation: both released catalogs have 2,905/2,905 completed messages. The
37-test native macOS suite passed, followed by the six affected palette,
localization, and GUI checks after the final preview-status adjustments. Qt
snapshots of the dialog were inspected in English, French, and German at normal
and compact sizes; the scroll area keeps the actions accessible on small windows.
