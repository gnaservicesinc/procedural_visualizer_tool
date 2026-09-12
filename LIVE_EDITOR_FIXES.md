# Live editor fixes — 2026-09-12

Base: `ddaec9e` (working tree version 19.1.1). The user reported the behavior
in installed 19.1.0, including screenshots showing large colored pixels in the
main editor while the companion Live monitor was smooth.

## Confirmed causes and fixes

- The main editor enlarged Live's reduced-resolution frame with nearest-neighbor
  sampling. The monitor and stage used smooth sampling. Live frames now select
  the same smooth display policy in the editor; ordinary editor frames restore
  their existing policy. This changes display sampling, not authored block size
  or export rendering.
- Ordinary editor edits waited for the next performance frame. A 0.1 fps rig
  could conceal an edit for ten seconds. Editor changes now request an immediate
  Live frame; a new document revision cancels obsolete rendering work.
- Old mapping and scene overrides could conceal subsequent authoring edits.
  Registry refresh now releases edited or removed targets, including targets in
  an unfinished scene transition, while preserving unrelated performance values.
- Relative and toggle controls now begin from the current setting. First-input
  smoothing begins there too; repeated identical input no longer restarts its
  ramp. A new mapped input supersedes a scene transition on that target.
- Disabling, removing, or rebinding a mapping releases its old output. Runtime
  connection identity survives unrelated row deletion and renaming, preserving
  independent button states and distinguishing duplicate connections.

## Verification

- New native GUI regressions fail with the previous display code and previous
  editor-update scheduling. The mapping regression fails with the previous
  relative-control behavior. Those checks pass with the fixes.
- Blank-project GUI coverage uses real editor widgets at 0.1 fps, verifies frame
  pixels, undo/redo, and three Live open/close cycles without authored-value loss.
- Existing Live renderer tests compare RGBA bytes against core rendering for
  block sizes 0, 0.5, 1, 3, and 6, with both frame and phase clocks.
- Native build passed. All **46/46 CTest tests passed**, including English,
  German, and French GUI smoke checks. `git diff --check` passed.
- Build: `/tmp/pvt-lfo-defaults/build`; final test log:
  `/tmp/pvt-live-edit-ctest.log`. Reproduction logs:
  `/tmp/pvt-live-red.log` and `/tmp/pvt-live-delay-red.log`.

The user's broader intermittent report that any numeric widget could jump was
not independently reproduced as an authored-value mutation. The display
mismatch, delayed updates, and stale overrides above were demonstrated.

Changes are uncommitted. No version bump, release, or replacement of the running
installed app was performed.
