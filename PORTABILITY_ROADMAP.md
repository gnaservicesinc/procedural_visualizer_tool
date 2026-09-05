# Cross-platform GPU and video roadmap

This document turns the Linux and Windows parity goal into implementation
boundaries. It deliberately does not claim a backend is complete merely because
it compiles in a software-rendered virtual machine.

## Selected direction

### Current acceleration work (17.2.0)

OpenGL remains a vendor-neutral path for Windows/Linux GPUs with a suitable
OpenGL 3.3 driver. Generated RGB now shades the block grid and expands on GPU;
alpha remains per pixel. Direct row-order readback removes CPU row swaps from
generated, Water, and analytic passes. This reduces work without requiring
compute shaders, new SDKs, or an architecture-specific binary.

`PVT_ENABLE_QT_OPENGL_WITHOUT_GUI=ON` enables the same renderer for a CLI or
library build using Qt Gui without compiling the editor or requiring Widgets.
The default core build remains free of Qt. Runtime context availability is
separate from compilation: `pvt-render --renderer-info` and Performance's
**Copy renderer report** expose both. Software OpenGL is still usable for
validation but is not evidence of physical GPU acceleration.

Windows GPU-enabled GUI and CLI executables export the driver-recognized
`NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance` hints. Linux's
desktop entry sets `PrefersNonDefaultGPU=true`. These are preferences, not a
device-selection guarantee; user/OS/driver policy may override them. Change
Windows per-application Graphics preferences and restart when a laptop selects
the wrong device. On Linux, a desktop's discrete-GPU launch option or the
driver's PRIME settings can select a different device. For Mesa use, for example,
`DRI_PRIME=1 pvt-render --renderer-info`; NVIDIA GLX offload commonly uses
`__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia pvt-render --renderer-info`.
Always inspect the returned renderer to verify selection.

