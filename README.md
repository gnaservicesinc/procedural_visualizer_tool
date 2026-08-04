# Seamless Ripple PNG Renderer

Build and open the interactive settings menu:

```sh
make
make run
```

Press Enter at the main menu to render with the displayed settings. Every prompt has a default; pressing Enter keeps the current value.

The default render is:

- 1920 x 1080
- 16-pixel blocks
- 480 frames (8 seconds at 60 fps)
- three independently configurable wave sources
- `frame_0000.png` through `frame_0479.png` in the current directory
- protection against replacing existing frames

The menu controls canvas size, block size, frame rate, loop length, wave placement, amplitude, spatial frequency, motion cycles, phase, 3D appearance, rhythm, color, output directory, filename prefix, numbering, and overwrite behavior. Output directories are created when needed.

## Smooth-loop guarantee

The renderer samples frames `0` through `N - 1` around one periodic phase. Animated wave speeds, swing pulses, and hue rotations are entered as whole cycles per loop, so every animated term reaches its starting state at the omitted endpoint `N`. Frame `N - 1` therefore transitions to frame `0` by one normal frame step. Negative wave cycles reverse direction; zero makes that wave stationary. The duplicated endpoint frame `N` is intentionally not written, which avoids a pause at the seam.

Spatial frequency is shown as rings across the shorter canvas edge. Wave locations are percentages of the canvas, so changing resolution keeps the same composition. Locations below 0% or above 100% place a source outside the frame.

## Other commands

Render defaults without opening the menu:

```sh
make render
# Equivalent direct command:
./render9 --defaults
```

Use command-line overrides for scripts or small previews:

```sh
./render9 --render \
  --width 640 --height 360 --block-size 8 \
  --frames 120 --fps 30 \
  --output-dir preview --prefix ripple_
```

Add `--overwrite` if matching files may be replaced. Existing extra frames are never deleted, so use a fresh directory or prefix when shortening a sequence.

Run the built-in validation and periodicity check:

```sh
make check
```

`make clean` removes only the compiled executable, never PNG frames.
