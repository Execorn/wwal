// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef WAYWAL_PALETTE_HCT_H
#define WAYWAL_PALETTE_HCT_H

#include "waywal/types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum : uint32_t {
    WAYWAL_THEME_DARK = 0,
    WAYWAL_THEME_LIGHT = 1,
} waywal_theme_mode_t;

typedef enum : uint32_t {
    WAYWAL_SCHEME_TONAL_SPOT = 0,
    WAYWAL_SCHEME_VIBRANT = 1,
    WAYWAL_SCHEME_EXPRESSIVE = 2,
    WAYWAL_SCHEME_CONTENT = 3,
    WAYWAL_SCHEME_FIDELITY = 4,
    WAYWAL_SCHEME_NEUTRAL = 5,
    WAYWAL_SCHEME_MONOCHROME = 6,
    WAYWAL_SCHEME_RAINBOW = 7,
    WAYWAL_SCHEME_FRUIT_SALAD = 8,
} waywal_scheme_variant_t;

typedef struct {
    float l;
    float a;
    float b;
} waywal_oklab_t;

typedef struct {
    float l;
    float c;
    float h_rad;
} waywal_oklch_t;

typedef struct {
    float j;
    float c;
    float h_deg;
} waywal_cam16_t;

typedef struct {
    waywal_theme_mode_t mode;
    waywal_scheme_variant_t variant;
    float contrast;
    bool is_monochrome;

    waywal_oklch_t primary_seed;
    waywal_oklch_t secondary_seed;
    waywal_oklch_t tertiary_seed;
    waywal_oklch_t neutral_seed;
    waywal_oklch_t neutral_variant_seed;

    color_rgba_t primary_paletteKeyColor;
    color_rgba_t secondary_paletteKeyColor;
    color_rgba_t tertiary_paletteKeyColor;
    color_rgba_t neutral_paletteKeyColor;
    color_rgba_t neutral_variant_paletteKeyColor;

    color_rgba_t background;
    color_rgba_t onBackground;
    color_rgba_t surface;
    color_rgba_t surfaceDim;
    color_rgba_t surfaceBright;
    color_rgba_t surfaceContainerLowest;
    color_rgba_t surfaceContainerLow;
    color_rgba_t surfaceContainer;
    color_rgba_t surfaceContainerHigh;
    color_rgba_t surfaceContainerHighest;
    color_rgba_t onSurface;
    color_rgba_t surfaceVariant;
    color_rgba_t onSurfaceVariant;
    color_rgba_t inverseSurface;
    color_rgba_t inverseOnSurface;
    color_rgba_t outline;
    color_rgba_t outlineVariant;
    color_rgba_t shadow;
    color_rgba_t scrim;
    color_rgba_t surfaceTint;

    color_rgba_t primary;
    color_rgba_t onPrimary;
    color_rgba_t primaryContainer;
    color_rgba_t onPrimaryContainer;
    color_rgba_t inversePrimary;

    color_rgba_t secondary;
    color_rgba_t onSecondary;
    color_rgba_t secondaryContainer;
    color_rgba_t onSecondaryContainer;

    color_rgba_t tertiary;
    color_rgba_t onTertiary;
    color_rgba_t tertiaryContainer;
    color_rgba_t onTertiaryContainer;

    color_rgba_t error;
    color_rgba_t onError;
    color_rgba_t errorContainer;
    color_rgba_t onErrorContainer;

    color_rgba_t primaryFixed;
    color_rgba_t primaryFixedDim;
    color_rgba_t onPrimaryFixed;
    color_rgba_t onPrimaryFixedVariant;

    color_rgba_t secondaryFixed;
    color_rgba_t secondaryFixedDim;
    color_rgba_t onSecondaryFixed;
    color_rgba_t onSecondaryFixedVariant;

    color_rgba_t tertiaryFixed;
    color_rgba_t tertiaryFixedDim;
    color_rgba_t onTertiaryFixed;
    color_rgba_t onTertiaryFixedVariant;

    union {
        color_rgba_t term[16];
        struct {
            color_rgba_t term0;
            color_rgba_t term1;
            color_rgba_t term2;
            color_rgba_t term3;
            color_rgba_t term4;
            color_rgba_t term5;
            color_rgba_t term6;
            color_rgba_t term7;
            color_rgba_t term8;
            color_rgba_t term9;
            color_rgba_t term10;
            color_rgba_t term11;
            color_rgba_t term12;
            color_rgba_t term13;
            color_rgba_t term14;
            color_rgba_t term15;
        };
    };

    color_rgba_t rosewater;
    color_rgba_t flamingo;
    color_rgba_t pink;
    color_rgba_t mauve;
    color_rgba_t red;
    color_rgba_t maroon;
    color_rgba_t peach;
    color_rgba_t yellow;
    color_rgba_t green;
    color_rgba_t teal;
    color_rgba_t sky;
    color_rgba_t sapphire;
    color_rgba_t blue;
    color_rgba_t lavender;

    color_rgba_t klink;
    color_rgba_t klinkSelection;
    color_rgba_t kvisited;
    color_rgba_t kvisitedSelection;
    color_rgba_t knegative;
    color_rgba_t knegativeSelection;
    color_rgba_t kneutral;
    color_rgba_t kneutralSelection;
    color_rgba_t kpositive;
    color_rgba_t kpositiveSelection;

    color_rgba_t text;
    color_rgba_t subtext1;
    color_rgba_t subtext0;
    color_rgba_t overlay2;
    color_rgba_t overlay1;
    color_rgba_t overlay0;
    color_rgba_t surface2;
    color_rgba_t surface1;
    color_rgba_t surface0;
    color_rgba_t base;
    color_rgba_t mantle;
    color_rgba_t crust;

    color_rgba_t success;
    color_rgba_t onSuccess;
    color_rgba_t successContainer;
    color_rgba_t onSuccessContainer;

    uint32_t source_color;
    uint32_t seeds[4];
    uint8_t seed_count;
    uint8_t source_color_index;
    bool is_light_mode;
    char wallpaper_path[512];
} waywal_palette_t;

