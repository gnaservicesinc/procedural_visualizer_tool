# Cross-platform GPU and video roadmap

This document turns the Linux and Windows parity goal into implementation
boundaries. It deliberately does not claim a backend is complete merely because
it compiles in a software-rendered virtual machine.

## Selected direction

PVT 5.0.0 begins the OpenGL renderer for Linux and Windows using the context,
surface, and function-loading APIs already supplied by Qt. Keep Metal on macOS.
The shipped first stage accelerates analytic 3D surface mapping without
replacing the UI or changing the public CPU/CPU+GPU/GPU policy names.

GLFW is not required for this editor path. It remains available for evaluation
only if a later standalone `pvt-live` presentation executable needs a minimal
full-screen window. The shared renderer must not depend on its window host.

## Portable GPU path

Use Qt's public OpenGL integration in the existing GUI and
`QOffscreenSurface` for non-windowed rendering.

1. **Implemented in 5.0.0:** keep `cpu`, `cpu+gpu`, and strict `gpu` as
   user-visible policies and report Metal/OpenGL runtime capabilities rather
   than using platform-name checks.
2. Extract the current Metal kernel inputs into backend-neutral packed
   parameters and explicit passes. The CPU renderer remains the reference.
3. **First stage implemented:** an OpenGL float-texture/framebuffer pass covers
   analytic closed Cylinder, Sphere, and Cube mapping. Flat/displacement Plane,
   procedural sources,
   effects, quantization, mesh surfaces, and final layer compositing remain on
   CPU for the portable path. CPU + GPU accelerates an eligible stage; strict
   GPU gives an actionable error when no supported analytic stage exists.
4. Reuse the current bounded admission, cancellation, straight-alpha, linear
   light, half-open loop, and ordered-compositing contracts. Do not read a GPU
   result into the CPU between every effect.
5. **CI stage implemented:** run CPU/OpenGL image and alpha parity for all three
   analytic mappings with Mesa under Xvfb, plus native compilation/package smoke
   on Windows. Cancellation expansion and real Intel, AMD, and NVIDIA driver
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
3. Complete supported source/effect coverage and GPU-resident compositing.
4. Windows Media Foundation movie export.
5. Linux GStreamer movie export.
6. Real-GPU and real-codec validation, followed by enabling `cpu+gpu` as a safe
   default only on the platforms and drivers that pass.
