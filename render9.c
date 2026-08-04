#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TAU (2.0 * M_PI)
#define CHANNELS 3
#define WAVE_COUNT 3
#define MAX_OUTPUT_PATH 4096
#define MAX_PREFIX_LENGTH 128
#define MAX_DIMENSION 16384
#define MAX_TOTAL_FRAMES 1000000

typedef struct {
    int enabled;
    double x_percent;
    double y_percent;
    double amplitude;
    double spatial_frequency;
    int cycles_per_loop;
    double phase_degrees;
} WaveConfig;

typedef struct {
    int width;
    int height;
    int block_size;
    int total_frames;
    double fps;

    WaveConfig waves[WAVE_COUNT];

    double swing_amount;
    int swing_cycles;
    double phrase_warp;
    double ghost_mix;
    double ghost_lag_degrees;

    double displacement;
    double wave_depth;
    double spiral_frequency;
    int spiral_arms;
    double wall_frequency;
    double wall_mix;
    int hue_cycles;
    double saturation;

    char output_directory[MAX_OUTPUT_PATH];
    char filename_prefix[MAX_PREFIX_LENGTH];
    int first_frame_number;
    int filename_digits;
    int overwrite_existing;
} RenderConfig;

static RenderConfig default_config(void) {
    RenderConfig config;

    memset(&config, 0, sizeof(config));
    config.width = 1920;
    config.height = 1080;
    config.block_size = 16;
    config.total_frames = 480;
    config.fps = 60.0;

    config.waves[0] = (WaveConfig){1, 50.0000, 50.0000, 0.55, 3.7815, 1, 25.7831};
    config.waves[1] = (WaveConfig){1, 29.1667, 26.8519, 0.25, 9.4538, 1, 25.7831};
    config.waves[2] = (WaveConfig){1, 70.8333, 73.1481, 0.20, 12.0321, 1, 25.7831};

    config.swing_amount = 0.28;
    config.swing_cycles = 16;
    config.phrase_warp = 0.05;
    config.ghost_mix = 0.25;
    config.ghost_lag_degrees = 5.7296;

    config.displacement = 32.0;
    config.wave_depth = 0.85;
    config.spiral_frequency = 3.4377;
    config.spiral_arms = 4;
    config.wall_frequency = 6.0161;
    config.wall_mix = 0.45;
    config.hue_cycles = 2;
    config.saturation = 1.0;

    strcpy(config.output_directory, ".");
    strcpy(config.filename_prefix, "frame_");
    config.first_frame_number = 0;
    config.filename_digits = 4;
    config.overwrite_existing = 0;

    return config;
}

