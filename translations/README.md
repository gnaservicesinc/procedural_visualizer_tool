# Localization and translation handoff

The framework is implemented. `pvt_en.ts` is the extracted English source
catalog, initially entirely unfinished. English source text is the fallback.
The current authoring pass begins with German (`pvt_de.ts`) and French
(`pvt_fr.ts`) core screens. Their finished entries were translated directly
from this English catalog; unfinished entries intentionally remain English
fallbacks until they receive the same level of review. Neither catalog is shipped
yet. Do not call a language release-ready until the checker reports every message
finished and a fluent reviewer approves it. Synthetic
catalogs in `tests/translations` exercise the implementation and are never linked
into the editor. Their bracketed English is not a real translation.

## Language selection

The editor tries the operating system's preferred UI languages in order, including
Qt's regional and script fallbacks, then uses English. An English preference in
the list stops the search. A saved **Application Settings > General > Language**
override takes effect on restart. The picker displays native language names and
locale codes for released catalogs. Missing individual messages fall back to their
English source while reviewing a catalog outside a release build.

`--language de`, `--language pt-BR`, or `--language system` overrides the saved
choice for one launch. `--localization-info` reports the resolved language,
available embedded catalogs, Qt translation status, and layout direction as JSON;
it fails if the required embedded application/Qt resources are missing.

Qt translates standard buttons/dialogs in the chosen application language and
mirrors widget layouts for right-to-left languages. Unavailable Qt translations
fall back to English. The user's numeric/date locale is retained. No network
service, telemetry, or locale downloads are involved. The preference is stored in
machine-local QSettings, never inside projects. Rendering math, model enums,
project formats, OSC addresses, and the CLI/library's diagnostic/API strings stay
stable. User-authored names, paths, and imported content are not translated.

## Translation priorities

The supplied Snap usage map is a geographic hint, not a measurement of language
preferences or relative user counts. It supports starting with German (`de`),
French (`fr`), and Persian (`fa`) alongside existing English. It does not justify
excluding languages that are not represented on the map.

Then cover Spanish (`es`), Brazilian and European Portuguese (`pt_BR`, `pt_PT`),
Simplified and Traditional Chinese (`zh_CN`, `zh_TW`), Japanese (`ja`), Korean
(`ko`), Arabic (`ar`), Hindi (`hi`), Bengali (`bn`), Russian (`ru`), Ukrainian
(`uk`), Italian (`it`), Turkish (`tr`), Indonesian (`id`), Vietnamese (`vi`),
Polish (`pl`), Dutch (`nl`), and Swedish (`sv`). Extend to Czech (`cs`), Danish
(`da`), Finnish (`fi`), Norwegian Bokmål (`nb`), Greek (`el`), Romanian (`ro`),
Hungarian (`hu`), Hebrew (`he`), Urdu (`ur`), Thai (`th`), Malay (`ms`), Tamil
(`ta`), Telugu (`te`), Marathi (`mr`), Gujarati (`gu`), Punjabi (`pa`), Swahili
(`sw`), Filipino (`fil`), Bulgarian (`bg`), Serbian Cyrillic and Latin (`sr`,
`sr_Latn`), Croatian (`hr`), Slovak (`sk`), Slovenian (`sl`), and more as review
capacity permits. This is a starting sequence, not a runtime whitelist. Any
Qt-supported locale can be added without C++ or CMake edits.

## Authoring workflow

Configure the normal GUI build with Qt >= 6.5 and its LinguistTools component.
Use the same Qt installation's tools throughout; substitute its bin path below.

```sh
cmake -S . -B build -DPVT_BUILD_QT_GUI=ON -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build --target pvt_update_translations
python3 scripts/translations.py add de --lupdate /path/to/Qt/bin/lupdate
```

Open `translations/pvt_de.ts` in Qt Linguist, or edit its XML with equivalent care.
The add command creates correct locale metadata and Qt plural-form slots from all
GUI sources, including macOS-only files, regardless of the build host. Keep the
`source`, `context/name`, `comment` disambiguation keys, and locale metadata intact.
Translate only the `translation` content. Complete every `numerusform` when a
message uses numerus. Leave uncertain or unreviewed entries `type="unfinished"`;
`lrelease -nounfinished` excludes them from local review payloads. A partial
catalog never enters a normal package because only locales listed in
`translations/released-locales.txt` are compiled.

`add` also accepts several locale codes in one invocation, which is useful when
starting a review batch:

```sh
python3 scripts/translations.py add de fr fa es pt_BR pt_PT \
    --lupdate /path/to/Qt/bin/lupdate
```

Each catalog is extracted from the original English GUI sources; never use a
different target catalog as a translation source.

Preserve `%1`, `%2`, `%n`, and `%L1` placeholders exactly, with their multiplicity.
Preserve rich-text tags, links, newlines, useful leading/trailing spaces, units,
format names, and keyboard shortcut syntax. Translate menu mnemonics (`&File`) to
a useful mnemonic in the target language without collisions within that menu.
Use a consistent glossary for layers, surfaces, effects, frames, clocks, audio
mapping, and LFOs. Avoid literal translations of technical terms that artists use
in their established form. Request fluent/native review and inspect long labels,
dialog wrapping, accessibility descriptions, complex scripts, and RTL layouts.

