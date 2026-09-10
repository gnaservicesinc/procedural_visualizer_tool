# PVT Beat Tracker

Maintained source: https://github.com/gnaservicesinc/PVT-Beat-and-Tempo-Tracking

Vendored source commit: `f0bed31dec980daca6fd5b82b900a03da4903aca`.

PVT now consumes our maintained tracker, including `PVTOnset.h` and its original
onset front end, callback timestamp/reset corrections, and the standalone tests.
The legacy tempo/beat observer remains derived from Michael Krzyzaniak's library;
its MIT license and attribution are retained. This is an incremental replacement,
not a claim that all inherited DSP has already been rewritten.

The canonical development checkout is the sibling `PVT-Beat-and-Tempo-Tracking`.
From PVT, vendor or verify its explicit source/test manifest with:

```sh
python3 scripts/sync-beat-tracker.py ../PVT-Beat-and-Tempo-Tracking
python3 scripts/sync-beat-tracker.py ../PVT-Beat-and-Tempo-Tracking --check
```

PVT's `pvt_beat_tracker` CTest entry runs the same tests as the standalone tracker.
The original multiplication-width patch under `patches/upstream` is historical
review material; it is already included and must not be reapplied.

See the maintained repository's `DESIGN.md` for module contracts and the next
steps toward an independently maintained tempo/beat implementation.