static double clamp_double(double value, double minimum, double maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static void hsl_to_rgb(double hue, double saturation, double lightness,
                       unsigned char *red, unsigned char *green, unsigned char *blue) {
    double chroma;
    double hue_prime;
    double x;
    double match;
    double red_prime = 0.0;
    double green_prime = 0.0;
    double blue_prime = 0.0;

    hue = fmod(hue, 360.0);
    if (hue < 0.0) {
        hue += 360.0;
    }
    saturation = clamp_double(saturation, 0.0, 1.0);
    lightness = clamp_double(lightness, 0.0, 1.0);

    chroma = (1.0 - fabs(2.0 * lightness - 1.0)) * saturation;
    hue_prime = hue / 60.0;
    x = chroma * (1.0 - fabs(fmod(hue_prime, 2.0) - 1.0));
    match = lightness - chroma / 2.0;

    if (hue_prime < 1.0) {
        red_prime = chroma;
        green_prime = x;
    } else if (hue_prime < 2.0) {
        red_prime = x;
        green_prime = chroma;
    } else if (hue_prime < 3.0) {
        green_prime = chroma;
        blue_prime = x;
    } else if (hue_prime < 4.0) {
        green_prime = x;
        blue_prime = chroma;
    } else if (hue_prime < 5.0) {
        red_prime = x;
        blue_prime = chroma;
    } else {
        red_prime = chroma;
        blue_prime = x;
    }

    *red = (unsigned char)lround(clamp_double(red_prime + match, 0.0, 1.0) * 255.0);
    *green = (unsigned char)lround(clamp_double(green_prime + match, 0.0, 1.0) * 255.0);
    *blue = (unsigned char)lround(clamp_double(blue_prime + match, 0.0, 1.0) * 255.0);
}

static double motion_phase_for_loop(const RenderConfig *config, double loop_phase) {
    return loop_phase
           + config->swing_amount * sin((double)config->swing_cycles * loop_phase)
           + config->phrase_warp * sin(loop_phase);
}

static double wave_height(const RenderConfig *config, double x, double y,
                          double motion_phase) {
    double short_side = (double)(config->width < config->height
                                     ? config->width
                                     : config->height);
    double height = 0.0;
    int wave_index;

    for (wave_index = 0; wave_index < WAVE_COUNT; ++wave_index) {
        const WaveConfig *wave = &config->waves[wave_index];
        double source_x;
        double source_y;
        double normalized_distance;
        double phase_offset;

        if (!wave->enabled) {
            continue;
        }

        source_x = (wave->x_percent / 100.0) * (double)config->width;
        source_y = (wave->y_percent / 100.0) * (double)config->height;
        normalized_distance = hypot(x - source_x, y - source_y) / short_side;
        phase_offset = wave->phase_degrees * M_PI / 180.0;

        height += wave->amplitude
                  * sin(TAU * wave->spatial_frequency * normalized_distance
                        - (double)wave->cycles_per_loop * motion_phase
                        + phase_offset);
    }

    return height;
}

static const WaveConfig *primary_wave(const RenderConfig *config) {
    int wave_index;

    for (wave_index = 0; wave_index < WAVE_COUNT; ++wave_index) {
        if (config->waves[wave_index].enabled) {
            return &config->waves[wave_index];
        }
    }
    return &config->waves[0];
}

static void render_frame_at_phase(const RenderConfig *config, unsigned char *pixels,
                                  double loop_phase) {
    const WaveConfig *anchor = primary_wave(config);
    double short_side = (double)(config->width < config->height
                                     ? config->width
                                     : config->height);
    double center_x = (anchor->x_percent / 100.0) * (double)config->width;
    double center_y = (anchor->y_percent / 100.0) * (double)config->height;
    double motion_phase;
    double ghost_phase;
    double phrase_swell;
    double breath_3d;
    int block_y;

    /* Keep the periodic endpoint numerically identical even for extreme settings. */
    loop_phase = fmod(loop_phase, TAU);
    if (loop_phase < 0.0) {
        loop_phase += TAU;
    }
    motion_phase = motion_phase_for_loop(config, loop_phase);
    ghost_phase = motion_phase - config->ghost_lag_degrees * M_PI / 180.0;
    phrase_swell = sin(loop_phase);
    breath_3d = 0.85 + 0.35 * phrase_swell;

    for (block_y = 0; block_y < config->height; block_y += config->block_size) {
        int block_x;

        for (block_x = 0; block_x < config->width; block_x += config->block_size) {
            double height_here = wave_height(config, (double)block_x, (double)block_y,
                                             motion_phase);
            double height_right = wave_height(config,
                                              (double)(block_x + config->block_size),
                                              (double)block_y, motion_phase);
            double height_down = wave_height(config, (double)block_x,
                                             (double)(block_y + config->block_size),
                                             motion_phase);
            double slope_x = height_right - height_here;
            double slope_y = height_down - height_here;
            double displaced_x = (double)block_x
                                 + slope_x * config->displacement * breath_3d;
            double displaced_y = (double)block_y
                                 + slope_y * config->displacement * breath_3d;
            double dx = displaced_x - center_x;
            double dy = displaced_y - center_y;
            double normalized_distance = hypot(dx, dy) / short_side;
            double angle = atan2(dy, dx);
            double wall_distance_pixels;
            double normalized_wall_distance;
            double main_spiral;
            double main_reflection;
            double ghost_spiral;
            double ghost_reflection;
            double main_signal;
            double ghost_signal;
            double combined_signal;
            double hue;
            double light_reflection;
            double normalized_light;
            double lightness;
            unsigned char red;
            unsigned char green;
            unsigned char blue;
            int end_x;
            int end_y;
            int y;

            wall_distance_pixels = fmin(
                fmin((double)block_x, (double)(config->width - 1 - block_x)),
                fmin((double)block_y, (double)(config->height - 1 - block_y)));
            normalized_wall_distance = wall_distance_pixels / short_side;

            main_spiral = sin(TAU * config->spiral_frequency * normalized_distance
                              + angle * (double)config->spiral_arms - motion_phase);
            main_reflection = sin(TAU * config->wall_frequency * normalized_wall_distance
                                  + 2.0 * motion_phase);
            ghost_spiral = sin(TAU * config->spiral_frequency * normalized_distance
                               + angle * (double)config->spiral_arms - ghost_phase);
            ghost_reflection = sin(TAU * config->wall_frequency * normalized_wall_distance
                                   + 2.0 * ghost_phase);

            main_signal = main_spiral + config->wall_mix * main_reflection;
            ghost_signal = ghost_spiral + config->wall_mix * ghost_reflection;
            combined_signal = (1.0 - config->ghost_mix) * main_signal
                              + config->ghost_mix * ghost_signal;
            hue = (combined_signal + 1.45) * 260.0
                  + 360.0 * (double)config->hue_cycles * (loop_phase / TAU);

            light_reflection = slope_x * -0.7071067811865476
                               + slope_y * -0.7071067811865476;
            normalized_light = light_reflection * config->wave_depth * breath_3d;
            if (normalized_light < 0.0) {
                lightness = 0.40 + 0.36 * normalized_light;
            } else {
                lightness = 0.40 + 0.28 * normalized_light;
            }
            lightness = clamp_double(lightness, 0.04, 0.68);

            hsl_to_rgb(hue, config->saturation, lightness, &red, &green, &blue);

            end_x = block_x + config->block_size;
            if (end_x > config->width) {
                end_x = config->width;
            }
            end_y = block_y + config->block_size;
            if (end_y > config->height) {
                end_y = config->height;
            }

            for (y = block_y; y < end_y; ++y) {
                int x;
                size_t pixel_index = ((size_t)y * (size_t)config->width
                                      + (size_t)block_x) * CHANNELS;

                for (x = block_x; x < end_x; ++x) {
                    pixels[pixel_index] = red;
                    pixels[pixel_index + 1] = green;
                    pixels[pixel_index + 2] = blue;
                    pixel_index += CHANNELS;
                }
            }
        }
    }
}

static void render_frame(const RenderConfig *config, unsigned char *pixels, int frame_index) {
    int wrapped_frame = frame_index % config->total_frames;
    double loop_phase;

    if (wrapped_frame < 0) {
        wrapped_frame += config->total_frames;
    }
    loop_phase = TAU * (double)wrapped_frame / (double)config->total_frames;
    render_frame_at_phase(config, pixels, loop_phase);
}

static int checked_buffer_size(const RenderConfig *config, size_t *buffer_size) {
    size_t width = (size_t)config->width;
    size_t height = (size_t)config->height;

    if (width > SIZE_MAX / height || width * height > SIZE_MAX / CHANNELS) {
        return 0;
    }
    *buffer_size = width * height * CHANNELS;
    return 1;
}

static int validate_prefix(const char *prefix) {
    const unsigned char *cursor = (const unsigned char *)prefix;

    if (*cursor == '\0') {
        return 0;
    }
    while (*cursor != '\0') {
        if (*cursor == '/' || *cursor == '\\' || iscntrl(*cursor)) {
            return 0;
        }
        ++cursor;
    }
    return 1;
}

static int validate_config(const RenderConfig *config, char *error, size_t error_size) {
    size_t ignored_size;
    int wave_index;

#define CONFIG_ERROR(...)                                      \
    do {                                                       \
        snprintf(error, error_size, __VA_ARGS__);              \
        return 0;                                              \
    } while (0)

    if (config->width < 16 || config->width > MAX_DIMENSION
        || config->height < 16 || config->height > MAX_DIMENSION) {
        CONFIG_ERROR("Width and height must each be between 16 and %d.", MAX_DIMENSION);
    }
    if (config->block_size < 1
        || config->block_size > (config->width > config->height
                                     ? config->width
                                     : config->height)) {
        CONFIG_ERROR("Block size must be between 1 and the larger canvas dimension.");
    }
    if (config->total_frames < 2 || config->total_frames > MAX_TOTAL_FRAMES) {
        CONFIG_ERROR("Frame count must be between 2 and %d.", MAX_TOTAL_FRAMES);
    }
    if (!isfinite(config->fps) || config->fps < 1.0 || config->fps > 240.0) {
        CONFIG_ERROR("FPS must be between 1 and 240.");
    }
    if (config->swing_cycles < 0 || config->swing_cycles > 1000
        || config->spiral_arms < -100 || config->spiral_arms > 100
        || config->hue_cycles < -100 || config->hue_cycles > 100) {
        CONFIG_ERROR("One or more loop-safe integer controls are out of range.");
    }
    if (!isfinite(config->swing_amount) || config->swing_amount < 0.0
        || config->swing_amount > 2.0
        || !isfinite(config->phrase_warp) || config->phrase_warp < 0.0
        || config->phrase_warp > 2.0
        || !isfinite(config->ghost_mix) || config->ghost_mix < 0.0
        || config->ghost_mix > 1.0
        || !isfinite(config->ghost_lag_degrees)
        || config->ghost_lag_degrees < -360.0
        || config->ghost_lag_degrees > 360.0) {
        CONFIG_ERROR("One or more rhythm settings are out of range.");
    }
    if (!isfinite(config->displacement) || config->displacement < 0.0
        || config->displacement > 1000.0
        || !isfinite(config->wave_depth) || config->wave_depth < 0.0
        || config->wave_depth > 10.0
        || !isfinite(config->spiral_frequency) || config->spiral_frequency < 0.0
        || config->spiral_frequency > 1000.0
        || !isfinite(config->wall_frequency) || config->wall_frequency < 0.0
        || config->wall_frequency > 1000.0
        || !isfinite(config->wall_mix) || config->wall_mix < 0.0
        || config->wall_mix > 5.0
        || !isfinite(config->saturation) || config->saturation < 0.0
        || config->saturation > 1.0) {
        CONFIG_ERROR("One or more appearance settings are out of range.");
    }
    for (wave_index = 0; wave_index < WAVE_COUNT; ++wave_index) {
        const WaveConfig *wave = &config->waves[wave_index];

        if (!isfinite(wave->x_percent) || wave->x_percent < -100.0
            || wave->x_percent > 200.0
            || !isfinite(wave->y_percent) || wave->y_percent < -100.0
            || wave->y_percent > 200.0
            || !isfinite(wave->amplitude) || wave->amplitude < 0.0
            || wave->amplitude > 10.0
            || !isfinite(wave->spatial_frequency) || wave->spatial_frequency < 0.0
            || wave->spatial_frequency > 1000.0
            || wave->cycles_per_loop < -1000 || wave->cycles_per_loop > 1000
            || !isfinite(wave->phase_degrees) || wave->phase_degrees < -36000.0
            || wave->phase_degrees > 36000.0) {
            CONFIG_ERROR("Wave %d has a value outside its allowed range.", wave_index + 1);
        }
    }
    if (config->output_directory[0] == '\0') {
        CONFIG_ERROR("Output directory cannot be empty.");
    }
    if (!validate_prefix(config->filename_prefix)) {
        CONFIG_ERROR("Filename prefix cannot be empty or contain a slash/control character.");
    }
    if (config->first_frame_number < 0 || config->first_frame_number > 1000000000
        || config->filename_digits < 1 || config->filename_digits > 12) {
        CONFIG_ERROR("Frame numbering is outside its allowed range.");
    }
    if (!checked_buffer_size(config, &ignored_size)) {
        CONFIG_ERROR("The requested pixel buffer is too large.");
    }

#undef CONFIG_ERROR
    error[0] = '\0';
    return 1;
}

static int directory_exists(const char *path) {
    struct stat status;

    return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

static int create_directory_component(const char *path) {
    if (mkdir(path, 0775) == 0) {
        return 1;
    }
    if (errno == EEXIST && directory_exists(path)) {
        return 1;
    }
    return 0;
}

static int ensure_output_directory(const char *path) {
    char copy[MAX_OUTPUT_PATH];
    size_t length;
    char *cursor;

    if (directory_exists(path)) {
        return 1;
    }
    length = strlen(path);
    if (length == 0 || length >= sizeof(copy)) {
        return 0;
    }
    memcpy(copy, path, length + 1);
    while (length > 1 && copy[length - 1] == '/') {
        copy[--length] = '\0';
    }

    for (cursor = copy + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (copy[0] != '\0' && !create_directory_component(copy)) {
                return 0;
            }
            *cursor = '/';
        }
    }
    return create_directory_component(copy);
}

static int build_output_path(const RenderConfig *config, int frame_index,
                             char *path, size_t path_size) {
    size_t directory_length = strlen(config->output_directory);
    const char *separator = (directory_length > 0
                             && config->output_directory[directory_length - 1] == '/')
                                ? ""
                                : "/";
    int result = snprintf(path, path_size, "%s%s%s%0*d.png",
                          config->output_directory, separator,
                          config->filename_prefix, config->filename_digits,
                          config->first_frame_number + frame_index);

    return result >= 0 && (size_t)result < path_size;
}

static int file_exists(const char *path) {
    struct stat status;

    return stat(path, &status) == 0;
}

static int preflight_output(const RenderConfig *config) {
    char path[MAX_OUTPUT_PATH];
    int frame_index;

    if (!ensure_output_directory(config->output_directory)) {
        fprintf(stderr, "Error: could not create output directory '%s': %s\n",
                config->output_directory, strerror(errno));
        return 0;
    }
    if (config->overwrite_existing) {
        return 1;
    }

    for (frame_index = 0; frame_index < config->total_frames; ++frame_index) {
        if (!build_output_path(config, frame_index, path, sizeof(path))) {
            fprintf(stderr, "Error: an output filename is too long.\n");
            return 0;
        }
        if (file_exists(path)) {
            fprintf(stderr,
                    "Error: '%s' already exists. Change the output settings or enable overwrite.\n",
                    path);
            return 0;
        }
    }
    return 1;
}

static int render_sequence(const RenderConfig *config) {
    char validation_error[256];
    char path[MAX_OUTPUT_PATH];
    size_t buffer_size;
    unsigned char *pixels;
    size_t directory_length;
    const char *separator;
    int frame_index;

    if (!validate_config(config, validation_error, sizeof(validation_error))) {
        fprintf(stderr, "Configuration error: %s\n", validation_error);
        return 0;
    }
    if (!preflight_output(config)) {
        return 0;
    }
    if (!checked_buffer_size(config, &buffer_size)) {
        fprintf(stderr, "Error: pixel buffer size overflow.\n");
        return 0;
    }

    pixels = (unsigned char *)malloc(buffer_size);
    if (pixels == NULL) {
        fprintf(stderr, "Error: could not allocate %.1f MiB for the pixel buffer.\n",
                (double)buffer_size / (1024.0 * 1024.0));
        return 0;
    }

    printf("\nRendering %d frames at %dx%d (%.3f seconds at %.3f fps)...\n",
           config->total_frames, config->width, config->height,
           (double)config->total_frames / config->fps, config->fps);
    directory_length = strlen(config->output_directory);
    separator = (directory_length > 0
                 && config->output_directory[directory_length - 1] == '/') ? "" : "/";
    printf("Output pattern: %s%s%s%%0%dd.png\n",
           config->output_directory, separator,
           config->filename_prefix, config->filename_digits);

    for (frame_index = 0; frame_index < config->total_frames; ++frame_index) {
        render_frame(config, pixels, frame_index);
        if (!build_output_path(config, frame_index, path, sizeof(path))) {
            fprintf(stderr, "\nError: output filename is too long.\n");
            free(pixels);
            return 0;
        }
        if (!stbi_write_png(path, config->width, config->height, CHANNELS, pixels,
                            config->width * CHANNELS)) {
            fprintf(stderr, "\nError: PNG writer failed for '%s'.\n", path);
            free(pixels);
            return 0;
        }
        printf("\rRendered frame %d/%d", frame_index + 1, config->total_frames);
        fflush(stdout);
    }

    free(pixels);
    printf("\nDone. The last frame transitions directly back to frame 0.\n");
    return 1;
}

static void trim_whitespace(char *text) {
    char *start = text;
    char *end;

    while (isspace((unsigned char)*start)) {
        ++start;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
}

static int read_line(const char *prompt, char *buffer, size_t buffer_size) {
    size_t length;
    int character;

    fputs(prompt, stdout);
    fflush(stdout);
    if (fgets(buffer, (int)buffer_size, stdin) == NULL) {
        return 0;
    }
    length = strlen(buffer);
    if (length > 0 && buffer[length - 1] == '\n') {
        buffer[length - 1] = '\0';
    } else {
        while ((character = getchar()) != '\n' && character != EOF) {
        }
    }
    trim_whitespace(buffer);
    return 1;
}

static int parse_long_value(const char *text, long minimum, long maximum, long *value) {
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    while (isspace((unsigned char)*end)) {
        ++end;
    }
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int parse_double_value(const char *text, double minimum, double maximum,
                              double *value) {
    char *end;
    double parsed;

    errno = 0;
    parsed = strtod(text, &end);
    while (isspace((unsigned char)*end)) {
        ++end;
    }
    if (errno != 0 || end == text || *end != '\0' || !isfinite(parsed)
        || parsed < minimum || parsed > maximum) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int prompt_int(const char *label, int *value, int minimum, int maximum) {
    char prompt[256];
    char input[128];

    for (;;) {
        long parsed;

        snprintf(prompt, sizeof(prompt), "%s [%d]: ", label, *value);
        if (!read_line(prompt, input, sizeof(input))) {
            return 0;
        }
        if (input[0] == '\0') {
            return 1;
        }
        if (parse_long_value(input, minimum, maximum, &parsed)) {
            *value = (int)parsed;
            return 1;
        }
        printf("Please enter a whole number from %d to %d, or press Enter to keep %d.\n",
               minimum, maximum, *value);
    }
}

static int prompt_double(const char *label, double *value,
                         double minimum, double maximum) {
    char prompt[256];
    char input[128];

    for (;;) {
        double parsed;

        snprintf(prompt, sizeof(prompt), "%s [%.6g]: ", label, *value);
        if (!read_line(prompt, input, sizeof(input))) {
            return 0;
        }
        if (input[0] == '\0') {
            return 1;
        }
        if (parse_double_value(input, minimum, maximum, &parsed)) {
            *value = parsed;
            return 1;
        }
        printf("Please enter a number from %.6g to %.6g, or press Enter to keep it.\n",
               minimum, maximum);
    }
}

static int prompt_bool(const char *label, int *value) {
    char prompt[256];
    char input[32];

    for (;;) {
        snprintf(prompt, sizeof(prompt), "%s [%s]: ", label, *value ? "yes" : "no");
        if (!read_line(prompt, input, sizeof(input))) {
            return 0;
        }
        if (input[0] == '\0') {
            return 1;
        }
        if (strcmp(input, "y") == 0 || strcmp(input, "Y") == 0
            || strcmp(input, "yes") == 0 || strcmp(input, "YES") == 0) {
            *value = 1;
            return 1;
        }
        if (strcmp(input, "n") == 0 || strcmp(input, "N") == 0
            || strcmp(input, "no") == 0 || strcmp(input, "NO") == 0) {
            *value = 0;
            return 1;
        }
        puts("Please enter yes or no, or press Enter to keep the current value.");
    }
}

static int prompt_text(const char *label, char *value, size_t value_size) {
    char prompt[256];
    char input[MAX_OUTPUT_PATH];

    for (;;) {
        snprintf(prompt, sizeof(prompt), "%s [%s]: ", label, value);
        if (!read_line(prompt, input, sizeof(input))) {
            return 0;
        }
        if (input[0] == '\0') {
            return 1;
        }
        if (strlen(input) < value_size) {
            strcpy(value, input);
            return 1;
        }
        printf("That value is too long (maximum %zu characters).\n", value_size - 1);
    }
}

static void print_summary(const RenderConfig *config) {
    char preview[MAX_OUTPUT_PATH];
    int wave_index;

    if (!build_output_path(config, 0, preview, sizeof(preview))) {
        strcpy(preview, "<path too long>");
    }

    puts("\n============================================================");
    puts(" Seamless Ripple PNG Renderer");
    puts("============================================================");
    printf("Canvas: %dx%d | Block: %d px | Frames: %d | %.3f fps | %.3f sec\n",
           config->width, config->height, config->block_size,
           config->total_frames, config->fps,
           (double)config->total_frames / config->fps);
    printf("Output: %s%s\n", preview,
           config->overwrite_existing ? " (overwrite enabled)" : " (protect existing files)");
    for (wave_index = 0; wave_index < WAVE_COUNT; ++wave_index) {
        const WaveConfig *wave = &config->waves[wave_index];
        printf("Wave %d: %s | pos %.2f%%,%.2f%% | amp %.3f | %.3f rings | %d cycle%s/loop\n",
               wave_index + 1, wave->enabled ? "on " : "off",
               wave->x_percent, wave->y_percent, wave->amplitude,
               wave->spatial_frequency, wave->cycles_per_loop,
               abs(wave->cycles_per_loop) == 1 ? "" : "s");
    }
    puts("\n1) Canvas resolution and block size");
    puts("2) Loop length and frame rate");
    puts("3) Configure/place wave 1");
    puts("4) Configure/place wave 2");
    puts("5) Configure/place wave 3");
    puts("6) 3D surface and pattern appearance");
    puts("7) Rhythm and color");
    puts("8) Output location and filenames");
    puts("9) Restore every default");
    puts("10) Render PNG frames (press Enter)");
    puts("0) Quit");
}

static void configure_canvas(RenderConfig *config) {
    puts("\n-- Canvas --");
    if (!prompt_int("Output width in pixels", &config->width, 16, MAX_DIMENSION)
        || !prompt_int("Output height in pixels", &config->height, 16, MAX_DIMENSION)
        || !prompt_int("Block size in pixels", &config->block_size, 1, MAX_DIMENSION)) {
        return;
    }
    if (config->block_size > config->width && config->block_size > config->height) {
        config->block_size = config->width > config->height
                                 ? config->width
                                 : config->height;
        printf("Block size clamped to %d for this canvas.\n", config->block_size);
    }
}

static void configure_timing(RenderConfig *config) {
    double duration = (double)config->total_frames / config->fps;
    long long frames;

    puts("\n-- Loop timing --");
    puts("PNG files contain no FPS metadata; FPS is used to convert length to a frame count.");
    if (!prompt_double("Playback frame rate", &config->fps, 1.0, 240.0)
        || !prompt_double("Loop length in seconds", &duration, 0.01, 3600.0)) {
        return;
    }
    frames = llround(config->fps * duration);
    if (frames < 2) {
        frames = 2;
    }
    if (frames > MAX_TOTAL_FRAMES) {
        frames = MAX_TOTAL_FRAMES;
    }
    config->total_frames = (int)frames;
    printf("The loop will contain %d frames (actual length %.6f seconds).\n",
           config->total_frames, (double)config->total_frames / config->fps);
}

static void configure_wave(RenderConfig *config, int wave_index) {
    WaveConfig *wave = &config->waves[wave_index];
    double wavelength;

    puts("\n-- Wave source --");
    puts("Positions are percentages, so they keep their placement at any resolution.");
    puts("Values outside 0-100% place a source beyond the canvas edge.");
    puts("Motion cycles are whole numbers by design; this guarantees a seamless loop.");
    if (!prompt_bool("Enabled", &wave->enabled)
        || !prompt_double("Horizontal location (%)", &wave->x_percent, -100.0, 200.0)
        || !prompt_double("Vertical location (%)", &wave->y_percent, -100.0, 200.0)
        || !prompt_double("Amplitude", &wave->amplitude, 0.0, 10.0)
        || !prompt_double("Spatial frequency (rings across the short canvas edge)",
                          &wave->spatial_frequency, 0.0, 1000.0)
        || !prompt_int("Motion cycles per loop (negative reverses direction)",
                       &wave->cycles_per_loop, -1000, 1000)
        || !prompt_double("Starting phase (degrees)", &wave->phase_degrees,
                          -36000.0, 36000.0)) {
        return;
    }

    if (wave->spatial_frequency > 0.0) {
        wavelength = (double)(config->width < config->height
                                  ? config->width
                                  : config->height) / wave->spatial_frequency;
        printf("Approximate ring wavelength at this resolution: %.2f pixels.\n", wavelength);
    }
}

static void configure_surface(RenderConfig *config) {
    puts("\n-- 3D surface and pattern --");
    if (!prompt_double("Glass displacement strength", &config->displacement, 0.0, 1000.0)
        || !prompt_double("3D lighting depth", &config->wave_depth, 0.0, 10.0)
        || !prompt_double("Spiral frequency (rings across short edge)",
                          &config->spiral_frequency, 0.0, 1000.0)
        || !prompt_int("Spiral arms (negative reverses twist)",
                       &config->spiral_arms, -100, 100)
        || !prompt_double("Wall reflection frequency (rings across short edge)",
                          &config->wall_frequency, 0.0, 1000.0)
        || !prompt_double("Wall reflection mix", &config->wall_mix, 0.0, 5.0)) {
        return;
    }
}

static void configure_rhythm(RenderConfig *config) {
    puts("\n-- Rhythm and color --");
    puts("Cycle counts remain integers so all animation components meet at the loop seam.");
    if (!prompt_double("Swing amount", &config->swing_amount, 0.0, 2.0)
        || !prompt_int("Swing pulses per loop", &config->swing_cycles, 0, 1000)
        || !prompt_double("Phrase warp amount", &config->phrase_warp, 0.0, 2.0)
        || !prompt_double("Ghost image mix", &config->ghost_mix, 0.0, 1.0)
        || !prompt_double("Ghost lag (degrees)", &config->ghost_lag_degrees,
                          -360.0, 360.0)
        || !prompt_int("Hue rotations per loop", &config->hue_cycles, -100, 100)
        || !prompt_double("Color saturation", &config->saturation, 0.0, 1.0)) {
        return;
    }
}

static void configure_output(RenderConfig *config) {
    char previous_prefix[MAX_PREFIX_LENGTH];

    puts("\n-- Output --");
    puts("The default directory '.' is the directory where you launch the program.");
    strcpy(previous_prefix, config->filename_prefix);
    if (!prompt_text("Output directory", config->output_directory,
                     sizeof(config->output_directory))
        || !prompt_text("Filename prefix", config->filename_prefix,
                        sizeof(config->filename_prefix))) {
        return;
    }
    if (!validate_prefix(config->filename_prefix)) {
        puts("Prefix cannot be empty or contain a slash/control character; keeping the old prefix.");
        strcpy(config->filename_prefix, previous_prefix);
    }
    if (!prompt_int("First frame number", &config->first_frame_number, 0, 1000000000)
        || !prompt_int("Minimum zero-padding digits", &config->filename_digits, 1, 12)
        || !prompt_bool("Overwrite matching existing PNG files",
                        &config->overwrite_existing)) {
        return;
    }
    if (config->overwrite_existing) {
        puts("Only files matching the selected frame names will be replaced; extra old frames are not removed.");
    }
}

static int interactive_menu(RenderConfig *config) {
    char input[64];

    for (;;) {
        long choice;

        print_summary(config);
        if (!read_line("\nChoice [10]: ", input, sizeof(input))) {
            puts("\nNo input received; exiting without rendering.");
            return 1;
        }
        if (input[0] == '\0') {
            choice = 10;
        } else if (!parse_long_value(input, 0, 10, &choice)) {
            puts("Please choose a menu number from 0 to 10.");
            continue;
        }

        switch (choice) {
            case 0:
                return 1;
            case 1:
                configure_canvas(config);
                break;
            case 2:
                configure_timing(config);
                break;
            case 3:
            case 4:
            case 5:
                configure_wave(config, (int)choice - 3);
                break;
            case 6:
                configure_surface(config);
                break;
            case 7:
                configure_rhythm(config);
                break;
            case 8:
                configure_output(config);
                break;
            case 9:
                *config = default_config();
                puts("All settings restored to defaults.");
                break;
            case 10:
                if (render_sequence(config)) {
                    return 1;
                }
                puts("Render did not complete. Your settings are preserved; adjust them and try again.");
                break;
            default:
                break;
        }
    }
}

static int self_test(void) {
    RenderConfig config = default_config();
    size_t buffer_size;
    unsigned char *start_pixels;
    unsigned char *end_pixels;
    size_t pixel_index;
    int maximum_difference = 0;
    char error[256];

    config.width = 97;
    config.height = 65;
    config.block_size = 16;
    config.total_frames = 12;
    config.fps = 24.0;

    if (!validate_config(&config, error, sizeof(error))
        || !checked_buffer_size(&config, &buffer_size)) {
        fprintf(stderr, "Self-test setup failed: %s\n", error);
        return 0;
    }
    start_pixels = (unsigned char *)malloc(buffer_size);
    end_pixels = (unsigned char *)malloc(buffer_size);
    if (start_pixels == NULL || end_pixels == NULL) {
        free(start_pixels);
        free(end_pixels);
        fputs("Self-test allocation failed.\n", stderr);
        return 0;
    }

    render_frame_at_phase(&config, start_pixels, 0.0);
    render_frame_at_phase(&config, end_pixels, TAU);
    for (pixel_index = 0; pixel_index < buffer_size; ++pixel_index) {
        int difference = abs((int)start_pixels[pixel_index] - (int)end_pixels[pixel_index]);
        if (difference > maximum_difference) {
            maximum_difference = difference;
        }
    }
    free(start_pixels);
    free(end_pixels);

    if (maximum_difference > 1) {
        fprintf(stderr, "Loop periodicity self-test failed (maximum channel delta %d).\n",
                maximum_difference);
        return 0;
    }

    puts("Self-test passed: validation, partial edge blocks, and loop periodicity are sound.");
    return 1;
}

static void print_help(const char *program_name) {
    printf("Usage:\n");
    printf("  %s                 Open the interactive menu\n", program_name);
    printf("  %s --render [options]  Render non-interactively\n", program_name);
    printf("  %s --self-test      Check loop periodicity without writing files\n", program_name);
    puts("\nNon-interactive options:");
    puts("  --render (or --defaults)");
    puts("  --width N --height N --block-size N --frames N --fps N");
    puts("  --output-dir PATH --prefix TEXT --start-frame N --digits N --overwrite");
    puts("  --help");
    puts("\nAll unspecified values use the same defaults shown in the menu.");
}

static int require_argument(int argc, char **argv, int *index, const char **value) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "Option '%s' requires a value.\n", argv[*index]);
        return 0;
    }
    ++(*index);
    *value = argv[*index];
    return 1;
}

static int parse_cli(int argc, char **argv, RenderConfig *config, int *render_now) {
    int index;

    for (index = 1; index < argc; ++index) {
        const char *option = argv[index];
        const char *value = NULL;
        long parsed_long;
        double parsed_double;

        if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) {
            print_help(argv[0]);
            return 2;
        }
        if (strcmp(option, "--self-test") == 0) {
            return self_test() ? 2 : -1;
        }
        if (strcmp(option, "--render") == 0 || strcmp(option, "--defaults") == 0) {
            *render_now = 1;
            continue;
        }
        if (strcmp(option, "--overwrite") == 0) {
            config->overwrite_existing = 1;
            continue;
        }
        if (strcmp(option, "--width") != 0
            && strcmp(option, "--height") != 0
            && strcmp(option, "--block-size") != 0
            && strcmp(option, "--frames") != 0
            && strcmp(option, "--fps") != 0
            && strcmp(option, "--start-frame") != 0
            && strcmp(option, "--digits") != 0
            && strcmp(option, "--output-dir") != 0
            && strcmp(option, "--prefix") != 0) {
            fprintf(stderr, "Unknown option '%s'. Use --help for usage.\n", option);
            return 0;
        }
        if (!require_argument(argc, argv, &index, &value)) {
            return 0;
        }

        if (strcmp(option, "--width") == 0
            && parse_long_value(value, 16, MAX_DIMENSION, &parsed_long)) {
            config->width = (int)parsed_long;
        } else if (strcmp(option, "--height") == 0
                   && parse_long_value(value, 16, MAX_DIMENSION, &parsed_long)) {
            config->height = (int)parsed_long;
        } else if (strcmp(option, "--block-size") == 0
                   && parse_long_value(value, 1, MAX_DIMENSION, &parsed_long)) {
            config->block_size = (int)parsed_long;
        } else if (strcmp(option, "--frames") == 0
                   && parse_long_value(value, 2, MAX_TOTAL_FRAMES, &parsed_long)) {
            config->total_frames = (int)parsed_long;
        } else if (strcmp(option, "--fps") == 0
                   && parse_double_value(value, 1.0, 240.0, &parsed_double)) {
            config->fps = parsed_double;
        } else if (strcmp(option, "--start-frame") == 0
                   && parse_long_value(value, 0, 1000000000, &parsed_long)) {
            config->first_frame_number = (int)parsed_long;
        } else if (strcmp(option, "--digits") == 0
                   && parse_long_value(value, 1, 12, &parsed_long)) {
            config->filename_digits = (int)parsed_long;
        } else if (strcmp(option, "--output-dir") == 0
                   && strlen(value) < sizeof(config->output_directory)) {
            strcpy(config->output_directory, value);
        } else if (strcmp(option, "--prefix") == 0
                   && strlen(value) < sizeof(config->filename_prefix)
                   && validate_prefix(value)) {
            strcpy(config->filename_prefix, value);
        } else {
            fprintf(stderr, "Invalid option or value near '%s'. Use --help for usage.\n", option);
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    RenderConfig config = default_config();
    int render_now = 0;
    int parse_result;

    if (argc == 1) {
        return interactive_menu(&config) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    parse_result = parse_cli(argc, argv, &config, &render_now);
    if (parse_result == 2) {
        return EXIT_SUCCESS;
    }
    if (parse_result < 0 || parse_result == 0) {
        return EXIT_FAILURE;
    }
    if (!render_now) {
        fputs("No action selected. Add --render or use --help.\n", stderr);
        return EXIT_FAILURE;
    }

    return render_sequence(&config) ? EXIT_SUCCESS : EXIT_FAILURE;
}