References: [NVIDIA Optimus policy](https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf),
[AMD switchable graphics](https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/),
[desktop GPU preference](https://specifications.freedesktop.org/desktop-entry/latest/recognized-keys.html),
[Mesa selection](https://docs.mesa3d.org/envvars.html),
[NVIDIA PRIME offload](https://download.nvidia.com/XFree86/Linux-x86_64/570.86.16/README/primerenderoffload.html).

### CUDA, HIP, and modern CPU features

NVIDIA CUDA and AMD ROCm/HIP are real options for future compute backends. CUDA
requires a supported NVIDIA GPU/toolchain. AMD HIP support varies by GPU and OS;
its Windows support is not a substitute for the full range of x64 and ARM64
targets PVT ships. A backend would need explicit ownership, residency, error,
memory, cancellation, parity, and packaging contracts, plus physical-device
measurements. This change adds no CUDA or HIP kernels and makes no claim of
CUDA/HIP acceleration. First reduce repeated shading and host/device transfers
in the existing backend; a new API alone does not make those costs disappear.

Official references: [CUDA on Windows](https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/index.html),
[CUDA on Linux](https://docs.nvidia.com/cuda/cuda-installation-guide-linux/index.html),
[HIP](https://rocm.docs.amd.com/projects/HIP/en/latest/what_is_hip.html),
[AMD Windows support matrix](https://rocm.docs.amd.com/projects/install-on-windows/en/develop/reference/system-requirements.html).
Consult current vendor matrices before selecting hardware.

CPU work stays bounded by workers and memory. Exact lookup-based display
conversion removes per-channel transcendental work on both x64 and ARM64;
release builds retain the compiler's target-baseline vectorization without
global fast-math, AVX-only, or `-march=native` requirements. Additional SIMD
dispatch must prove a workload benefit and preserve image semantics. Tensor,
ray-tracing, and low-precision hardware have no automatic benefit for the
current float32 procedural pipeline. No claim is made that every CPU/GPU
instruction set is used.

### Validation with only a Mac Studio available

Use the Mac Studio for native Metal and a separate Metal-disabled OpenGL test
build. The five-platform GitHub Actions matrix covers compilation and package
tests; Linux Mesa under Xvfb also executes portable shader parity. CI now sets
`PVT_REQUIRE_OPENGL=1` so a missing Linux context fails instead of skipping the
shader tests. Mesa software execution verifies semantics, not vendor performance.

GitHub retired GPU machine types in Codespaces in August 2025. GitHub Actions
does offer NVIDIA Tesla T4 larger runners on Ubuntu and Windows, but larger
runners require a Team/Enterprise Cloud organization and are billed separately,
including for public repositories. The published GPU runner table does not list
AMD GPUs. The authenticated hosted-runner and machine-size API requests for the user's
`GNA-SERVICES-INC` organization returned HTTP 404: "GitHub hosted runners are not
supported for this organization"; its plan reports Free. No paid runner or
billing change was provisioned. The PVT repository is personally owned by
`gnaservicesinc`, whose student Pro discount applies to the supplied standard
Actions billing examples, not proof of larger-runner eligibility.

An NVIDIA runner could supply additional evidence after checking that its
driver exposes a working OpenGL graphics context; having CUDA access alone does
not establish that. AMD driver qualification remains pending access to AMD
hardware or an appropriate external runner. Capture `--renderer-info`, run the
parity suite, and compare `pvt_opengl_surface_backend_tests --benchmark` on the
same machine and driver before making performance claims.

Sources: [Codespaces GPU retirement](https://github.blog/changelog/2025-08-01-upcoming-deprecation-of-gpu-machine-type-in-codespaces/),
[GitHub GPU runner specifications](https://docs.github.com/en/actions/reference/runners/larger-runners),
[eligibility and billing](https://docs.github.com/en/actions/concepts/runners/larger-runners).

PVT 5.0.0 begins the OpenGL renderer for Linux and Windows using the context,
surface, and function-loading APIs already supplied by Qt. PVT 8.0.3 adds the
first generated-source pass for ordinary Continuous hue layers. Keep Metal on
macOS. These shipped stages preserve the public CPU/CPU+GPU/GPU policy names.

GLFW is not required for this editor path. It remains available for evaluation
only if a later standalone `pvt-live` presentation executable needs a minimal
full-screen window. The shared renderer must not depend on its window host.

## Portable GPU path

Use Qt's public OpenGL integration in the existing GUI and
`QOffscreenSurface` for non-windowed rendering.

1. **Implemented in 5.0.0:** keep `cpu`, `cpu+gpu`, and `gpu` as
   user-visible policies and report Metal/OpenGL runtime capabilities rather
   than using platform-name checks.
2. Extract the current Metal kernel inputs into backend-neutral packed
   parameters and explicit passes. The CPU renderer remains the reference.
3. **Surface and first source stages implemented:** OpenGL float
   texture/framebuffer passes cover ordinary Continuous hue generated layers,
   analytic closed Cylinder/Sphere/Cube/flat Plane mapping, and displaced Plane
   mesh rasterization on Windows/Linux. Other generated modes, starting images,
   palettes, effects, quantization, Custom OBJ, and final layer compositing
   remain ordered reference stages inside the portable path. CPU + GPU
   accelerates eligible stages; GPU admits every valid layer and completes it
   through OpenGL instead of rejecting projects without a particular surface
   or generated-source variant.
4. Reuse the current bounded admission, cancellation, straight-alpha, linear
   light, half-open loop, and ordered-compositing contracts. Do not read a GPU
   result into the CPU between every effect.
5. **CI stage implemented:** run CPU/OpenGL image and alpha parity for the three
   portable curved mappings with Mesa under Xvfb, plus flat Plane parity and
   native compilation/package smoke on Windows. Cancellation expansion and real
   Intel, AMD, and NVIDIA driver
   qualification remain required before calling the complete portable backend
   production-ready.
6. Keep Metal as the preferred macOS backend. Apple's deprecated OpenGL stack
   is not a reason to replace the already-tested Metal implementation.

Official context reference: <https://doc.qt.io/qt-6/qopenglcontext.html>.

Qt's QRhi could eventually provide Metal, Direct3D, Vulkan, and OpenGL behind a
single layer, but Qt documents that family with a more limited compatibility
guarantee than its normal public APIs. PVT should not make a release-critical
backend depend on private or compatibility-limited Qt interfaces without first
isolating that dependency and proving the maintenance tradeoff.

## Native movie export

Movie export should extend the existing bounded `video_export` interface rather
than route frames through shell command strings.

### Windows

Prefer Windows Media Foundation and its Sink Writer API for the default native
backend. It is part of Windows, avoids downloading or redistributing FFmpeg,
and permits runtime discovery of installed encoders. The implementation must
probe actual codec/hardware availability, keep audio timestamps synchronized,
write to a sibling temporary, support cancellation, and atomically install the
completed file. Alpha-capable output must be advertised only when an available
codec and container really preserve alpha.

An advanced user-selected FFmpeg executable can remain a later fallback. PVT
must never download or redistribute it, must invoke it directly with an argv
array rather than through a shell, and must display the discovered binary and
codec capabilities before export.

### Linux

Use optional GStreamer development packages and the GStreamer API. Feed ordered
video/audio samples through `appsrc`, discover encoders and muxers at runtime,
and explain missing plugins rather than assuming a distribution ships a
particular codec. Packages must not silently acquire patent-encumbered plugin
sets. Preserve the same cancellation, temporary-output, collision, alpha, and
audio-clock contracts as macOS.

PNG and EXR image sequences remain the lossless portable fallback on every
platform throughout this work.

## Delivery order

1. ~~OpenGL proof for analytic one-layer surface mapping.~~ Shipped in 5.0.0.
2. Linux and Windows installed-package physical hardware probes and diagnostics.
3. Complete remaining source/effect coverage and GPU-resident compositing.
4. Windows Media Foundation movie export.
5. Linux GStreamer movie export.
6. Real-GPU and real-codec validation, followed by enabling `cpu+gpu` as a safe
   default only on the platforms and drivers that pass.