```sh
cmake -S . -B build
cmake --build build --target pvt_update_translations
python3 scripts/translations.py check --lupdate /path/to/Qt/bin/lupdate
cmake --build build --parallel
ctest --test-dir build --output-on-failure
# Use the app executable inside Contents/MacOS on macOS, or .exe on Windows.
build/procedural-visualizer-tool --language de --localization-info
build/procedural-visualizer-tool --language de
```

For an incomplete catalog under active review, configure a local preview build
explicitly. This cache setting is never enabled by packaging files:

```sh
cmake -S . -B build-translation-preview \
    -DPVT_TRANSLATION_PREVIEW_LOCALES='de;fr'
cmake --build build-translation-preview --parallel
build-translation-preview/procedural-visualizer-tool --language de
```

The checker reports completion counts and rejects stale source catalogs, extraction
warnings, inconsistent locale metadata, duplicate messages, empty finished entries,
missing or wrong Qt plural slots, broken placeholders, and changed rich-text
structure/links. It also rejects any catalog named in `released-locales.txt` unless
every message is finished and its Linux/macOS metadata is present. With `--lupdate`,
it asks that exact Qt installation for each
locale's required plural-form count, so validation remains aligned with the Qt
version used to build the application. It does not certify linguistic quality or
choose mnemonics for translators. It runs as a build dependency and a CTest when
Python 3 is available. Only Python's standard library is used. Reconfigure after
adding a catalog (CMake also watches the glob).
Normal builds compile English plus the catalogs named in `released-locales.txt`;
`pvt_update_translations` updates every authoring catalog.

After complete coverage and fluent/native review, add the locale code to
`released-locales.txt`, add the matching localized desktop/MIME and macOS metadata,
and rerun the full configure, build, test, and package checks. This explicit gate
keeps partial or draft translations out of GitHub, Snap, Debian/PPA, Windows, and
macOS packages.

## Adding interface text

Use `tr("Literal source")` in classes with `Q_OBJECT`, or
`QCoreApplication::translate("ExplicitContext", "Literal source")` in helpers.
Use `ClassName::tr`, not `object->tr`, where extraction cannot infer the receiver.
Use `QT_TRANSLATE_NOOP` for static tables with dynamic lookup. Do not translate
serialized values, object names, filenames, or protocol identifiers. Use
`tr("%n frame(s)", nullptr, count)` for plural-sensitive text, and supply complete
English plural forms in `pvt_en.ts` as well as the target languages. Existing
non-numerus summaries can be phrased with count-neutral labels when translating.

`gui/renderer_labels.h` provides GUI-only translation of renderer display names.
After adding/changing a renderer name, run `python3 scripts/translations.py labels`
and then `pvt_update_translations`. The core API's strings remain untouched.
Detailed low-level errors propagated from the C++ library still use the library's
English diagnostics; localizing those requires a future structured-error pass.

## Distribution

`cmake/Translations.cmake` compiles English and each catalog listed in
`translations/released-locales.txt` into a `.qm` and embeds it at `:/i18n`. Qt's
`qtbase_*.qm` files are embedded at `:/i18n/qt`. This is
the same payload in macOS bundles, Windows packages, Linux GitHub archives, Snap,
and Debian/PPA packages. It does not depend on a package's working directory,
translation install path, host language packs, or a writable installation.

GUI builds require Qt's translation files and fail at configure time if they are
missing. Set `PVT_QT_TRANSLATIONS_DIR` to their directory for nonstandard SDK layouts.
The Snap build points to its KDE Qt 6 SDK; Debian adds `qt6-tools-dev`,
`qt6-l10n-tools`, and `qt6-translations-l10n` build dependencies. GitHub's source-built
Linux ARM64 Qt now builds matching, checksum-pinned Qt Tools and Qt Translations.
Archive Qt installations already carry those modules. CLI/library-only builds
retain their existing dependency set. macOS `CFBundleLocalizations` is generated
from catalog filenames so Cocoa knows the languages embedded in the app.

Package checks run `--localization-info` from installed executables. On a Linux
Snap builder, validate both supported architectures and a confined launch after
real catalogs are added:

```sh
snapcraft pack --build-for=amd64
# Install the resulting local snap, then run:
snap run procedural-visualizer-tool --language de --localization-info
snap run procedural-visualizer-tool --language fa
```

Repeat on arm64. The current `kde-neon-6` core24 runtime and SDK snaps publish only
amd64 and arm64 (Snap Store channel metadata checked 2026-09-05). Supporting armhf,
ppc64el, s390x, or riscv64 would require replacing/building the desktop dependency
stack, so this change does not broaden architectures. Sources:
[Canonical's extension](https://github.com/canonical/snapcraft/blob/main/snapcraft/extensions/kde_neon_6.py),
[Qt SDK](https://snapcraft.io/kde-qt6-core24-sdk),
[KDE SDK](https://snapcraft.io/kf6-core24-sdk).

Desktop entry `Name[locale]`/`Comment[locale]` fields, MIME descriptions, Snap Store
listing copy, and macOS privacy prompts are separate platform metadata. Translate
them alongside reviewed application catalogs. For macOS, copy
`translations/macos/en.lproj/InfoPlist.strings` to a matching `<locale>.lproj`
directory and translate the values; CMake automatically embeds these files in the
bundle, including distribution staging. Keep the permission-description keys
unchanged. Use Apple's hyphenated locale spelling, such as `pt-BR.lproj`, to match
the generated bundle language list. The Linux desktop entry and MIME XML already install verbatim, including
any localized fields added to them. Store listing copy is managed in the store.
Catalog framework validation does not certify translated typography or native
platform prompts.
