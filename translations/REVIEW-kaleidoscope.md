# Kaleidoscope interface review

Reviewed 2026-09-08 by Codex against the English source and rendering behavior.

The 14 added messages in MainWindow, QObject, and RendererLabels are translated
in French and German. Review covered the distinction between repeated sectors
and their mirrored pairs, source zoom versus output scale, signed spiral twist,
mirror-axis rotation versus animated source rotation, and zero-cycle stills.
Existing translated center, phase, cycle, placement, and area controls are reused.

Terminology: French uses « kaléidoscope », « secteurs en miroir », « zoom de la
source », and « torsion en spirale ». German uses „Kaleidoskop“, „gespiegelte
Sektoren“, „Quellenzoom“, and „Spiralverdrehung“. The tooltips explain the less
familiar twist unit and how to hold a still pattern.

The translation checker and English, French, and German native GUI smoke checks
cover catalog consistency, discoverability, editor ranges, and Live targets.
This records model review, not independent native-speaker sign-off.
