#include <metal_stdlib>

using namespace metal;

constant float kPi = 3.14159265358979323846f;
constant float kTau = 6.28318530717958647692f;
constant float kBlurWeights[9] = {
    0.02763055f, 0.06628225f, 0.12383154f, 0.18017382f,
    0.20416369f, 0.18017382f, 0.12383154f, 0.06628225f,
    0.02763055f};

struct FrameConstants {
    uint4 dimensions_counts; // width, height, block size, wave count
    uint4 counts_flags;      // swing count, palette count, alpha, displacement
    uint4 base_flags;        // lighting, spiral, wall, reserved
    int4 signed_values;      // alpha cycles, spiral arms, hue cycles, mirror
    uint4 transform_quant;   // flip H, flip V, quant enabled, quant mode
    uint4 quant_values;      // quant levels, generated bits/count low/count high
    float4 phases;           // loop, global motion, breath, short side
    float4 timelines;        // independent loop, reserved...
    float4 center_ghost;     // center x/y, ghost lag radians, ghost mix
    float4 pattern0;         // displacement, wave depth, spiral freq, wall freq
    float4 pattern1;         // wall mix, saturation, audio hue, alpha spatial freq
    float4 alpha_quant;      // alpha min/max, alpha phase radians, quant mix
    uint4 starting_flags;    // mode, include alpha, effective source alpha, dither+1
    uint4 starting_reference; // full width/height/block size, auto levels
    float4 starting_minimum; // RGBA range minima
    float4 starting_maximum; // RGBA range maxima
    uint4 shaping_uint; // segments, octaves, signed cycles, seed low 32 bits
    float4 shaping_values; // kaleido rotation/mix, warp strength/scale
    uint4 post_flags; // invert RGB, invert alpha, antialias, passes
    float4 post_values; // invert mixes, antialias strength, threshold
};

struct GpuWave {
    float4 geometry; // source x/y, effective amplitude, spatial frequency
    float4 phase;    // phase radians, direction, resolved tangent, unused
    int4 behavior;  // cycles, synchronized, follow tangent, unused
};

struct GpuSwing {
    float4 value; // center x/y, radius, contribution
};

struct GpuEffect {
    uint4 kind; // type, space, edge mode, unused
    float4 primary; // phase, intensity, magnitude, frequency
    float4 placement; // secondary, center x/y, angle radians
    float4 glow_area; // radius, threshold, soft knee, area radius
    uint4 blur; // BlurType, samples, passes, horizontal-pass flag
};

struct GpuSurface {
    uint4 kind; // mapping, unused...
    float4 values; // phase, curvature, lighting, unused
};

struct GpuSourceImage {
    uint4 source; // width, height, StartingImageFit, unused
    uint4 destination; // width, height, unused...
};

struct GpuMotion {
    float4 source_target; // source center x/y, destination center x/y
    float4 rotation_scale; // cosine, sine, scale, unused
};

struct GpuParticlePoint {
    float4 geometry; // center x/y, local radius, trail gain
};

struct GpuParticleGrid {
    uint4 layout; // tiles across/down, tile size, point count
};

float clamp_unit(float value) {
    return clamp(value, 0.0f, 1.0f);
}

float smooth_unit(float value) {
    value = clamp_unit(value);
    return value * value * (3.0f - 2.0f * value);
}

float wrap_unit(float value) {
    value = fmod(value, 1.0f);
    return value < 0.0f ? value + 1.0f : value;
}

ulong generated_shape_hash(ulong value) {
    value += 0x9e3779b97f4a7c15ul;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ul;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebul;
    return value ^ (value >> 31u);
}

float generated_shape_unit(ulong value) {
    return float(generated_shape_hash(value) >> 40u)
           * (1.0f / 16777216.0f);
}

float2 shape_generated_coordinate(constant FrameConstants& frame,
                                  float2 coordinate) {
    const float center_x = 0.5f * float(frame.dimensions_counts.x - 1u);
    const float center_y = 0.5f * float(frame.dimensions_counts.y - 1u);
    const float short_side = frame.phases.w;
    const float warp_strength = frame.shaping_values.z;
    if (warp_strength > 1.0e-7f) {
        float2 normalized = (coordinate - float2(center_x, center_y))
                            / short_side;
        float2 offset = float2(0.0f);
        float amplitude = warp_strength;
        float normalization = 0.0f;
        float frequency = frame.shaping_values.w;
        const float temporal = float(int(frame.shaping_uint.z))
                               * frame.phases.x;
        const ulong seed = (ulong(frame.base_flags.w) << 32u)
                           | ulong(frame.shaping_uint.w);
        for (uint octave = 0u; octave < frame.shaping_uint.y; ++octave) {
            const ulong octave_seed = generated_shape_hash(
                seed ^ (ulong(octave) + 1ul) * 0xd1b54a32d192ed03ul);
            const float phase_x = kTau * generated_shape_unit(octave_seed);
            const float phase_y = kTau * generated_shape_unit(
                octave_seed ^ 0x94d049bb133111ebul);
            offset.x += amplitude * sin(
                kTau * frequency * normalized.y + temporal + phase_x);
            offset.y += amplitude * cos(
                kTau * frequency * normalized.x - temporal + phase_y);
            normalization += amplitude;
            normalized += 0.35f * offset;
            amplitude *= 0.5f;
            frequency *= 2.0f;
        }
        if (normalization > 1.0e-7f) {
            coordinate += short_side * offset / normalization
                          * warp_strength;
        }
    }

    const float kaleidoscope_mix = frame.shaping_values.y;
    if (kaleidoscope_mix > 1.0e-7f) {
        const float2 delta = coordinate - float2(center_x, center_y);
        const float radius = length(delta);
        const float rotation = frame.shaping_values.x;
        const float period = kTau / float(frame.shaping_uint.x);
        float local = fmod(atan2(delta.y, delta.x) - rotation, period);
        if (local < 0.0f) local += period;
        const float folded = fabs(local - 0.5f * period);
        const float target_angle = rotation + folded;
        const float2 target = float2(center_x, center_y)
                              + radius * float2(cos(target_angle),
                                                sin(target_angle));
        coordinate = mix(coordinate, target, kaleidoscope_mix);
    }
    return coordinate;
}