typedef waywal_scheme_variant_t waywal_scheme_type_t;

struct waywal_config;

waywal_scheme_variant_t waywal_scheme_variant_from_str(const char *str);
color_rgba_t waywal_hct_to_rgba(float h_deg, float c_cam16, float t_lstar);
void waywal_rgba_to_hct(color_rgba_t rgba, float *out_h_deg, float *out_c_cam16,
                        float *out_t_lstar);
color_rgba_t waywal_palette_generate_tone(float l_target, float chroma, float hue_rad);

bool waywal_palette_extract(const uint8_t *rgba_pixels, uint32_t width, uint32_t height,
                            uint32_t stride_bytes, waywal_theme_mode_t mode,
                            waywal_palette_t *out_palette);

bool waywal_palette_extract_ex(const uint8_t *pixels, uint32_t width, uint32_t height,
                               uint32_t stride_bytes, bool is_bgra, waywal_theme_mode_t mode,
                               waywal_scheme_variant_t variant, float contrast,
                               waywal_palette_t *out_palette);

bool waywal_palette_extract_seed_ex(const uint8_t *pixels, uint32_t width, uint32_t height,
                                    uint32_t stride_bytes, bool is_bgra, waywal_theme_mode_t mode,
                                    waywal_scheme_variant_t variant, float contrast,
                                    uint8_t source_color_index, bool has_seed_override,
                                    uint32_t seed_override_rgb, waywal_palette_t *out_palette);

bool waywal_palette_extract_dual(const uint8_t *rgba_pixels, uint32_t width, uint32_t height,
                                 uint32_t stride_bytes, waywal_palette_t *out_dark,
                                 waywal_palette_t *out_light);

bool waywal_palette_extract_dual_ex(const uint8_t *pixels, uint32_t width, uint32_t height,
                                    uint32_t stride_bytes, bool is_bgra,
                                    waywal_scheme_variant_t variant, float contrast,
                                    uint8_t source_color_index, bool has_seed_override,
                                    uint32_t seed_override_rgb, waywal_palette_t *out_dark,
                                    waywal_palette_t *out_light);

bool waywal_palette_extract_bgra(const uint8_t *bgra_pixels, uint32_t width, uint32_t height,
                                 uint32_t stride_bytes, waywal_theme_mode_t mode,
                                 waywal_palette_t *out_palette);

bool waywal_palette_extract_dual_bgra(const uint8_t *bgra_pixels, uint32_t width, uint32_t height,
                                      uint32_t stride_bytes, waywal_palette_t *out_dark,
                                      waywal_palette_t *out_light);

void waywal_palette_derive_opposite(const waywal_palette_t *src, waywal_palette_t *out_opp);

size_t waywal_palette_format_sequences(const waywal_palette_t *pal, char *out_buf, size_t buf_cap);

bool waywal_palette_export_all(const waywal_palette_t *pal, const char *override_base_dir);

bool waywal_palette_export_filtered(const waywal_palette_t *pal, const char *override_base_dir,
                                    bool caelestia_sync, bool export_css, bool export_hyprland);

bool waywal_palette_export_pywal(const waywal_palette_t *pal, const char *override_base_dir);

bool waywal_palette_render_templates(const waywal_palette_t *pal, const char *templates_dir,
                                     const char *out_dir);

bool waywal_palette_render_template_file(const waywal_palette_t *default_pal,
                                         const waywal_palette_t *dark_pal,
                                         const waywal_palette_t *light_pal, const char *input_path,
                                         const char *output_path);

size_t waywal_palette_apply_configured_templates(const waywal_palette_t *default_pal,
                                                 const waywal_palette_t *dark_pal,
                                                 const waywal_palette_t *light_pal,
                                                 const struct waywal_config *cfg);

bool waywal_palette_run_hook(const char *cmd);

bool waywal_palette_broadcast_tty(const waywal_palette_t *pal);

#ifdef __cplusplus
}
#endif

#endif
