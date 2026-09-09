# Launchpad Snap and Debian build repair

Investigated on 2026-09-09 against `c52112cda2115e26947928c53d3511df069390ad`
(17.11.0). Noble Debian builds are excluded at the owner's request.

## Confirmed failure and fix

The settings dialog now clears the renderer's caches when resource limits
change. `prune_obj_mesh_cache` and `prune_opengl_mesh_cache` lacked `PVT_API`,
so the renderer's hidden symbol visibility prevented the GUI from linking
against its shared library. Both functions now have explicit exports, matching
the existing source-image and displacement-cache functions. Rendering and
cache algorithms are unchanged.

The same link errors appeared in:

- [Snap amd64 build 3275161](https://launchpad.net/~gnaservicesinc/procedural/+snap/procedural-visualizer-tool/+build/3275161)
- [Snap arm64 build 3275162](https://launchpad.net/~gnaservicesinc/procedural/+snap/procedural-visualizer-tool/+build/3275162)
- [Debian Resolute build 33582747](https://launchpad.net/~gnaservicesinc/+archive/ubuntu/proceduralvisualizertool/+build/33582747)
- [Debian Stonking build 33582787](https://launchpad.net/~gnaservicesinc/+archive/ubuntu/proceduralvisualizertool/+build/33582787)

An earlier [Snap amd64 attempt 3275151](https://launchpad.net/~gnaservicesinc/procedural/+snap/procedural-visualizer-tool/+build/3275151)
failed while cloning the Launchpad repository with a terminated TLS connection
and early EOF. Its retry reached the reproducible link failure above; the
earlier transport error does not require a project code change.

## Regression prevention

The desktop workflow previously built only static renderer libraries. Its Linux
x64 and ARM64 jobs now also build the full editor, CLI, and tests with
`BUILD_SHARED_LIBS=ON` and `PVT_DEPLOY_QT_RUNTIME=OFF`, as used by Snap and
Debian. The shared test runs require OpenGL through Mesa/Xvfb.

The asset-cache test calls all four cache-pruning entry points directly, as the
settings dialog does. It checks that cache eviction preserves handles held by
admitted renders, releases ownership after those handles expire, and preserves
exact CPU pixels across constrained and parallel rendering.

## Local evidence

- A fresh macOS ARM64 shared build, including the portable OpenGL backend,
  reproduced the GUI's two undefined-symbol errors before the fix.
- The updated asset-cache regression independently reproduced both missing
  exports before the fix.
- After the fix, the full shared build linked and all 40 CTest tests passed,
  including the asset-cache regression and English/German/French GUI smoke tests.
- `nm -gU` confirmed both symbols are externally visible in the shared library.
- A separate run with `QT_QPA_PLATFORM=cocoa` and `PVT_REQUIRE_OPENGL=1`
  passed the actual OpenGL acceleration/parity checks on Apple M2 Max. The
  offscreen CTest run cannot create an OpenGL context on this host.
- A staged shared-library install passed the CLI self-test and the external
  CMake consumer build/run. Because Qt deployment is deliberately disabled in
  this configuration, these probes supplied the installed Qt framework path
  through `DYLD_FRAMEWORK_PATH=/opt/qt/6.11.2/macos/lib`.
- Workflow YAML, the added Bash step, and `git diff --check` passed validation.
- Tests used `DYLD_FALLBACK_LIBRARY_PATH=/usr/lib` for this machine's local
  compiler runtime and `QT_QPA_PLATFORM=offscreen` for GUI smoke checks.

Raw API snapshots and logs are saved outside the source checkout at
`/opt/vrm/render/launchpad-investigation-20260909/`.

## Remote verification still required

The source fix must be imported from GitHub `main` into
[lp:procedural](https://code.launchpad.net/~gnaservicesinc/procedural/+git/procedural)
before requesting fresh builds. Both recipes already track that branch and
build automatically. Retrying an old build before import would use old code.

1. Verify the imported `main` revision includes this repair.
2. Verify fresh Snap amd64 and arm64 builds, including Store upload status.
3. Verify fresh Debian binary builds for Resolute and Stonking. A successful
   source-recipe build alone does not establish binary-package success.
4. Verify the added Linux shared-build CI checks.

At investigation time the available Launchpad browser session required Ubuntu
One sign-in for immediate import/rebuild requests. The next automatic import
was scheduled about five hours later. Local test results do not establish
successful Launchpad publication. No larger unresolved source defect was
identified during this focused packaging investigation.