float srgb_to_linear(float value) {
    value = clamp_unit(value);
    return value <= 0.04045f
               ? value / 12.92f
               : pow((value + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float value) {
    value = clamp_unit(value);
    return value <= 0.0031308f
               ? 12.92f * value
               : 1.055f * pow(value, 1.0f / 2.4f) - 0.055f;
}

float4 apply_generated_rgb_range(constant FrameConstants& frame,
                                 float4 color) {
    const float4 unit = float4(
        linear_to_srgb(color.r), linear_to_srgb(color.g),
        linear_to_srgb(color.b), color.a);
    const float4 ranged = mix(frame.starting_minimum,
                              frame.starting_maximum, unit);
    return float4(srgb_to_linear(ranged.r), srgb_to_linear(ranged.g),
                  srgb_to_linear(ranged.b), color.a);
}

float4 hsl_to_linear(float hue_degrees, float saturation, float lightness) {
    float hue = fmod(hue_degrees, 360.0f);
    if (hue < 0.0f) hue += 360.0f;
    saturation = clamp_unit(saturation);
    lightness = clamp_unit(lightness);
    const float chroma = (1.0f - fabs(2.0f * lightness - 1.0f)) * saturation;
    const float sector = hue / 60.0f;
    const float intermediate =
        chroma * (1.0f - fabs(fmod(sector, 2.0f) - 1.0f));
    const float match = lightness - 0.5f * chroma;
    float3 rgb = float3(0.0f);
    if (sector < 1.0f) rgb = float3(chroma, intermediate, 0.0f);
    else if (sector < 2.0f) rgb = float3(intermediate, chroma, 0.0f);
    else if (sector < 3.0f) rgb = float3(0.0f, chroma, intermediate);
    else if (sector < 4.0f) rgb = float3(0.0f, intermediate, chroma);
    else if (sector < 5.0f) rgb = float3(intermediate, 0.0f, chroma);
    else rgb = float3(chroma, 0.0f, intermediate);
    rgb += match;
    return float4(srgb_to_linear(rgb.r), srgb_to_linear(rgb.g),
                  srgb_to_linear(rgb.b), 1.0f);
}

float linear_hue_degrees(float4 color) {
    const float3 rgb = float3(linear_to_srgb(color.r),
                              linear_to_srgb(color.g),
                              linear_to_srgb(color.b));
    const float maximum = max(rgb.r, max(rgb.g, rgb.b));
    const float minimum = min(rgb.r, min(rgb.g, rgb.b));
    const float delta = maximum - minimum;
    if (delta <= 1.0e-7f) return 0.0f;
    if (maximum == rgb.r) {
        return 60.0f * fmod((rgb.g - rgb.b) / delta + 6.0f, 6.0f);
    }
    if (maximum == rgb.g) {
        return 60.0f * ((rgb.b - rgb.r) / delta + 2.0f);
    }
    return 60.0f * ((rgb.r - rgb.g) / delta + 4.0f);
}

float4 rotate_linear_hue(float4 color, float degrees) {
    if (fabs(degrees) <= 1.0e-7f) return color;
    float3 rgb = float3(linear_to_srgb(color.r), linear_to_srgb(color.g),
                        linear_to_srgb(color.b));
    const float maximum = max(rgb.r, max(rgb.g, rgb.b));
    const float minimum = min(rgb.r, min(rgb.g, rgb.b));
    const float delta = maximum - minimum;
    if (delta <= 1.0e-7f) return color;
    float hue = 0.0f;
    if (maximum == rgb.r) hue = fmod((rgb.g - rgb.b) / delta, 6.0f);
    else if (maximum == rgb.g) hue = (rgb.b - rgb.r) / delta + 2.0f;
    else hue = (rgb.r - rgb.g) / delta + 4.0f;
    hue = wrap_unit(hue / 6.0f + degrees / 360.0f);
    const float saturation = maximum > 1.0e-7f ? delta / maximum : 0.0f;
    const float chroma = maximum * saturation;
    const float sector = hue * 6.0f;
    const float intermediate =
        chroma * (1.0f - fabs(fmod(sector, 2.0f) - 1.0f));
    const float match = maximum - chroma;
    float3 rotated = float3(0.0f);
    if (sector < 1.0f) rotated = float3(chroma, intermediate, 0.0f);
    else if (sector < 2.0f) rotated = float3(intermediate, chroma, 0.0f);
    else if (sector < 3.0f) rotated = float3(0.0f, chroma, intermediate);
    else if (sector < 4.0f) rotated = float3(0.0f, intermediate, chroma);
    else if (sector < 5.0f) rotated = float3(intermediate, 0.0f, chroma);
    else rotated = float3(chroma, 0.0f, intermediate);
    rotated += match;
    color.rgb = float3(srgb_to_linear(rotated.r),
                       srgb_to_linear(rotated.g),
                       srgb_to_linear(rotated.b));
    return color;
}

float circular_influence(float center_x, float center_y, float radius,
                         float x, float y, uint width, uint height) {
    if (radius <= 1.0e-7f) return 1.0f;
    const float short_side = float(min(width, height));
    const float dx = x - center_x * float(width - 1u);
    const float dy = y - center_y * float(height - 1u);
    const float distance = length(float2(dx, dy)) / short_side;
    const float feather_start = radius * 0.8f;
    if (distance <= feather_start) return 1.0f;
    if (distance >= radius) return 0.0f;
    return 1.0f - smooth_unit(
        (distance - feather_start) / max(1.0e-7f, radius - feather_start));
}

float motion_phase_at(constant FrameConstants& frame,
                      const device GpuSwing* swings, float x, float y) {
    float result = frame.phases.y;
    for (uint index = 0u; index < frame.counts_flags.x; ++index) {
        const float4 swing = swings[index].value;
        result += swing.w * circular_influence(
            swing.x, swing.y, swing.z, x, y,
            frame.dimensions_counts.x, frame.dimensions_counts.y);
    }
    return result;
}

float wave_height(constant FrameConstants& frame,
                  const device GpuWave* waves, float x, float y,
                  float motion_phase) {
    float result = 0.0f;
    const float short_side = frame.phases.w;
    for (uint index = 0u; index < frame.dimensions_counts.w; ++index) {
        const device GpuWave& wave = waves[index];
        const float dx = (x - wave.geometry.x) / short_side;
        const float dy = (y - wave.geometry.y) / short_side;
        const float radial = length(float2(dx, dy));
        const float direction = wave.phase.y;
        const float coordinate = wave.behavior.z != 0
            ? cos(wave.phase.z) * dx + sin(wave.phase.z) * dy
            : (direction < 0.5f
                   ? mix(radial, dx, 1.0f - 2.0f * direction)
                   : mix(radial, dy, 2.0f * direction - 1.0f));
        const float clock = wave.behavior.y != 0
                                ? motion_phase
                                : frame.timelines.x;
        const float phase = float(wave.behavior.x) * clock;
        result += wave.geometry.z
                  * sin(kTau * wave.geometry.w * coordinate
                        - phase + wave.phase.x);
    }
    return result;
}

float4 nearest_palette(float4 input, const device float4* palette,
                       uint count, bool compare_alpha) {
    if (count == 0u) return input;
    if (compare_alpha && input.a <= 1.0e-12f) return float4(0.0f);
    float4 closest = palette[0];
    float closest_distance = INFINITY;
    for (uint index = 0u; index < count; ++index) {
        const float3 delta = input.rgb - palette[index].rgb;
        const float alpha_delta = input.a - palette[index].a;
        const float distance = dot(delta * delta,
                                   float3(0.2126f, 0.7152f, 0.0722f))
                               + (compare_alpha
                                      ? alpha_delta * alpha_delta : 0.0f);
        if (distance < closest_distance) {
            closest = palette[index];
            closest_distance = distance;
        }
    }
    return closest;
}

ulong diagonal_traversal_index(ulong x, ulong y, ulong width, ulong height) {
    const ulong diagonal = x + y;
    const ulong short_side = min(width, height);
    const ulong long_side = max(width, height);
    ulong prefix = 0ul;
    if (diagonal < short_side) {
        prefix = diagonal * (diagonal + 1ul) / 2ul;
    } else if (diagonal < long_side) {
        prefix = short_side * (short_side + 1ul) / 2ul
                 + (diagonal - short_side) * short_side;
    } else {
        const ulong remaining = width + height - 1ul - diagonal;
        prefix = width * height - remaining * (remaining + 1ul) / 2ul;
    }
    const ulong minimum_x = diagonal >= height
        ? diagonal - (height - 1ul) : 0ul;
    const ulong maximum_x = min(width - 1ul, diagonal);
    const ulong offset = (diagonal & 1ul) == 0ul
        ? x - minimum_x : maximum_x - x;
    return prefix + offset;
}

ulong square_spiral_traversal_index(ulong x, ulong y,
                                    ulong width, ulong height) {
    const ulong layer = min(
        min(x, y), min(width - 1ul - x, height - 1ul - y));
    const ulong ring_width = width - 2ul * layer;
    const ulong ring_height = height - 2ul * layer;
    const ulong prefix = width * height - ring_width * ring_height;
    const ulong local_x = x - layer;
    const ulong local_y = y - layer;
    ulong offset = 0ul;
    if (ring_height == 1ul) {
        offset = local_x;
    } else if (ring_width == 1ul) {
        offset = local_y;
    } else if (local_y == 0ul) {
        offset = local_x;
    } else if (local_x == ring_width - 1ul) {
        offset = ring_width - 1ul + local_y;
    } else if (local_y == ring_height - 1ul) {
        offset = ring_width - 1ul + ring_height - 1ul
                 + ring_width - 1ul - local_x;
    } else {
        offset = 2ul * (ring_width - 1ul) + ring_height - 1ul
                 + ring_height - 1ul - local_y;
    }
    return prefix + offset;
}

ulong generated_starting_index(constant FrameConstants& frame,
                               uint block_x, uint block_y) {
    const ulong reference_width = ulong(frame.starting_reference.x);
    const ulong reference_height = ulong(frame.starting_reference.y);
    const ulong reference_block = ulong(frame.starting_reference.z);
    const ulong reference_x = min(
        reference_width - 1ul,
        ulong(block_x) * reference_width
            / ulong(frame.dimensions_counts.x));
    const ulong reference_y = min(
        reference_height - 1ul,
        ulong(block_y) * reference_height
            / ulong(frame.dimensions_counts.y));
    const ulong blocks_across =
        (reference_width + reference_block - 1ul) / reference_block;
    const ulong blocks_down =
        (reference_height + reference_block - 1ul) / reference_block;
    const ulong x = reference_x / reference_block;
    const ulong y = reference_y / reference_block;
    const uint mode = frame.starting_flags.x;
    if (mode == 1u) return x * blocks_down + y;
    if (mode == 3u || mode == 6u) {
        return diagonal_traversal_index(x, y, blocks_across, blocks_down);
    }
    if (mode == 4u) {
        return square_spiral_traversal_index(
            x, y, blocks_across, blocks_down);
    }
    return y * blocks_across + x;
}

ulong hue_sector_prefix(ulong maximum) {
    return (maximum - 1ul) * maximum * (maximum + 1ul) / 6ul;
}

ulong hue_minimum_prefix(ulong maximum, ulong minimum) {
    return minimum * (2ul * maximum - minimum + 1ul) / 2ul;
}

ulong3 hue_ordered_rgb_indices(ulong index, ulong levels) {
    if (levels <= 1ul) return ulong3(0ul);

    const ulong rgb_capacity = levels * levels * levels;
    index %= rgb_capacity;
    const ulong non_gray_count = rgb_capacity - levels;
    if (index >= non_gray_count) {
        const ulong gray = index - non_gray_count;
        return ulong3(gray);
    }

    const ulong sector_size = non_gray_count / 6ul;
    const ulong sector = index / sector_size;
    ulong local = index % sector_size;
    if ((sector & 1ul) != 0ul) local = sector_size - 1ul - local;
    ulong maximum_low = 1ul;
    ulong maximum_high = levels;
    while (maximum_low + 1ul < maximum_high) {
        const ulong middle =
            maximum_low + (maximum_high - maximum_low) / 2ul;
        if (hue_sector_prefix(middle) <= local) maximum_low = middle;
        else maximum_high = middle;
    }
    const ulong maximum = maximum_low;
    const ulong within_maximum = local - hue_sector_prefix(maximum);
    ulong minimum_low = 0ul;
    ulong minimum_high = maximum;
    while (minimum_low + 1ul < minimum_high) {
        const ulong middle =
            minimum_low + (minimum_high - minimum_low) / 2ul;
        if (hue_minimum_prefix(maximum, middle) <= within_maximum) {
            minimum_low = middle;
        } else {
            minimum_high = middle;
        }
    }
    const ulong minimum = minimum_low;
    const ulong offset = within_maximum
                         - hue_minimum_prefix(maximum, minimum);
    if (sector == 0ul) return ulong3(maximum, minimum + offset, minimum);
    if (sector == 1ul) return ulong3(maximum - offset, maximum, minimum);
    if (sector == 2ul) return ulong3(minimum, maximum, minimum + offset);
    if (sector == 3ul) return ulong3(minimum, maximum - offset, maximum);
    if (sector == 4ul) return ulong3(minimum + offset, minimum, maximum);
    return ulong3(maximum, minimum, maximum - offset);
}

float4 generated_starting_color(constant FrameConstants& frame,
                                ulong index) {
    const ulong color_count = (ulong(frame.quant_values.w) << 32u)
                              | ulong(frame.quant_values.z);
    const uint mode = frame.starting_flags.x;
    // Ordered modes retain their coherent whole-render walks. Mode 5 is the
    // explicit repeatable Random traversal and alone applies the bijection.
    if (mode == 5u) {
        if (color_count > 1ul) {
            const uint bits = frame.quant_values.y;
            const ulong mask = (1ul << bits) - 1ul;
            const uint shift1 = max(1u, bits / 2u);
            const uint shift2 = max(1u, bits / 3u);
            const uint shift3 = max(1u, (bits * 2u) / 3u);
            do {
                index = (index + 0x9e3779b97f4a7c15ul) & mask;
                index ^= index >> shift1;
                index = (index * 0xbf58476d1ce4e5b9ul) & mask;
                index ^= index >> shift2;
                index = (index * 0x94d049bb133111ebul) & mask;
                index ^= index >> shift3;
                index &= mask;
            } while (index >= color_count);
        } else {
            index = 0ul;
        }
    }
    const ulong levels = max(1ul, ulong(frame.starting_reference.w));
    const ulong alpha_levels = frame.starting_flags.y != 0u ? levels : 1ul;
    const ulong rgb_capacity = levels * levels * levels;
    const ulong3 rgb = hue_ordered_rgb_indices(
        index % rgb_capacity, levels);
    const ulong red_index = rgb.x;
    const ulong green_index = rgb.y;
    const ulong blue_index = rgb.z;
    const ulong alpha_index = frame.starting_flags.y != 0u
        ? (index / rgb_capacity) % alpha_levels : 0ul;
    const float denominator = levels > 1ul ? float(levels - 1ul) : 1.0f;
    const float alpha_denominator = alpha_levels > 1ul
        ? float(alpha_levels - 1ul) : 1.0f;
    const float4 unit = float4(
        float(red_index) / denominator,
        float(green_index) / denominator,
        float(blue_index) / denominator,
        frame.starting_flags.y != 0u
            ? float(alpha_index) / alpha_denominator : 1.0f);
    const float4 ranged = mix(frame.starting_minimum,
                              frame.starting_maximum, unit);
    const float3 source_rgb = mode == 6u ? unit.rgb : ranged.rgb;
    return float4(srgb_to_linear(source_rgb.r), srgb_to_linear(source_rgb.g),
                  srgb_to_linear(source_rgb.b),
                  frame.starting_flags.y != 0u ? ranged.a : 1.0f);
}

float procedural_alpha(constant FrameConstants& frame,
                       uint x, uint y, uint width, uint height) {
    if (frame.counts_flags.z == 0u) return 1.0f;
    const float width_scale = width > 1u
        ? float(x) / float(width - 1u) : 0.0f;
    const float height_scale = height > 1u
        ? float(y) / float(height - 1u) : 0.0f;
    const float spatial =
        (width_scale + height_scale) * 0.7071067811865476f;
    const float alpha_phase =
        kTau * frame.pattern1.w * spatial
        - float(frame.signed_values.x) * frame.phases.x
        + frame.alpha_quant.z;
    const float amount = 0.5f + 0.5f * sin(alpha_phase);
    return mix(frame.alpha_quant.x, frame.alpha_quant.y, amount);
}

kernel void base_render(constant FrameConstants& frame [[buffer(0)]],
                        const device GpuWave* waves [[buffer(1)]],
                        const device GpuSwing* swings [[buffer(2)]],
                        const device float4* palette [[buffer(3)]],
                        device float4* output [[buffer(4)]],
                        uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    const uint block_size = frame.dimensions_counts.z;
    const uint block_x = gid.x * block_size;
    const uint block_y = gid.y * block_size;
    if (block_x >= width || block_y >= height) return;

    const float x = float(block_x);
    const float y = float(block_y);
    const float motion = motion_phase_at(frame, swings, x, y);
    const float motion_right = frame.counts_flags.x == 0u
        ? motion
        : motion_phase_at(frame, swings, x + float(block_size), y);
    const float motion_down = frame.counts_flags.x == 0u
        ? motion
        : motion_phase_at(frame, swings, x, y + float(block_size));
    const float height_here = wave_height(frame, waves, x, y, motion);
    const float height_right = wave_height(
        frame, waves, x + float(block_size), y, motion_right);
    const float height_down = wave_height(
        frame, waves, x, y + float(block_size), motion_down);
    const float slope_x = height_right - height_here;
    const float slope_y = height_down - height_here;
    const float displacement = frame.counts_flags.w != 0u
                                   ? frame.pattern0.x * frame.phases.z
                                   : 0.0f;
    const float displaced_x = x + slope_x * displacement;
    const float displaced_y = y + slope_y * displacement;
    const bool shaped_source = frame.shaping_values.y > 1.0e-7f
                               || frame.shaping_values.z > 1.0e-7f;
    const float2 shaped = shape_generated_coordinate(
        frame, float2(displaced_x, displaced_y));
    const float pattern_x = shaped.x;
    const float pattern_y = shaped.y;
    const float dx = pattern_x - frame.center_ghost.x;
    const float dy = pattern_y - frame.center_ghost.y;
    const float normalized_distance = length(float2(dx, dy)) / frame.phases.w;
    const float angle = atan2(dy, dx);
    const ulong starting_reference_width =
        ulong(frame.starting_reference.x);
    const ulong starting_reference_height =
        ulong(frame.starting_reference.y);
    const ulong starting_reference_x = min(
        starting_reference_width - 1ul,
        ulong(block_x) * starting_reference_width / ulong(width));
    const ulong starting_reference_y = min(
        starting_reference_height - 1ul,
        ulong(block_y) * starting_reference_height / ulong(height));
    const float2 starting_delta = shaped_source
        ? float2(
            (pattern_x / max(1.0f, float(width - 1u)) - 0.5f)
                * float(starting_reference_width - 1ul),
            (pattern_y / max(1.0f, float(height - 1u)) - 0.5f)
                * float(starting_reference_height - 1ul))
        : float2(
            float(starting_reference_x)
                - 0.5f * float(starting_reference_width - 1ul),
            float(starting_reference_y)
                - 0.5f * float(starting_reference_height - 1ul));
    const float starting_short_side = float(min(
        starting_reference_width, starting_reference_height));
    const float starting_radius = length(starting_delta)
        / max(1.0f, 0.5f * starting_short_side);
    const float starting_angle = atan2(starting_delta.y, starting_delta.x);
    const float starting_spiral_hue = frame.starting_flags.x == 6u
        ? 360.0f * (3.0f * starting_radius + starting_angle / kTau)
        : 0.0f;
    const float wall_distance = shaped_source
        ? min(min(pattern_x, float(width - 1u) - pattern_x),
              min(pattern_y, float(height - 1u) - pattern_y))
        : min(min(x, float(width - 1u - block_x)),
              min(y, float(height - 1u - block_y)));
    const float normalized_wall_distance = wall_distance / frame.phases.w;
    const float ghost_phase = motion - frame.center_ghost.z;
    const float main_spiral = frame.base_flags.y != 0u
        ? sin(kTau * frame.pattern0.z * normalized_distance
              + angle * float(frame.signed_values.y) - motion)
        : 0.0f;
    const float ghost_spiral = frame.base_flags.y != 0u
        ? sin(kTau * frame.pattern0.z * normalized_distance
              + angle * float(frame.signed_values.y) - ghost_phase)
        : 0.0f;
    const float main_wall = frame.base_flags.z != 0u
        ? sin(kTau * frame.pattern0.w * normalized_wall_distance
              + 2.0f * motion)
        : 0.0f;
    const float ghost_wall = frame.base_flags.z != 0u
        ? sin(kTau * frame.pattern0.w * normalized_wall_distance
              + 2.0f * ghost_phase)
        : 0.0f;
    const float main_signal = main_spiral + frame.pattern1.x * main_wall;
    const float ghost_signal = ghost_spiral + frame.pattern1.x * ghost_wall;
    const float combined = mix(main_signal, ghost_signal, frame.center_ghost.w);
    const float hue = (combined + 1.45f) * 260.0f
                      + 360.0f * float(frame.signed_values.z)
                            * (frame.phases.x / kTau);
    float lightness = 0.40f;
    if (frame.base_flags.x != 0u) {
        const float reflection = (slope_x + slope_y) * -0.7071067811865476f;
        const float normalized_light =
            reflection * frame.pattern0.y * frame.phases.z;
        lightness += normalized_light < 0.0f
                         ? 0.36f * normalized_light
                         : 0.28f * normalized_light;
    }
    lightness = clamp(lightness, 0.04f, 0.68f);
    const ulong starting_index = generated_starting_index(
        frame, block_x, block_y);
    float4 base;
    if (frame.counts_flags.y == 0u && frame.starting_flags.x == 0u) {
        base = hsl_to_linear(hue, frame.pattern1.y, lightness);
        base = apply_generated_rgb_range(frame, base);
        base = rotate_linear_hue(base, frame.pattern1.z);
        if (frame.starting_flags.y != 0u) {
            const ulong color_count = (ulong(frame.quant_values.w) << 32u)
                                      | ulong(frame.quant_values.z);
            const float position = color_count > 1ul
                ? float(starting_index) / float(color_count - 1ul) : 0.0f;
            base.a = mix(frame.starting_minimum.a,
                         frame.starting_maximum.a, position);
        }
    } else if (frame.counts_flags.y == 0u) {
        base = generated_starting_color(frame, starting_index);
        const float starting_spiral_hue_shift = frame.starting_flags.x == 6u
            ? starting_spiral_hue - linear_hue_degrees(base) : 0.0f;
        if (frame.starting_flags.x == 6u) {
            base = rotate_linear_hue(base, starting_spiral_hue_shift);
            base = apply_generated_rgb_range(frame, base);
        }
        base = rotate_linear_hue(
            base, combined * 260.0f + frame.pattern1.z);
        if (frame.base_flags.x != 0u) {
            base.rgb *= lightness / 0.40f;
        }
    } else {
        base = nearest_palette(
            hsl_to_linear(hue, frame.pattern1.y, 0.40f),
            palette, frame.counts_flags.y,
            frame.starting_flags.z != 0u);
        base = rotate_linear_hue(base, frame.pattern1.z);
        if (frame.base_flags.x != 0u) {
            base.rgb *= lightness / 0.40f;
        }
    }

    const uint end_x = min(block_x + block_size, width);
    const uint end_y = min(block_y + block_size, height);
    for (uint py = block_y; py < end_y; ++py) {
        for (uint px = block_x; px < end_x; ++px) {
            const float source_alpha = frame.starting_flags.z != 0u
                                           ? base.a : 1.0f;
            const float alpha = source_alpha
                                * procedural_alpha(frame, px, py,
                                                   width, height);
            output[py * width + px] = float4(base.rgb, clamp_unit(alpha));
        }
    }
}

int reflected_index(int index, int size) {
    if (size <= 1) return 0;
    const int period = 2 * (size - 1);
    index %= period;
    if (index < 0) index += period;
    if (index >= size) index = period - index;
    return index;
}

float4 edge_color(uint mode) {
    if (mode == 1u) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (mode == 2u) return float4(1.0f);
    return float4(0.0f);
}

float4 sample_texel(const device float4* image, int x, int y,
                    uint width, uint height, uint edge_mode) {
    if (x >= 0 && x < int(width) && y >= 0 && y < int(height)) {
        return image[uint(y) * width + uint(x)];
    }
    if (edge_mode != 3u) return edge_color(edge_mode);
    return image[uint(reflected_index(y, int(height))) * width
                 + uint(reflected_index(x, int(width)))];
}

float4 sample_bilinear(const device float4* image, float x, float y,
                       uint width, uint height, uint edge_mode) {
    if (!isfinite(x) || !isfinite(y)) return edge_color(edge_mode);
    const int x0 = int(floor(x));
    const int y0 = int(floor(y));
    const float tx = x - float(x0);
    const float ty = y - float(y0);
    float4 samples[4] = {
        sample_texel(image, x0, y0, width, height, edge_mode),
        sample_texel(image, x0 + 1, y0, width, height, edge_mode),
        sample_texel(image, x0, y0 + 1, width, height, edge_mode),
        sample_texel(image, x0 + 1, y0 + 1, width, height, edge_mode)};
    const float weights[4] = {
        (1.0f - tx) * (1.0f - ty), tx * (1.0f - ty),
        (1.0f - tx) * ty, tx * ty};
    float4 result = float4(0.0f);
    float rgb_weight = 0.0f;
    for (uint index = 0u; index < 4u; ++index) {
        const int sx = x0 + int(index & 1u);
        const int sy = y0 + int(index >> 1u);
        const bool outside = sx < 0 || sx >= int(width)
                             || sy < 0 || sy >= int(height);
        if (edge_mode != 0u || !outside) {
            result.rgb += samples[index].rgb * weights[index];
            rgb_weight += weights[index];
        }
        result.a += samples[index].a * weights[index];
    }
    if (edge_mode == 0u && rgb_weight > 1.0e-7f) {
        result.rgb /= rgb_weight;
    }
    result.a = clamp_unit(result.a);
    return result;
}

float4 sample_starting_image(const device float4* image, float x, float y,
                             uint width, uint height, bool tile,
                             bool transparent_outside) {
    if (!isfinite(x) || !isfinite(y) || width == 0u || height == 0u) {
        return float4(0.0f);
    }
    if (tile) {
        x = fmod(x, float(width));
        y = fmod(y, float(height));
        if (x < 0.0f) x += float(width);
        if (y < 0.0f) y += float(height);
    } else if (x < 0.0f || y < 0.0f || x > float(width - 1u)
               || y > float(height - 1u)) {
        if (transparent_outside) return float4(0.0f);
        x = clamp(x, 0.0f, float(width - 1u));
        y = clamp(y, 0.0f, float(height - 1u));
    }
    const uint x0 = uint(clamp(int(floor(x)), 0, int(width) - 1));
    const uint y0 = uint(clamp(int(floor(y)), 0, int(height) - 1));
    const uint x1 = tile ? (x0 + 1u) % width : min(x0 + 1u, width - 1u);
    const uint y1 = tile ? (y0 + 1u) % height : min(y0 + 1u, height - 1u);
    const float tx = x - floor(x);
    const float ty = y - floor(y);
    const float4 top = mix(image[y0 * width + x0],
                           image[y0 * width + x1], tx);
    const float4 bottom = mix(image[y1 * width + x0],
                              image[y1 * width + x1], tx);
    return mix(top, bottom, ty);
}

ulong source_dither_hash(ulong value) {
    value ^= value >> 30u;
    value *= 0xbf58476d1ce4e5b9ul;
    value ^= value >> 27u;
    value *= 0x94d049bb133111ebul;
    return value ^ (value >> 31u);
}

float source_dither_noise(uint x, uint y, uint method) {
    if (method == 2u) {
        const int bayer[16] = {
            0, 8, 2, 10, 12, 4, 14, 6,
            3, 11, 1, 9, 15, 7, 13, 5};
        const uint index = (y & 3u) * 4u + (x & 3u);
        return (float(bayer[index]) + 0.5f) / 16.0f - 0.5f;
    }
    const ulong coordinate = (ulong(x) << 32u) | ulong(y);
    const ulong bits = source_dither_hash(
        coordinate ^ 0xa0761d6478bd642ful);
    return float(bits >> 40u) * (1.0f / 16777216.0f) - 0.5f;
}

kernel void source_image_render(
    constant FrameConstants& frame [[buffer(0)]],
    constant GpuSourceImage& source [[buffer(1)]],
    const device float4* image [[buffer(2)]],
    const device float4* palette [[buffer(3)]],
    device float4* output [[buffer(4)]],
    uint2 gid [[thread_position_in_grid]]) {
    const uint source_width = source.source.x;
    const uint source_height = source.source.y;
    const uint fit = source.source.z;
    const uint destination_width = source.destination.x;
    const uint destination_height = source.destination.y;
    if (gid.x >= destination_width || gid.y >= destination_height) return;
    if (source.source.w != 0u) {
        output[gid.y * destination_width + gid.x] =
            image[gid.y * destination_width + gid.x];
        return;
    }

    const float sx = float(destination_width) / float(source_width);
    const float sy = float(destination_height) / float(source_height);
    float source_x = 0.0f;
    float source_y = 0.0f;
    const bool tile = fit == 3u;
    const bool transparent_outside = fit == 1u;
    if (fit == 0u) {
        source_x = (float(gid.x) + 0.5f) / sx - 0.5f;
        source_y = (float(gid.y) + 0.5f) / sy - 0.5f;
    } else if (tile) {
        source_x = float(gid.x);
        source_y = float(gid.y);
    } else {
        const float fit_scale = fit == 1u ? min(sx, sy) : max(sx, sy);
        source_x = (float(gid.x) - 0.5f * float(destination_width))
                       / fit_scale
                   + 0.5f * float(source_width);
        source_y = (float(gid.y) - 0.5f * float(destination_height))
                       / fit_scale
                   + 0.5f * float(source_height);
    }
    float4 color = sample_starting_image(
        image, source_x, source_y, source_width, source_height, tile,
        transparent_outside);
    const bool compare_alpha = frame.starting_flags.z != 0u;
    if (frame.counts_flags.y != 0u) {
        if (compare_alpha && color.a <= 1.0e-12f) {
            color = float4(0.0f);
            output[gid.y * destination_width + gid.x] = color;
            return;
        }
        if (frame.starting_flags.w != 0u) {
            const float amplitude = clamp(
                0.5f / pow(float(frame.counts_flags.y), 1.0f / 3.0f),
                0.015f, 0.18f);
            const float noise = source_dither_noise(
                gid.x, gid.y, frame.starting_flags.w) * amplitude;
            color.rgb = clamp(color.rgb + noise, 0.0f, 1.0f);
            if (compare_alpha) color.a = clamp_unit(color.a + noise);
        }
        color = nearest_palette(
            color, palette, frame.counts_flags.y, compare_alpha);
    }
    color.a = (compare_alpha ? color.a : 1.0f)
              * procedural_alpha(frame, gid.x, gid.y,
                                 destination_width, destination_height);
    output[gid.y * destination_width + gid.x] = color;
}

kernel void layer_motion(constant FrameConstants& frame [[buffer(0)]],
                         constant GpuMotion& motion [[buffer(1)]],
                         const device float4* input [[buffer(2)]],
                         device float4* output [[buffer(3)]],
                         uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    if (gid.x >= width || gid.y >= height) return;
    const float relative_x = float(gid.x) - motion.source_target.z;
    const float relative_y = float(gid.y) - motion.source_target.w;
    const float rotated_x = motion.rotation_scale.x * relative_x
                            - motion.rotation_scale.y * relative_y;
    const float rotated_y = motion.rotation_scale.y * relative_x
                            + motion.rotation_scale.x * relative_y;
    const float source_x = motion.source_target.x
                           + rotated_x / motion.rotation_scale.z;
    const float source_y = motion.source_target.y
                           + rotated_y / motion.rotation_scale.z;
    if (!isfinite(source_x) || !isfinite(source_y)
        || source_x <= -1.0f || source_y <= -1.0f
        || source_x >= float(width) || source_y >= float(height)) {
        output[gid.y * width + gid.x] = float4(0.0f);
        return;
    }
    output[gid.y * width + gid.x] = sample_bilinear(
        input, source_x, source_y, width, height, 0u);
}

float4 sample_bilinear_wrapped_x(const device float4* image, float x, float y,
                                 uint width, uint height) {
    if (!isfinite(x) || !isfinite(y) || width == 0u || height == 0u) {
        return float4(0.0f);
    }
    float wrapped_x = fmod(x, float(width));
    if (wrapped_x < 0.0f) wrapped_x += float(width);
    const int x0 = int(floor(wrapped_x));
    const int x1 = (x0 + 1) % int(width);
    const int y0 = int(floor(y));
    const float tx = wrapped_x - float(x0);
    const float ty = y - float(y0);
    const float4 samples[4] = {
        image[uint(reflected_index(y0, int(height))) * width + uint(x0)],
        image[uint(reflected_index(y0, int(height))) * width + uint(x1)],
        image[uint(reflected_index(y0 + 1, int(height))) * width + uint(x0)],
        image[uint(reflected_index(y0 + 1, int(height))) * width + uint(x1)]};
    return samples[0] * ((1.0f - tx) * (1.0f - ty))
           + samples[1] * (tx * (1.0f - ty))
           + samples[2] * ((1.0f - tx) * ty)
           + samples[3] * (tx * ty);
}

float3 rotate_x(float3 value, float angle) {
    const float cosine = cos(angle);
    const float sine = sin(angle);
    return float3(value.x, cosine * value.y - sine * value.z,
                  sine * value.y + cosine * value.z);
}

float3 rotate_y(float3 value, float angle) {
    const float cosine = cos(angle);
    const float sine = sin(angle);
    return float3(cosine * value.x + sine * value.z, value.y,
                  -sine * value.x + cosine * value.z);
}

float3 orient_normal(float3 normal, float3 ray_direction) {
    return dot(normal, ray_direction) > 0.0f ? -normal : normal;
}

float4 shade_surface(float4 color, float3 normal, float lighting) {
    const float3 light = normalize(float3(-0.45f, -0.55f, 0.75f));
    const float diffuse = max(0.0f, dot(normalize(normal), light));
    const float lit = 0.28f + 0.72f * diffuse;
    color.rgb *= max(0.0f, 1.0f + lighting * (lit - 1.0f));
    return color;
}

float4 composite_straight_over(float4 front, float4 back) {
    const float front_alpha = clamp_unit(front.a);
    const float back_weight = clamp_unit(back.a) * (1.0f - front_alpha);
    const float output_alpha = front_alpha + back_weight;
    if (output_alpha <= 1.0e-7f) {
        return float4(front.rgb, 0.0f);
    }
    return float4((front.rgb * front_alpha + back.rgb * back_weight)
                      / output_alpha,
                  output_alpha);
}

float4 sample_cylinder_side(const device float4* source,
                            uint width, uint height,
                            float longitude, float normal_x, float normal_z,
                            float surface_v, float phase, float lighting) {
    const float wrapped_u = wrap_unit(
        0.5f + longitude / kTau - phase / kTau);
    float4 sampled = sample_bilinear_wrapped_x(
        source, wrapped_u * float(width),
        surface_v * float(height - 1u), width, height);
    return shade_surface(
        sampled,
        orient_normal(float3(normal_x, 0.0f, normal_z),
                      float3(0.0f, 0.0f, -1.0f)),
        lighting);
}

float4 sample_sphere_side(const device float4* source,
                          uint width, uint height,
                          float normal_x, float normal_y, float normal_z,
                          float phase, float lighting) {
    const float3 normal = float3(normal_x, normal_y, normal_z);
    const float3 texture_normal = rotate_y(normal, -phase);
    const float longitude = atan2(texture_normal.x, texture_normal.z);
    const float latitude = asin(clamp(texture_normal.y, -1.0f, 1.0f));
    const float wrapped_u = wrap_unit(0.5f + longitude / kTau);
    const float sphere_v = 0.5f - latitude / kPi;
    float4 sampled = sample_bilinear_wrapped_x(
        source, wrapped_u * float(width),
        sphere_v * float(height - 1u), width, height);
    return shade_surface(
        sampled,
        orient_normal(normal, float3(0.0f, 0.0f, -1.0f)),
        lighting);
}

struct CubeHit {
    float distance;
    float3 point;
    float3 normal;
};

struct CubeIntersections {
    CubeHit front;
    CubeHit back;
    bool has_back;
};

float3 cube_normal(float3 point) {
    const float3 absolute = fabs(point);
    if (absolute.x >= absolute.y && absolute.x >= absolute.z) {
        return float3(point.x >= 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
    }
    if (absolute.y >= absolute.x && absolute.y >= absolute.z) {
        return float3(0.0f, point.y >= 0.0f ? 1.0f : -1.0f, 0.0f);
    }
    return float3(0.0f, 0.0f, point.z >= 0.0f ? 1.0f : -1.0f);
}

bool intersect_cube(float3 origin, float3 direction,
                    thread CubeIntersections& intersections) {
    float near_distance = -INFINITY;
    float far_distance = INFINITY;
    for (uint axis = 0u; axis < 3u; ++axis) {
        if (fabs(direction[axis]) < 1.0e-7f) {
            if (origin[axis] < -1.0f || origin[axis] > 1.0f) return false;
            continue;
        }
        float first = (-1.0f - origin[axis]) / direction[axis];
        float second = (1.0f - origin[axis]) / direction[axis];
        if (first > second) {
            const float temporary = first;
            first = second;
            second = temporary;
        }
        near_distance = max(near_distance, first);
        far_distance = min(far_distance, second);
        if (near_distance > far_distance) return false;
    }
    if (far_distance < 0.0f) return false;

    intersections.front.distance = near_distance >= 0.0f
                                       ? near_distance : far_distance;
    intersections.front.point = origin
                                + direction * intersections.front.distance;
    intersections.front.normal = cube_normal(intersections.front.point);
    intersections.has_back = near_distance >= 0.0f
                             && far_distance - near_distance > 1.0e-7f;
    if (intersections.has_back) {
        intersections.back.distance = far_distance;
        intersections.back.point = origin + direction * far_distance;
        intersections.back.normal = cube_normal(intersections.back.point);
    }
    return true;
}

float2 cube_uv(float3 point, float3 normal) {
    float u = 0.5f;
    float v = 0.5f;
    if (fabs(normal.x) > 0.5f) {
        u = normal.x > 0.0f ? (1.0f - point.z) * 0.5f
                            : (point.z + 1.0f) * 0.5f;
        v = (1.0f - point.y) * 0.5f;
    } else if (fabs(normal.y) > 0.5f) {
        u = (point.x + 1.0f) * 0.5f;
        v = normal.y > 0.0f ? (point.z + 1.0f) * 0.5f
                            : (1.0f - point.z) * 0.5f;
    } else {
        u = normal.z > 0.0f ? (point.x + 1.0f) * 0.5f
                            : (1.0f - point.x) * 0.5f;
        v = (1.0f - point.y) * 0.5f;
    }
    return clamp(float2(u, v), 0.0f, 1.0f);
}

float4 sample_cube_hit(const device float4* source,
                       uint width, uint height, CubeHit hit,
                       float2 screen_uv, float curvature,
                       float3 ray_direction, float y_rotation,
                       float lighting) {
    const float2 uv = cube_uv(hit.point, hit.normal);
    const float mapped_u = mix(screen_uv.x, uv.x, curvature);
    const float mapped_v = mix(screen_uv.y, uv.y, curvature);
    float4 sampled = sample_bilinear(
        source, mapped_u * float(width - 1u),
        mapped_v * float(height - 1u), width, height, 3u);
    const float3 lighting_normal = orient_normal(hit.normal, ray_direction);
    const float3 world_normal = rotate_y(
        rotate_x(lighting_normal, -0.35f), y_rotation);
    return shade_surface(sampled, world_normal, lighting * curvature);
}

kernel void surface_mapping(constant FrameConstants& frame [[buffer(0)]],
                            constant GpuSurface& surface [[buffer(1)]],
                            const device float4* source [[buffer(2)]],
                            device float4* output [[buffer(3)]],
                            uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    if (gid.x >= width || gid.y >= height) return;
    const float x = float(gid.x);
    const float y = float(gid.y);
    const float phase = surface.values.x;
    const float curvature = clamp_unit(surface.values.y);
    const float lighting = surface.values.z;
    const float short_side = float(min(width, height));
    const float center_x = 0.5f * float(width - 1u);
    const float center_y = 0.5f * float(height - 1u);
    const float screen_u = width > 1u ? x / float(width - 1u) : 0.5f;
    const float screen_v = height > 1u ? y / float(height - 1u) : 0.5f;
    const float4 planar = source[gid.y * width + gid.x];
    float4 result = planar;
    bool visible = true;

    if (surface.kind.x == 0u) {
        const float dx = x - center_x;
        const float dy = y - center_y;
        const float cosine = cos(-phase);
        const float sine = sin(-phase);
        result = sample_bilinear(
            source, center_x + cosine * dx - sine * dy,
            center_y + sine * dx + cosine * dy,
            width, height, 3u);
    } else if (surface.kind.x == 1u) {
        const float radius = 0.46f * short_side;
        const float normalized_x = (x - center_x) / radius;
        const float normalized_y = (y - center_y) / (0.46f * float(height));
        if (fabs(normalized_x) > 1.0f || fabs(normalized_y) > 1.0f) {
            visible = false;
        } else {
            const float longitude = asin(clamp(normalized_x, -1.0f, 1.0f));
            const float surface_v = 0.5f + 0.5f * normalized_y;
            const float normalized_z = sqrt(max(
                0.0f, 1.0f - normalized_x * normalized_x));
            float4 wrapped = sample_cylinder_side(
                source, width, height, longitude, normalized_x,
                normalized_z, surface_v, phase, lighting);
            if (normalized_z > 1.0e-7f) {
                const float rear_longitude = normalized_x >= 0.0f
                    ? kPi - longitude : -kPi - longitude;
                const float4 rear = sample_cylinder_side(
                    source, width, height, rear_longitude, normalized_x,
                    -normalized_z, surface_v, phase, lighting);
                wrapped = composite_straight_over(wrapped, rear);
            }
            result = mix(planar, wrapped, curvature);
        }
    } else if (surface.kind.x == 2u) {
        const float radius = 0.46f * short_side;
        const float normalized_x = (x - center_x) / radius;
        const float normalized_y = (center_y - y) / radius;
        const float radius_squared = normalized_x * normalized_x
                                     + normalized_y * normalized_y;
        if (radius_squared > 1.0f) {
            visible = false;
        } else {
            const float normalized_z = sqrt(max(0.0f, 1.0f - radius_squared));
            float4 wrapped = sample_sphere_side(
                source, width, height, normalized_x, normalized_y,
                normalized_z, phase, lighting);
            if (normalized_z > 1.0e-7f) {
                const float4 rear = sample_sphere_side(
                    source, width, height, normalized_x, normalized_y,
                    -normalized_z, phase, lighting);
                wrapped = composite_straight_over(wrapped, rear);
            }
            result = mix(planar, wrapped, curvature);
        }
    } else {
        const float scale = 0.52f * short_side;
        const float screen_x = (x - center_x) / scale;
        const float screen_y = (center_y - y) / scale;
        float3 origin = float3(0.0f, 0.0f, 3.4f);
        float3 direction = normalize(float3(screen_x, screen_y, -2.5f));
        const float fixed_x_rotation = -0.35f;
        const float y_rotation = 0.55f + phase;
        origin = rotate_x(rotate_y(origin, -y_rotation),
                          -fixed_x_rotation);
        direction = rotate_x(rotate_y(direction, -y_rotation),
                             -fixed_x_rotation);
        CubeIntersections intersections;
        if (!intersect_cube(origin, direction, intersections)) {
            visible = false;
        } else {
            const float2 screen_uv = float2(screen_u, screen_v);
            const float4 front = sample_cube_hit(
                source, width, height, intersections.front, screen_uv,
                curvature, direction, y_rotation, lighting);
            result = front;
            if (intersections.has_back) {
                const float4 back = sample_cube_hit(
                    source, width, height, intersections.back, screen_uv,
                    curvature, direction, y_rotation, lighting);
                result = mix(front,
                             composite_straight_over(front, back), curvature);
            }
        }
    }

    if (!visible) result = mix(planar, float4(0.0f), curvature);
    result.a = clamp_unit(result.a);
    output[gid.y * width + gid.x] = result;
}

kernel void coordinate_effect(constant FrameConstants& frame [[buffer(0)]],
                              constant GpuEffect& effect [[buffer(1)]],
                              const device float4* source [[buffer(2)]],
                              device float4* output [[buffer(3)]],
                              uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    if (gid.x >= width || gid.y >= height) return;
    const float x = float(gid.x);
    const float y = float(gid.y);
    const float center_x = effect.placement.y * float(width - 1u);
    const float center_y = effect.placement.z * float(height - 1u);
    const float intensity = max(0.0f, effect.primary.y);
    const float base_displacement = effect.primary.z * frame.phases.w;
    const float displacement = base_displacement * intensity;
    const float axis_x = cos(effect.placement.w);
    const float axis_y = sin(effect.placement.w);
    const float perpendicular_x = -axis_y;
    const float perpendicular_y = axis_x;
    const float area = circular_influence(
        effect.placement.y, effect.placement.z, effect.glow_area.w,
        x, y, width, height);
    float sample_x = x;
    float sample_y = y;
    float4 sampled;
    if (effect.kind.x == 0u) {
        if (effect.primary.z <= 1.0e-7f || intensity <= 1.0e-7f) {
            sampled = source[gid.y * width + gid.x];
        } else {
            const float fraction = wrap_unit(effect.primary.x / kTau);
            const float zoom_blend = smooth_unit(fraction);
            const float octaves = clamp(
                effect.primary.z * max(0.01f, effect.primary.w)
                    * max(1.0f, intensity),
                0.0f, 4.0f);
            const float ratio = pow(2.0f, octaves);
            const float scale_a = pow(ratio, fraction);
            const float scale_b = ratio > 1.0e-7f ? scale_a / ratio : scale_a;
            const float rx = x - center_x;
            const float ry = y - center_y;
            const float4 first = sample_bilinear(
                source, center_x + rx / scale_a, center_y + ry / scale_a,
                width, height, effect.kind.z);
            const float4 second = sample_bilinear(
                source, center_x + rx / scale_b, center_y + ry / scale_b,
                width, height, effect.kind.z);
            const float4 zoomed = mix(first, second, zoom_blend);
            sampled = mix(source[gid.y * width + gid.x], zoomed,
                          clamp_unit(intensity * area));
        }
    } else if (effect.kind.x == 1u) {
        const float dx = x - center_x;
        const float dy = y - center_y;
        const float distance = length(float2(dx, dy));
        if (distance > 1.0e-7f) {
            const float wave = sin(kTau * effect.primary.w
                                   * (distance / frame.phases.w)
                                   - effect.primary.x);
            const float attenuation = effect.placement.x > 1.0f
                ? 1.0f / (1.0f + (effect.placement.x - 1.0f)
                                   * distance / frame.phases.w)
                : 1.0f;
            sample_x -= dx / distance * displacement * wave * attenuation * area;
            sample_y -= dy / distance * displacement * wave * attenuation * area;
        }
        sampled = sample_bilinear(source, sample_x, sample_y,
                                  width, height, effect.kind.z);
    } else if (effect.kind.x == 2u) {
        const int harmonic = clamp(
            int(floor(effect.primary.w + 0.5f)), 1, 1000);
        const float phase = effect.primary.x;
        const float shake_x = displacement
            * (0.72f * sin(float(harmonic) * phase)
               + 0.20f * sin(float(harmonic + 2) * phase + 1.234f)
               + 0.08f * sin(float(harmonic + 4) * phase + 3.456f));
        const float shake_y = displacement * effect.placement.x
            * (0.70f * cos(float(harmonic + 1) * phase + 0.731f)
               + 0.22f * sin(float(harmonic + 3) * phase + 2.718f)
               + 0.08f * cos(float(harmonic + 5) * phase + 4.123f));
        const float rotated_x = axis_x * shake_x - axis_y * shake_y;
        const float rotated_y = axis_y * shake_x + axis_x * shake_y;
        sampled = sample_bilinear(source, x - rotated_x * area,
                                  y - rotated_y * area,
                                  width, height, effect.kind.z);
    } else if (effect.kind.x == 3u) {
        const float dx = x - center_x;
        const float dy = y - center_y;
        const float along = (dx * axis_x + dy * axis_y) / frame.phases.w;
        const float flag = sin(kTau * effect.primary.w * along
                               - effect.primary.x);
        const float harmonic = sin(kTau * effect.primary.w * 0.5f * along
                                   - 2.0f * effect.primary.x + 1.0472f);
        const float amount = displacement
            * (flag + effect.placement.x * 0.35f * harmonic) * area;
        sample_x -= perpendicular_x * amount;
        sample_y -= perpendicular_y * amount;
        sampled = sample_bilinear(source, sample_x, sample_y,
                                  width, height, effect.kind.z);
    } else if (effect.kind.x == 8u) {
        const uint bands = max(1u, uint(floor(effect.primary.w + 0.5f)));
        const uint band = min(
            bands - 1u, uint((ulong(gid.y) * ulong(bands))
                             / ulong(max(1u, height))));
        const ulong effect_id = (ulong(effect.blur.w) << 32u)
                                | ulong(effect.kind.w);
        const ulong band_seed = generated_shape_hash(
            effect_id ^ (ulong(band) + 1ul) * 0xd1b54a32d192ed03ul);
        const float random = generated_shape_unit(band_seed);
        const float band_offset = base_displacement
            * (2.0f * random - 1.0f)
            * sin(effect.primary.x + kTau * random) * area;
        const float split = fabs(base_displacement)
                            * effect.placement.x * area;
        const float4 middle = sample_bilinear(
            source, x - band_offset, y, width, height, effect.kind.z);
        const float4 red = sample_bilinear(
            source, x - band_offset - split, y,
            width, height, effect.kind.z);
        const float4 blue = sample_bilinear(
            source, x - band_offset + split, y,
            width, height, effect.kind.z);
        const float4 glitched = float4(
            red.r, middle.g, blue.b, max(red.a, max(middle.a, blue.a)));
        sampled = mix(source[gid.y * width + gid.x], glitched,
                      clamp_unit(intensity * area));
    } else if (effect.kind.x == 9u) {
        const float dx = x - center_x;
        const float dy = y - center_y;
        const float distance = length(float2(dx, dy));
        if (distance <= 1.0e-7f) {
            sampled = source[gid.y * width + gid.x];
        } else {
            const float ray_angle = atan2(dy, dx) - effect.placement.w;
            const float ray = max(
                0.0f, cos(effect.primary.w * ray_angle - effect.primary.x));
            const float sharp = pow(
                ray, 1.0f + 15.0f * effect.placement.x);
            const float travel = base_displacement * sharp
                * sin(effect.primary.x + kTau * distance / frame.phases.w)
                * area;
            const float4 burst = sample_bilinear(
                source, x - dx / distance * travel,
                y - dy / distance * travel, width, height, effect.kind.z);
            sampled = mix(source[gid.y * width + gid.x], burst,
                          clamp_unit(intensity * area));
        }
    } else if (effect.kind.x == 10u) {
        const float dx = x - center_x;
        const float dy = y - center_y;
        const float normalized_radius = length(float2(dx, dy))
                                        / frame.phases.w;
        const float pulse = 0.75f + 0.25f * sin(effect.primary.x);
        const float radial = pow(normalized_radius, effect.primary.w);
        const float scale = max(
            0.05f, 1.0f + effect.placement.x * effect.primary.z
                            * pulse * radial * area);
        const float4 distorted = sample_bilinear(
            source, center_x + dx * scale, center_y + dy * scale,
            width, height, effect.kind.z);
        sampled = mix(source[gid.y * width + gid.x], distorted,
                      clamp_unit(intensity * area));
    } else {
        sampled = source[gid.y * width + gid.x];
    }
    sampled.a = clamp_unit(sampled.a);
    output[gid.y * width + gid.x] = sampled;
}

kernel void block_scale(constant FrameConstants& frame [[buffer(0)]],
                        constant GpuEffect& effect [[buffer(1)]],
                        const device float4* source [[buffer(2)]],
                        device float4* output [[buffer(3)]],
                        uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    float travel = 0.5f - 0.5f * cos(effect.primary.x);
    const int steps = int(floor(effect.placement.x + 0.5f));
    if (steps > 0) travel = round(travel * float(steps)) / float(steps);
    const float multiplier = mix(effect.primary.z, effect.primary.w, travel);
    const float requested = float(frame.dimensions_counts.z) * multiplier;
    const uint maximum_size = max(width, height);
    const uint block_size = uint(max(
        1.0f, min(float(maximum_size), floor(min(requested,
                                                 float(maximum_size)) + 0.5f))));
    const uint block_x = gid.x * block_size;
    const uint block_y = gid.y * block_size;
    if (block_x >= width || block_y >= height) return;
    const uint end_x = min(block_x + block_size, width);
    const uint end_y = min(block_y + block_size, height);
    float4 average = float4(0.0f);
    uint count = 0u;
    for (uint y = block_y; y < end_y; ++y) {
        for (uint x = block_x; x < end_x; ++x) {
            average += source[y * width + x];
            ++count;
        }
    }
    average /= float(count);
    const float amount = clamp_unit(effect.primary.y);
    for (uint y = block_y; y < end_y; ++y) {
        for (uint x = block_x; x < end_x; ++x) {
            float4 value = mix(source[y * width + x], average, amount);
            value.a = clamp_unit(value.a);
            output[y * width + x] = value;
        }
    }
}

kernel void particle_field(
    constant FrameConstants& frame [[buffer(0)]],
    constant GpuEffect& effect [[buffer(1)]],
    constant GpuParticleGrid& grid [[buffer(2)]],
    const device GpuParticlePoint* points [[buffer(3)]],
    const device uint* tile_offsets [[buffer(4)]],
    const device uint* tile_indices [[buffer(5)]],
    const device float4* source [[buffer(6)]],
    device float4* output [[buffer(7)]],
    uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    if (gid.x >= width || gid.y >= height) return;
    float4 color = source[gid.y * width + gid.x];
    const uint tile_x = gid.x / grid.layout.z;
    const uint tile_y = gid.y / grid.layout.z;
    const uint tile = tile_y * grid.layout.x + tile_x;
    const uint begin = tile_offsets[tile];
    const uint end = tile_offsets[tile + 1u];
    const float base_brightness = max(0.0f, effect.primary.y);
    const float core = clamp_unit(effect.glow_area.y);
    const float softness = max(0.05f, effect.glow_area.z);
    for (uint slot = begin; slot < end; ++slot) {
        const uint point_index = tile_indices[slot];
        if (point_index >= grid.layout.w) continue;
        const float4 point = points[point_index].geometry;
        const float dx = (float(gid.x) - point.x) / point.z;
        const float dy = (float(gid.y) - point.y) / point.z;
        const float distance_squared = dx * dx + dy * dy;
        if (distance_squared > 6.25f) continue;
        const float gaussian = exp(
            -distance_squared / (0.22f + 1.55f * softness));
        const float area = circular_influence(
            effect.placement.y, effect.placement.z, effect.glow_area.w,
            float(gid.x), float(gid.y), width, height);
        const float amount = gaussian * point.w * area;
        if (amount <= 1.0e-8f) continue;
        const float white_core = pow(gaussian, 1.0f + 5.0f * core);
        const float particle_alpha = clamp_unit(amount);
        const float previous_alpha = clamp_unit(color.a);
        const float combined_alpha = particle_alpha
            + previous_alpha * (1.0f - particle_alpha);
        const float3 emission = base_brightness * float3(
            1.20f + 0.80f * white_core,
            0.28f + 0.72f * white_core,
            0.05f + 0.65f * white_core);
        if (combined_alpha > 1.0e-12f) {
            color.rgb = (color.rgb * previous_alpha
                         + emission * particle_alpha)
                        / combined_alpha;
        }
        color.a = combined_alpha;
    }
    output[gid.y * width + gid.x] = color;
}

kernel void glow_extract(constant FrameConstants& frame [[buffer(0)]],
                         constant GpuEffect& effect [[buffer(1)]],
                         const device float4* source [[buffer(2)]],
                         device float4* output [[buffer(3)]],
                         uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    if (gid.x >= width || gid.y >= height) return;
    const float4 input = source[gid.y * width + gid.x];
    const float luminance = dot(input.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    const float threshold = effect.glow_area.y;
    const float knee = max(1.0e-6f, threshold * effect.glow_area.z);
    float soft = clamp(luminance - threshold + knee, 0.0f, 2.0f * knee);
    soft = soft * soft / (4.0f * knee + 1.0e-7f);
    const float contribution = max(luminance - threshold, soft);
    const float bloom = luminance > 1.0e-7f
        ? clamp_unit(contribution / luminance) : 0.0f;
    const float area = circular_influence(
        effect.placement.y, effect.placement.z, effect.glow_area.w,
        float(gid.x), float(gid.y), width, height);
    output[gid.y * width + gid.x] =
        float4(input.rgb, clamp_unit(input.a * area * bloom));
}

float4 blur_pixel(const device float4* source, int x, int y,
                  uint width, uint height, float radius, bool horizontal) {
    const float spacing = radius / 4.0f;
    float4 accumulated = float4(0.0f);
    for (int tap = -4; tap <= 4; ++tap) {
        const int offset = int(round(float(tap) * spacing));
        const int sx = horizontal ? x + offset : x;
        const int sy = horizontal ? y : y + offset;
        const float4 sample = sample_texel(
            source, sx, sy, width, height, 3u);
        const float weight = kBlurWeights[tap + 4];
        accumulated.rgb += sample.rgb * sample.a * weight;
        accumulated.a += sample.a * weight;
    }
    if (accumulated.a > 1.0e-7f) accumulated.rgb /= accumulated.a;
    else accumulated.rgb = float3(0.0f);
    accumulated.a = clamp_unit(accumulated.a);
    return accumulated;
}

kernel void glow_blur_horizontal(
    constant FrameConstants& frame [[buffer(0)]],
    constant GpuEffect& effect [[buffer(1)]],
    const device float4* source [[buffer(2)]],
    device float4* output [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= frame.dimensions_counts.x
        || gid.y >= frame.dimensions_counts.y) return;
    output[gid.y * frame.dimensions_counts.x + gid.x] = blur_pixel(
        source, int(gid.x), int(gid.y), frame.dimensions_counts.x,
        frame.dimensions_counts.y, effect.glow_area.x, true);
}

kernel void glow_blur_vertical(
    constant FrameConstants& frame [[buffer(0)]],
    constant GpuEffect& effect [[buffer(1)]],
    const device float4* source [[buffer(2)]],
    device float4* output [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= frame.dimensions_counts.x
        || gid.y >= frame.dimensions_counts.y) return;
    output[gid.y * frame.dimensions_counts.x + gid.x] = blur_pixel(
        source, int(gid.x), int(gid.y), frame.dimensions_counts.x,
        frame.dimensions_counts.y, effect.glow_area.x, false);
}

kernel void configurable_blur(
    constant FrameConstants& frame [[buffer(0)]],
    constant GpuEffect& effect [[buffer(1)]],
    const device float4* source [[buffer(2)]],
    device float4* output [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    if (gid.x >= width || gid.y >= height) return;
    const uint blur_type = effect.blur.x;
    const uint samples = effect.blur.y;
    const bool horizontal = effect.blur.w != 0u;
    const float center = 0.5f * float(samples - 1u);
    const float denominator = max(1.0f, center);
    const float direction_x = cos(effect.placement.w);
    const float direction_y = sin(effect.placement.w);
    const float center_x = effect.placement.y * float(width - 1u);
    const float center_y = effect.placement.z * float(height - 1u);
    const float short_side = float(max(1u, min(width, height)));
    float3 premultiplied = float3(0.0f);
    float alpha_sum = 0.0f;
    float total_weight = 0.0f;
    for (uint tap = 0u; tap < samples; ++tap) {
        const float normalized = (float(tap) - center) / denominator;
        const float offset = normalized * effect.glow_area.x;
        float sample_x = float(gid.x);
        float sample_y = float(gid.y);
        if (blur_type <= 1u) {
            if (horizontal) sample_x += offset;
            else sample_y += offset;
        } else if (blur_type == 2u) {
            sample_x += direction_x * offset;
            sample_y += direction_y * offset;
        } else if (blur_type == 3u) {
            const float theta = offset / short_side;
            const float cosine = cos(theta);
            const float sine = sin(theta);
            const float relative_x = float(gid.x) - center_x;
            const float relative_y = float(gid.y) - center_y;
            sample_x = center_x + relative_x * cosine - relative_y * sine;
            sample_y = center_y + relative_x * sine + relative_y * cosine;
        } else {
            const float scale = max(0.01f, 1.0f + offset / short_side);
            sample_x = center_x + (float(gid.x) - center_x) * scale;
            sample_y = center_y + (float(gid.y) - center_y) * scale;
        }
        const float weight = blur_type == 0u
            ? exp(-4.5f * normalized * normalized) : 1.0f;
        const float4 sample = sample_bilinear(
            source, sample_x, sample_y, width, height, effect.kind.z);
        premultiplied += sample.rgb * sample.a * weight;
        alpha_sum += sample.a * weight;
        total_weight += weight;
    }
    float4 result = float4(0.0f);
    result.a = total_weight > 0.0f ? alpha_sum / total_weight : 0.0f;
    result.rgb = alpha_sum > 1.0e-7f
        ? premultiplied / alpha_sum : float3(0.0f);
    output[gid.y * width + gid.x] = result;
}

kernel void blur_combine(
    constant FrameConstants& frame [[buffer(0)]],
    constant GpuEffect& effect [[buffer(1)]],
    device float4* image [[buffer(2)]],
    const device float4* blurred [[buffer(3)]],
    uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    if (gid.x >= width || gid.y >= height) return;
    const uint offset = gid.y * width + gid.x;
    const float area = circular_influence(
        effect.placement.y, effect.placement.z, effect.glow_area.w,
        float(gid.x), float(gid.y), width, height);
    float4 result = mix(image[offset], blurred[offset],
                        clamp_unit(effect.primary.y * area));
    result.a = clamp_unit(result.a);
    image[offset] = result;
}

kernel void glow_combine(constant FrameConstants& frame [[buffer(0)]],
                         constant GpuEffect& effect [[buffer(1)]],
                         device float4* image [[buffer(2)]],
                         const device float4* glow [[buffer(3)]],
                         uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    if (gid.x >= width || gid.y >= frame.dimensions_counts.y) return;
    const uint offset = gid.y * width + gid.x;
    float4 original = image[offset];
    const float4 halo = glow[offset];
    const float pulse_depth = clamp_unit(fabs(effect.placement.x));
    const float pulse = 0.5f + 0.5f * sin(effect.primary.x);
    const float intensity = effect.primary.y
                            * mix(1.0f, pulse, pulse_depth);
    const float coverage = clamp_unit(intensity * halo.a);
    const float output_alpha =
        original.a + coverage * (1.0f - original.a);
    if (output_alpha > 1.0e-7f) {
        original.rgb = (original.rgb * original.a
                        + intensity * halo.rgb * halo.a) / output_alpha;
    }
    original.a = clamp_unit(output_alpha);
    image[offset] = original;
}

kernel void transform_image(constant FrameConstants& frame [[buffer(0)]],
                            const device float4* source [[buffer(1)]],
                            device float4* output [[buffer(2)]],
                            uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    if (gid.x >= width || gid.y >= height) return;
    int x = frame.transform_quant.x != 0u
                ? int(width - 1u - gid.x) : int(gid.x);
    int y = frame.transform_quant.y != 0u
                ? int(height - 1u - gid.y) : int(gid.y);
    const int mirror = frame.signed_values.w;
    if ((mirror == 1 || mirror == 5) && x >= int((width + 1u) / 2u)) {
        x = int(width - 1u) - x;
    } else if (mirror == 2 && x < int(width / 2u)) {
        x = int(width - 1u) - x;
    }
    if ((mirror == 3 || mirror == 5) && y >= int((height + 1u) / 2u)) {
        y = int(height - 1u) - y;
    } else if (mirror == 4 && y < int(height / 2u)) {
        y = int(height - 1u) - y;
    }
    output[gid.y * width + gid.x] = source[uint(y) * width + uint(x)];
}

kernel void post_process_invert(
    constant FrameConstants& frame [[buffer(0)]],
    device float4* image [[buffer(1)]],
    uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    if (gid.x >= width || gid.y >= frame.dimensions_counts.y) return;
    const uint offset = gid.y * width + gid.x;
    float4 color = image[offset];
    if (frame.post_flags.x != 0u && frame.post_values.x > 0.0f) {
        color.rgb = mix(color.rgb, 1.0f - color.rgb,
                        frame.post_values.x);
    }
    if (frame.post_flags.y != 0u && frame.post_values.y > 0.0f) {
        color.a = mix(color.a, 1.0f - color.a, frame.post_values.y);
    }
    image[offset] = color;
}

float4 post_premultiply(float4 color) {
    color.rgb *= color.a;
    return color;
}

float post_contrast(float4 first, float4 second) {
    const float4 difference = fabs(first - second);
    return max(max(difference.r, difference.g),
               max(difference.b, difference.a));
}

kernel void post_process_antialias(
    constant FrameConstants& frame [[buffer(0)]],
    const device float4* source [[buffer(1)]],
    device float4* destination [[buffer(2)]],
    uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    const uint height = frame.dimensions_counts.y;
    if (gid.x >= width || gid.y >= height) return;
    const int2 coordinate = int2(gid);
    const int2 maximum = int2(int(width) - 1, int(height) - 1);
    const int2 neighbor_coordinates[4] = {
        clamp(coordinate + int2(-1, 0), int2(0), maximum),
        clamp(coordinate + int2(1, 0), int2(0), maximum),
        clamp(coordinate + int2(0, -1), int2(0), maximum),
        clamp(coordinate + int2(0, 1), int2(0), maximum)};
    const uint offset = gid.y * width + gid.x;
    const float4 center = source[offset];
    const float4 center_premultiplied = post_premultiply(center);
    float4 filtered = 0.5f * center_premultiplied;
    float contrast = 0.0f;
    for (uint index = 0u; index < 4u; ++index) {
        const int2 sample_coordinate = neighbor_coordinates[index];
        const uint sample_offset = uint(sample_coordinate.y) * width
                                   + uint(sample_coordinate.x);
        const float4 sample = post_premultiply(source[sample_offset]);
        contrast = max(contrast,
                       post_contrast(center_premultiplied, sample));
        filtered += 0.125f * sample;
    }
    const float threshold = frame.post_values.w;
    if (contrast <= threshold) {
        destination[offset] = center;
        return;
    }
    const float transition = max(1.0e-12f, 1.0f - threshold);
    const float amount = frame.post_values.z
        * clamp((contrast - threshold) / transition, 0.0f, 1.0f);
    const float4 mixed_premultiplied = mix(
        center_premultiplied, filtered, amount);
    float4 output = float4(0.0f, 0.0f, 0.0f,
                           mixed_premultiplied.a);
    if (output.a > 1.0e-7f) {
        output.rgb = mixed_premultiplied.rgb / output.a;
    }
    destination[offset] = output;
}

float3 rgb_to_hsv(float3 rgb) {
    const float maximum = max(rgb.r, max(rgb.g, rgb.b));
    const float minimum = min(rgb.r, min(rgb.g, rgb.b));
    const float delta = maximum - minimum;
    float hue = 0.0f;
    if (delta > 1.0e-7f) {
        if (maximum == rgb.r) hue = fmod((rgb.g - rgb.b) / delta, 6.0f);
        else if (maximum == rgb.g) hue = (rgb.b - rgb.r) / delta + 2.0f;
        else hue = (rgb.r - rgb.g) / delta + 4.0f;
        hue /= 6.0f;
        if (hue < 0.0f) hue += 1.0f;
    }
    const float saturation = maximum > 1.0e-7f ? delta / maximum : 0.0f;
    return float3(hue, saturation, maximum);
}

float3 hsv_to_rgb(float3 hsv) {
    const float hue = wrap_unit(hsv.x);
    const float saturation = clamp_unit(hsv.y);
    const float chroma = hsv.z * saturation;
    const float sector = hue * 6.0f;
    const float intermediate =
        chroma * (1.0f - fabs(fmod(sector, 2.0f) - 1.0f));
    const float match = hsv.z - chroma;
    float3 rgb = float3(0.0f);
    if (sector < 1.0f) rgb = float3(chroma, intermediate, 0.0f);
    else if (sector < 2.0f) rgb = float3(intermediate, chroma, 0.0f);
    else if (sector < 3.0f) rgb = float3(0.0f, chroma, intermediate);
    else if (sector < 4.0f) rgb = float3(0.0f, intermediate, chroma);
    else if (sector < 5.0f) rgb = float3(intermediate, 0.0f, chroma);
    else rgb = float3(chroma, 0.0f, intermediate);
    return rgb + match;
}

kernel void quantize_image(constant FrameConstants& frame [[buffer(0)]],
                           device float4* image [[buffer(1)]],
                           uint2 gid [[thread_position_in_grid]]) {
    const uint width = frame.dimensions_counts.x;
    if (gid.x >= width || gid.y >= frame.dimensions_counts.y) return;
    const uint offset = gid.y * width + gid.x;
    float4 color = image[offset];
    float3 quantized = color.rgb;
    const float levels = float(frame.quant_values.x);
    const uint mode = frame.transform_quant.w;
    if (mode == 0u) {
        const float maximum_index = levels - 1.0f;
        quantized = round(clamp(color.rgb, 0.0f, 1.0f) * maximum_index)
                    / maximum_index;
    } else if (mode == 1u) {
        const float luminance = dot(color.rgb,
                                    float3(0.2126f, 0.7152f, 0.0722f));
        const float maximum_index = levels - 1.0f;
        const float q = round(clamp_unit(luminance) * maximum_index)
                        / maximum_index;
        quantized = luminance > 1.0e-7f
                        ? color.rgb * (q / luminance)
                        : float3(0.0f);
    } else {
        float3 hsv = rgb_to_hsv(color.rgb);
        hsv.x = wrap_unit(round(hsv.x * levels) / levels);
        quantized = hsv_to_rgb(hsv);
    }
    color.rgb = mix(color.rgb, quantized, frame.alpha_quant.w);
    image[offset] = color;
}
