# French and German review for 17.4.0

Review date: 2026-09-06. Reviewer: Codex (AI linguistic review, not an
independent human/native-speaker sign-off).

## Scope and decisions

Both catalogs were read against the original English sources, including all 503
previously finished messages per language and all 2,336 unfinished messages per
language. The pass checked meaning, grammar, idiom, tone, terminology, and
consistency across editor controls, contextual help, Live performance, audio,
export, saved-project workflows, renderer labels, and internal test diagnostics.
The final catalogs have 2,842 messages each: three fragmented rename strings
became two complete sentences, and three standalone smoke-test fragments
were removed in favor of comparing complete translated messages. Seven
previously unextracted wave/effect/swing status labels were also localized after
visual review found English on/off/sync/free labels in otherwise translated lists.

French uses polite imperative instructions, *calque* for an image layer,
*plan* for a geometric plane, *battement* for a beat, *horloge* for a clock,
*affectation* for a control mapping, and *placage* for surface mapping.
German uses *Ebene* for an image layer, *ebene Fläche* for the geometric plane,
*Takt* for a clock, *Zuordnung* for a control mapping, and
*Oberflächenabbildung* for surface mapping. Particle silhouette, blur, alpha,
input gain, response, and last-good-frame terminology was reconciled across the
previous drafts and new text. Technical names such as MIDI, OSC, LFO, Qt, PNG,
OpenEXR, sRGB, Smoothstep, and codec names remain recognizable.

Counts in non-numerus source messages use count-neutral wording where needed.
There are currently no numerus messages in the production catalogs; Qt plural
handling remains covered by the synthetic localization tests. Menu-bar mnemonics
are distinct within each language. Placeholder multiplicity, line breaks,
boundary spaces, escaped ampersands, file filters, rich-text markup, and links
were checked. Intentional identical translations were reviewed: shared words,
units, identifiers, format names, and placeholder-only strings remain unchanged.
The `Add` label deliberately differs between an add-item action and the additive
blend mode. Linux desktop/MIME metadata and macOS permission descriptions were
also reviewed against their English originals.

The saved-project rename prompt now translates complete sentences with positional
placeholders rather than combining localized fragments with English quotation
marks. Cancellation, particle-placement, and recovery-warning smoke assertions compare
complete translated messages, so grammatical inflection and German word order
cannot cause false failures.

## Validation

The English catalog remains the extraction template and fallback. French and
German are explicitly enabled in `released-locales.txt`; normal builds embed
both application catalogs and Qt standard-dialog translations. The new
`pvt_released_localizations` test checks the real GUI executable, and the desktop
workflow repeats that check against installed packages on all five platforms.

Local build, runtime, and visual validation results are recorded below before
the release commit. Remote CI and downloadable artifacts are not certified by
this linguistic review record.

The preceding localization-framework CI run (34012256950) failed on Linux:
the installed x64 package contained the xcb platform plugin, but its locale probe
requested offscreen; the ARM64 Qt Linguist build found an incomplete Clang 18
development installation. Package probes now use the installed xcb plugin under
Xvfb, and source-Qt runners explicitly install the Clang/LLVM 18 development
packages. macOS and both Windows jobs passed that preceding run.

Local validation completed on macOS arm64:

- Source extraction/freshness and catalog validation passed: 2,842/2,842 finished
  messages in each released catalog, with placeholder and formatting checks.
- All 36 CTest checks passed, including English, French, and German GUI smoke
  checks and the real-executable released-localization probe. The host beta
  toolchain required `DYLD_LIBRARY_PATH=/usr/lib` for the development test build.
- The final distribution passed the self-contained bundle verifier (50 Mach-O
  files), deep/strict codesign verification, CLI version 17.4.0, and CLI self-test.
  The packaged executable loaded embedded French/German application and Qt
  translations and passed both localized GUI smoke checks without that override.
- Final native screenshots of French Movement and German Layer Effects were
  inspected for wording, wrapping, clipping, and translated list statuses.
  This was screenshot/code review, not an exhaustive interactive UI review.
- The local ZIP passed archive integrity checks and contained only the app,
  README, and license at its root, with `pvt-render` inside the app bundle.
