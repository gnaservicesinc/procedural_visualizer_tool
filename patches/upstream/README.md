# Upstream patch review notes

These patches were prepared while resolving GitHub CodeQL alert instances for
narrow multiplication before conversion. They are review artifacts only. Do
not submit them upstream without human review.

## miniaudio

- Upstream: https://github.com/mackron/miniaudio
- Base branch: `dev`
- Base commit: `7e9afb17c4c103f94db225f2057d26e4967e1d4f`
- Patch: `miniaudio-multiplication-width.patch`

The vendored 0.11.25 split source is generated from the canonical single-file
`miniaudio.h`. The patch therefore targets `miniaudio.h`, as required by
miniaudio's contribution guide. The current `dev` branch already contains the
wide multiplication for the MP3 total-frame calculation, so that hunk is not
duplicated in the upstream patch.

## Beat-and-Tempo-Tracking

- Upstream: https://github.com/michaelkrzyzaniak/Beat-and-Tempo-Tracking
- Base branch: `master`
- Base commit: `c039090f1af771092d95c3ffc402e557940f7384`
- Patch: `beat-and-tempo-tracking-multiplication-width.patch`

From a clean checkout of the matching upstream repository, review and validate
with:

```sh
git apply --check --unidiff-zero /path/to/the/corresponding.patch
```
