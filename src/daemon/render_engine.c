#include "waywal/render_engine.h"

#include "gpu_compute.h"
#include "simd/simd_blend.h"
#include "waywal/log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

bool render_engine_init(render_engine_t *re, dmabuf_context_t *dmabuf_ctx)
{
    if (!re)
        return false;
    memset(re, 0, sizeof(*re));

    if (dmabuf_ctx && dmabuf_ctx->available) {
        re->gpu_ctx = gpu_compute_create(dmabuf_ctx);
        if (re->gpu_ctx) {
            re->has_gpu_compute = true;
            WAYWAL_LOG_INFO("Render Engine initialized with hardware GPU compute shader pipeline");
            return true;
        }
    }

    simd_blend_scanline_fn fn = simd_get_blend_fn();
    const char *simd_name = "Scalar";
#if defined(__x86_64__) || defined(_M_X64)
    if (fn == blend_scanline_avx512)
        simd_name = "AVX-512 VBMI2";
    else if (fn == blend_scanline_avx2)
        simd_name = "AVX2";
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (fn == blend_scanline_neon)
        simd_name = "ARM NEON";
#endif

    WAYWAL_LOG_INFO("Render Engine initialized with CPU SIMD fallback (%s)", simd_name);
    return true;
}

void render_engine_destroy(render_engine_t *re)
{
    if (!re)
        return;
    if (re->has_gpu_compute && re->gpu_ctx) {
        gpu_compute_destroy((gpu_compute_ctx_t *)re->gpu_ctx);
        re->gpu_ctx = NULL;
        re->has_gpu_compute = false;
    }
}

bool render_engine_load_custom_shader(render_engine_t *re, const char *shader_src)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx || !shader_src)
        return false;
    return gpu_compute_load_custom_shader((gpu_compute_ctx_t *)re->gpu_ctx, shader_src);
}

void render_engine_release_bo(render_engine_t *re, dmabuf_bo_t *bo)
{
    if (!re || !re->has_gpu_compute || !re->gpu_ctx || !bo)
        return;
    gpu_compute_release_bo((gpu_compute_ctx_t *)re->gpu_ctx, bo);
}

static inline float clampf(float v, float min_v, float max_v)
{
    if (v < min_v)
        return min_v;
    if (v > max_v)
        return max_v;
    return v;
}

static inline float smoothstepf(float edge0, float edge1, float x)
{
    float t = clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline float hash21(float x, float y)
{
    float px = fmodf(x * 123.34f + y * 456.21f, 1.0f);
    if (px < 0.0f)
        px += 1.0f;
    float dot = px * (px + 45.32f);
    float val = fmodf(dot * 1337.5f, 1.0f);
    return val < 0.0f ? val + 1.0f : val;
}

static inline uint32_t blend_pixel_scalar(uint32_t a, uint32_t b, uint16_t wb)
{
    uint32_t wa = 256 - wb;
    uint32_t rb = (((a & 0x00FF00FF) * wa) + ((b & 0x00FF00FF) * wb)) >> 8;
    uint32_t g = (((a & 0x0000FF00) * wa) + ((b & 0x0000FF00) * wb)) >> 8;
    return (rb & 0x00FF00FF) | (g & 0x0000FF00) | 0xFF000000;
}

static inline uint32_t add_saturate_rgb(uint32_t c, int flash)
{
    int r = (int)((c >> 16) & 0xFF) + flash;
    int g = (int)((c >> 8) & 0xFF) + flash;
    int b = (int)(c & 0xFF) + flash;
    if (r > 255)
        r = 255;
    else if (r < 0)
        r = 0;
    if (g > 255)
        g = 255;
    else if (g < 0)
        g = 0;
    if (b > 255)
        b = 255;
    else if (b < 0)
        b = 0;
    return (c & 0xFF000000) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline float value_noise2(float x, float y)
{
    float ix = floorf(x);
    float iy = floorf(y);
    float fx = x - ix;
    float fy = y - iy;
    fx = fx * fx * (3.0f - 2.0f * fx);
    fy = fy * fy * (3.0f - 2.0f * fy);
    float a = hash21(ix, iy);
    float b = hash21(ix + 1.0f, iy);
    float c = hash21(ix, iy + 1.0f);
    float d = hash21(ix + 1.0f, iy + 1.0f);
    return (a * (1.0f - fx) + b * fx) * (1.0f - fy) + (c * (1.0f - fx) + d * fx) * fy;
}

static inline float fbm2(float x, float y)
{
    return 0.65f * value_noise2(x, y) + 0.35f * value_noise2(x * 2.1f + 1.7f, y * 2.1f + 3.2f);
}

bool render_engine_execute_cpu_transition(uint32_t *dst, const uint32_t *old_pixels,
                                          const uint32_t *new_pixels, uint32_t width,
                                          uint32_t height, uint32_t stride_bytes,
                                          const waywal_transition_params_t *params)
{
    if (!dst || !old_pixels || !new_pixels || width == 0 || height == 0 || !params) {
        return false;
    }

    simd_blend_scanline_fn blend_fn = simd_get_blend_fn();
    uint32_t stride_pixels = stride_bytes / sizeof(uint32_t);
    float progress = clampf(params->progress, 0.0f, 1.0f);

    switch (params->type) {
    case WAYWAL_TRANSITION_FADE:
    case WAYWAL_TRANSITION_SIMPLE: {
        uint16_t weight_b = (uint16_t)(progress * 256.0f);
        if (weight_b > 256)
            weight_b = 256;

        for (uint32_t y = 0; y < height; ++y) {
            uint8_t *dst_row = (uint8_t *)(dst + y * stride_pixels);
            const uint8_t *src_a_row = (const uint8_t *)(old_pixels + y * width);
            const uint8_t *src_b_row = (const uint8_t *)(new_pixels + y * width);

            blend_fn(dst_row, src_a_row, src_b_row, weight_b, width * sizeof(uint32_t));
        }
        return true;
    }

    case WAYWAL_TRANSITION_WIPE: {
        float cos_a = cosf(params->angle_rad);
        float sin_a = sinf(params->angle_rad);
        float edge = 0.04f;

        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y / (float)height - 0.5f;
            uint32_t *dst_row = dst + y * stride_pixels;
            const uint32_t *a_row = old_pixels + y * width;
            const uint32_t *b_row = new_pixels + y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float nx = (float)x / (float)width - 0.5f;
                float proj = (nx * cos_a + ny * sin_a) / 1.41421356f + 0.5f;
                float alpha = smoothstepf(proj - edge, proj + edge, progress);
                if (alpha <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (alpha >= 1.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_GROW: {
        float cx = params->center_x > 0.0f ? params->center_x : 0.5f;
        float cy = params->center_y > 0.0f ? params->center_y : 0.5f;
        float aspect = (float)width / (float)height;
        float max_x = cx > (1.0f - cx) ? cx : (1.0f - cx);
        float max_y = cy > (1.0f - cy) ? cy : (1.0f - cy);
        float max_dist = sqrtf((max_x * aspect) * (max_x * aspect) + max_y * max_y) + 0.05f;
        float r = progress * max_dist;
        float edge = 0.03f * max_dist;

        for (uint32_t y = 0; y < height; ++y) {
            float dy = (float)y / (float)height - cy;
            uint32_t *dst_row = dst + y * stride_pixels;
            const uint32_t *a_row = old_pixels + y * width;
            const uint32_t *b_row = new_pixels + y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float dx = ((float)x / (float)width - cx) * aspect;
                float dist = sqrtf(dx * dx + dy * dy);
                float alpha = smoothstepf(dist - edge, dist + edge, r);
                if (alpha <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (alpha >= 1.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_OUTER: {
        float cx = params->center_x > 0.0f ? params->center_x : 0.5f;
        float cy = params->center_y > 0.0f ? params->center_y : 0.5f;
        float aspect = (float)width / (float)height;
        float max_x = cx > (1.0f - cx) ? cx : (1.0f - cx);
        float max_y = cy > (1.0f - cy) ? cy : (1.0f - cy);
        float max_dist = sqrtf((max_x * aspect) * (max_x * aspect) + max_y * max_y) + 0.05f;
        float r = (1.0f - progress) * max_dist;
        float edge = 0.03f * max_dist;

        for (uint32_t y = 0; y < height; ++y) {
            float dy = (float)y / (float)height - cy;
            uint32_t *dst_row = dst + y * stride_pixels;
            const uint32_t *a_row = old_pixels + y * width;
            const uint32_t *b_row = new_pixels + y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float dx = ((float)x / (float)width - cx) * aspect;
                float dist = sqrtf(dx * dx + dy * dy);
                float alpha = smoothstepf(r - edge, r + edge, dist);
                if (alpha <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (alpha >= 1.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_WAVE: {
        float cos_a = cosf(params->angle_rad);
        float sin_a = sinf(params->angle_rad);
        float freq = params->wave_freq > 0.0f ? params->wave_freq : 20.0f;
        float amp = params->wave_amp > 0.0f ? params->wave_amp : 0.05f;
        float edge = 0.04f;

        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y / (float)height - 0.5f;
            uint32_t *dst_row = dst + y * stride_pixels;
            const uint32_t *a_row = old_pixels + y * width;
            const uint32_t *b_row = new_pixels + y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float nx = (float)x / (float)width - 0.5f;
                float proj = (nx * cos_a + ny * sin_a) / 1.41421356f + 0.5f;
                float cross = -nx * sin_a + ny * cos_a;
                float wave = sinf(cross * freq) * amp;
                float d = proj + wave;
                float alpha = smoothstepf(d - edge, d + edge, progress);
                if (alpha <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (alpha >= 1.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_NOISE: {
        float edge = 0.05f;
        for (uint32_t y = 0; y < height; ++y) {
            uint32_t *dst_row = dst + y * stride_pixels;
            const uint32_t *a_row = old_pixels + y * width;
            const uint32_t *b_row = new_pixels + y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float n = hash21((float)x * 0.05f, (float)y * 0.05f);
                float alpha = smoothstepf(n - edge, n + edge, progress);
                if (alpha <= 0.0f) {
                    dst_row[x] = a_row[x];
                } else if (alpha >= 1.0f) {
                    dst_row[x] = b_row[x];
                } else {
                    uint16_t wb = (uint16_t)(alpha * 256.0f);
                    dst_row[x] = blend_pixel_scalar(a_row[x], b_row[x], wb);
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_CROSSZOOM: {
        float cx = (params->center_x > 0.0f || params->center_y > 0.0f) ? params->center_x : 0.5f;
        float cy = (params->center_x > 0.0f || params->center_y > 0.0f) ? params->center_y : 0.5f;
        float strength = params->wave_amp > 0.001f ? params->wave_amp : 0.4f;
        float peak = sinf(progress * 3.14159265f);
        int flash = (int)(peak * 60.0f);
        float zoom_a = 1.0f - progress * strength;
        float zoom_b = 1.0f + (1.0f - progress) * strength;
        uint16_t wb = (uint16_t)(progress * 256.0f);

        for (uint32_t y = 0; y < height; ++y) {
            float ny = ((float)y / (float)height - cy);
            uint32_t *dst_row = dst + y * stride_pixels;

            for (uint32_t x = 0; x < width; ++x) {
                float nx = ((float)x / (float)width - cx);
                int src_ax = (int)((cx + nx * zoom_a) * width);
                int src_ay = (int)((cy + ny * zoom_a) * height);
                if (src_ax < 0)
                    src_ax = 0;
                else if (src_ax >= (int)width)
                    src_ax = width - 1;
                if (src_ay < 0)
                    src_ay = 0;
                else if (src_ay >= (int)height)
                    src_ay = height - 1;

                int src_bx = (int)((cx + nx * zoom_b) * width);
                int src_by = (int)((cy + ny * zoom_b) * height);
                if (src_bx < 0)
                    src_bx = 0;
                else if (src_bx >= (int)width)
                    src_bx = width - 1;
                if (src_by < 0)
                    src_by = 0;
                else if (src_by >= (int)height)
                    src_by = height - 1;

                uint32_t ca = old_pixels[src_ay * width + src_ax];
                uint32_t cb = new_pixels[src_by * width + src_bx];
                uint32_t blended = blend_pixel_scalar(ca, cb, wb);
                dst_row[x] = flash > 0 ? add_saturate_rgb(blended, flash) : blended;
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_SLIDE: {
        float cos_a = cosf(params->angle_rad);
        float sin_a = sinf(params->angle_rad);
        float edge = 0.04f;

        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y / (float)height - 0.5f;
            uint32_t *dst_row = dst + y * stride_pixels;

            for (uint32_t x = 0; x < width; ++x) {
                float nx = (float)x / (float)width - 0.5f;
                float proj = (nx * cos_a + ny * sin_a) / 1.41421356f + 0.5f;
                float split = smoothstepf(proj - edge, proj + edge, progress);

                int ax = (int)(x + cos_a * progress * width);
                int ay = (int)(y + sin_a * progress * height);
                if (ax < 0)
                    ax = 0;
                else if (ax >= (int)width)
                    ax = width - 1;
                if (ay < 0)
                    ay = 0;
                else if (ay >= (int)height)
                    ay = height - 1;

                int bx = (int)(x - cos_a * (1.0f - progress) * width);
                int by = (int)(y - sin_a * (1.0f - progress) * height);
                if (bx < 0)
                    bx = 0;
                else if (bx >= (int)width)
                    bx = width - 1;
                if (by < 0)
                    by = 0;
                else if (by >= (int)height)
                    by = height - 1;

                uint32_t ca = old_pixels[ay * width + ax];
                uint32_t cb = new_pixels[by * width + bx];
                if (split <= 0.0f) {
                    dst_row[x] = ca;
                } else if (split >= 1.0f) {
                    dst_row[x] = cb;
                } else {
                    uint16_t wb = (uint16_t)(split * 256.0f);
                    dst_row[x] = blend_pixel_scalar(ca, cb, wb);
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_GLITCH: {
        float bands = params->wave_freq > 0.0f ? params->wave_freq : 35.0f;
        float intensity = params->wave_amp > 0.0f ? params->wave_amp : 0.06f;
        float peak = sinf(progress * 3.14159265f);
        float step_t = floorf(progress * 24.0f);

        for (uint32_t y = 0; y < height; ++y) {
            float slice_idx = floorf(((float)y / (float)height) * bands);
            float h = hash21(slice_idx, step_t);
            int jitter = 0;
            if (h > 0.60f) {
                jitter = (int)((h - 0.80f) * 2.5f * intensity * peak * (float)width);
            }
            int split = (int)(peak * intensity * 0.5f * (float)width);

            uint32_t *dst_row = dst + y * stride_pixels;
            for (uint32_t x = 0; x < width; ++x) {
                int rx = (int)x + jitter + split;
                int gx = (int)x + jitter;
                int bx = (int)x + jitter - split;
                if (rx < 0)
                    rx = 0;
                else if (rx >= (int)width)
                    rx = width - 1;
                if (gx < 0)
                    gx = 0;
                else if (gx >= (int)width)
                    gx = width - 1;
                if (bx < 0)
                    bx = 0;
                else if (bx >= (int)width)
                    bx = width - 1;

                uint32_t a_r = old_pixels[y * width + rx] & 0x00FF0000;
                uint32_t a_g = old_pixels[y * width + gx] & 0x0000FF00;
                uint32_t a_b = old_pixels[y * width + bx] & 0x000000FF;
                uint32_t ca = 0xFF000000 | a_r | a_g | a_b;

                uint32_t b_r = new_pixels[y * width + rx] & 0x00FF0000;
                uint32_t b_g = new_pixels[y * width + gx] & 0x0000FF00;
                uint32_t b_b = new_pixels[y * width + bx] & 0x000000FF;
                uint32_t cb = 0xFF000000 | b_r | b_g | b_b;

                float block_h = hash21(floorf((float)x / 24.0f), floorf((float)y / 14.0f) + step_t);
                float gprog = progress + (block_h - 0.5f) * peak * 0.35f;
                float m = smoothstepf(0.46f, 0.54f, gprog);
                uint16_t wb = (uint16_t)(m * 256.0f);
                dst_row[x] = blend_pixel_scalar(ca, cb, wb);
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_BURN: {
        float scale = params->wave_freq > 0.0f ? params->wave_freq : 14.0f;
        float flame_width = params->wave_amp > 0.0f ? params->wave_amp : 0.08f;

        for (uint32_t y = 0; y < height; ++y) {
            float ny = (float)y / (float)height;
            uint32_t *dst_row = dst + y * stride_pixels;
            const uint32_t *a_row = old_pixels + y * width;
            const uint32_t *b_row = new_pixels + y * width;

            for (uint32_t x = 0; x < width; ++x) {
                float nx = (float)x / (float)width;
                float n = fbm2(nx * scale, ny * scale);
                float d = n - progress;

                if (d > flame_width) {
                    dst_row[x] = a_row[x];
                } else if (d < -flame_width) {
                    dst_row[x] = b_row[x];
                } else {
                    float burn_factor = smoothstepf(flame_width, -flame_width, d);
                    uint16_t wb = (uint16_t)(burn_factor * 256.0f);
                    uint32_t base = blend_pixel_scalar(a_row[x], b_row[x], wb);

                    float dist_norm = 1.0f - fabsf(d) / flame_width;
                    float heat = dist_norm * dist_norm;
                    int fr = (int)(heat * 240.0f);
                    int fg = (int)(heat * 120.0f);
                    int fb = (int)(heat * 20.0f);

                    int r = (int)((base >> 16) & 0xFF) + fr;
                    int g = (int)((base >> 8) & 0xFF) + fg;
                    int b = (int)(base & 0xFF) + fb;
                    if (r > 255)
                        r = 255;
                    if (g > 255)
                        g = 255;
                    if (b > 255)
                        b = 255;
                    dst_row[x] =
                        0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_RIPPLE: {
        float cx = (params->center_x > 0.0f || params->center_y > 0.0f) ? params->center_x : 0.5f;
        float cy = (params->center_y > 0.0f || params->center_y > 0.0f) ? params->center_y : 0.5f;
        float aspect = (float)width / (float)height;
        float max_x = cx > (1.0f - cx) ? cx : (1.0f - cx);
        float max_y = cy > (1.0f - cy) ? cy : (1.0f - cy);
        float max_dist = sqrtf((max_x * aspect) * (max_x * aspect) + max_y * max_y) + 0.1f;
        float current_radius = progress * max_dist;
        float wave_width = 0.18f;
        float freq = params->wave_freq > 0.0f ? params->wave_freq : 32.0f;
        float amp = params->wave_amp > 0.0f ? params->wave_amp : 0.04f;
        float decay = (1.0f - progress) * (1.0f - progress);

        for (uint32_t y = 0; y < height; ++y) {
            float dy = (float)y / (float)height - cy;
            uint32_t *dst_row = dst + y * stride_pixels;

            for (uint32_t x = 0; x < width; ++x) {
                float dx = ((float)x / (float)width - cx) * aspect;
                float dist = sqrtf(dx * dx + dy * dy);
                float diff = dist - current_radius;

                float wave = 0.0f;
                if (fabsf(diff) < wave_width) {
                    float window = smoothstepf(wave_width, 0.0f, fabsf(diff));
                    wave = sinf(diff * freq) * amp * decay * window;
                }

                float ndx = (dist > 0.0001f) ? (dx / dist / aspect) : 0.0f;
                float ndy = (dist > 0.0001f) ? (dy / dist) : 0.0f;

                int sx = (int)(((float)x / (float)width + ndx * wave) * width);
                int sy = (int)(((float)y / (float)height + ndy * wave) * height);
                if (sx < 0)
                    sx = 0;
                else if (sx >= (int)width)
                    sx = width - 1;
                if (sy < 0)
                    sy = 0;
                else if (sy >= (int)height)
                    sy = height - 1;

                uint32_t ca = old_pixels[sy * width + sx];
                uint32_t cb = new_pixels[sy * width + sx];

                float split = smoothstepf(current_radius - 0.02f, current_radius + 0.02f, dist);
                uint16_t wa = (uint16_t)(split * 256.0f);
                dst_row[x] = blend_pixel_scalar(cb, ca, wa);
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_PIXELATE: {
        float d = sinf(progress * 3.1415926535f) * 64.0f + 1.0f;
        uint32_t block_sz = (d >= 1.0f) ? (uint32_t)d : 1u;
        uint16_t wb = (uint16_t)(progress * 256.0f);

        for (uint32_t by = 0; by < height; by += block_sz) {
            uint32_t bh = (by + block_sz <= height) ? block_sz : (height - by);
            uint32_t sy = by + bh / 2;
            if (sy >= height)
                sy = height - 1;

            for (uint32_t bx = 0; bx < width; bx += block_sz) {
                uint32_t bw = (bx + block_sz <= width) ? block_sz : (width - bx);
                uint32_t sx = bx + bw / 2;
                if (sx >= width)
                    sx = width - 1;

                uint32_t ca = old_pixels[sy * width + sx];
                uint32_t cb = new_pixels[sy * width + sx];
                uint32_t blended = blend_pixel_scalar(ca, cb, wb);

                for (uint32_t y = by; y < by + bh; ++y) {
                    uint32_t *dst_row = dst + y * stride_pixels;
                    for (uint32_t x = bx; x < bx + bw; ++x) {
                        dst_row[x] = blended;
                    }
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_DOOM: {
        uint32_t col_width = 4;
        for (uint32_t bx = 0; bx < width; bx += col_width) {
            uint32_t bw = (bx + col_width <= width) ? col_width : (width - bx);
            float col_idx = (float)(bx / col_width);
            float delay = fmodf(col_idx * 123.34f + 45.32f, 1.0f);
            if (delay < 0.0f)
                delay += 1.0f;
            delay = fmodf(delay * (delay + 33.33f) * 1337.5f, 1.0f);
            if (delay < 0.0f)
                delay += 1.0f;
            delay *= 0.35f;

            float shift = clampf((progress * 1.35f - delay) / 1.0f, 0.0f, 1.0f);
            int pixel_shift = (int)(shift * height);

            for (uint32_t y = 0; y < height; ++y) {
                uint32_t *dst_row = dst + y * stride_pixels;
                if ((int)y < pixel_shift) {
                    for (uint32_t x = bx; x < bx + bw; ++x) {
                        dst_row[x] = new_pixels[y * width + x];
                    }
                } else {
                    int sy = (int)y - pixel_shift;
                    if (sy < 0)
                        sy = 0;
                    if (sy >= (int)height)
                        sy = height - 1;
                    for (uint32_t x = bx; x < bx + bw; ++x) {
                        dst_row[x] = old_pixels[sy * width + x];
                    }
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_SWIRL: {
        float cx = (params->center_x > 0.0f || params->center_y > 0.0f) ? params->center_x : 0.5f;
        float cy = (params->center_y > 0.0f || params->center_y > 0.0f) ? params->center_y : 0.5f;
        float aspect = (float)width / (float)height;
        float angle_amt = sinf(progress * 3.1415926535f) * 8.0f;
        float max_r = 0.85f;
        uint16_t wb = (uint16_t)(progress * 256.0f);

        for (uint32_t y = 0; y < height; ++y) {
            float dy = (float)y / (float)height - cy;
            uint32_t *dst_row = dst + y * stride_pixels;
            for (uint32_t x = 0; x < width; ++x) {
                float dx = ((float)x / (float)width - cx) * aspect;
                float r = sqrtf(dx * dx + dy * dy);
                float uv_x = (float)x / (float)width;
                float uv_y = (float)y / (float)height;

                if (r < max_r) {
                    float factor = (1.0f - r / max_r);
                    float theta = factor * factor * angle_amt;
                    float cos_t = cosf(theta);
                    float sin_t = sinf(theta);
                    float rx = dx * cos_t - dy * sin_t;
                    float ry = dx * sin_t + dy * cos_t;
                    uv_x = cx + rx / aspect;
                    uv_y = cy + ry;
                }

                int sx = (int)(uv_x * width);
                int sy = (int)(uv_y * height);
                if (sx < 0)
                    sx = 0;
                else if (sx >= (int)width)
                    sx = width - 1;
                if (sy < 0)
                    sy = 0;
                else if (sy >= (int)height)
                    sy = height - 1;

                uint32_t ca = old_pixels[sy * width + sx];
                uint32_t cb = new_pixels[sy * width + sx];
                dst_row[x] = blend_pixel_scalar(ca, cb, wb);
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_CUBE: {
        float theta = progress * 1.57079632679f;
        float zoom = 1.0f + 0.35f * sinf(progress * 3.1415926535f);
        float cos_t = cosf(theta);
        float sin_t = sinf(theta);
        float d_cam = 2.5f;

        for (uint32_t y = 0; y < height; ++y) {
            float py = ((float)y / (float)height - 0.5f) * 2.0f;
            uint32_t *dst_row = dst + y * stride_pixels;
            for (uint32_t x = 0; x < width; ++x) {
                float px = ((float)x / (float)width - 0.5f) * 2.0f;
                float rdx = px * zoom;
                float rdy = py * zoom;
                float rdz = d_cam;
                float inv_len = 1.0f / sqrtf(rdx * rdx + rdy * rdy + rdz * rdz);
                rdx *= inv_len;
                rdy *= inv_len;
                rdz *= inv_len;

                float ro_rel_x = 0.0f;
                float ro_rel_y = 0.0f;
                float ro_rel_z = -d_cam - 1.0f;

                float ro_cube_x = ro_rel_x * cos_t - ro_rel_z * sin_t;
                float ro_cube_y = ro_rel_y;
                float ro_cube_z = ro_rel_x * sin_t + ro_rel_z * cos_t;

                float rd_cube_x = rdx * cos_t - rdz * sin_t;
                float rd_cube_y = rdy;
                float rd_cube_z = rdx * sin_t + rdz * cos_t;

                float t_front = -1.0f;
                if (fabsf(rd_cube_z) > 1e-5f) {
                    float t = (-1.0f - ro_cube_z) / rd_cube_z;
                    if (t > 0.0f) {
                        float hx = ro_cube_x + t * rd_cube_x;
                        float hy = ro_cube_y + t * rd_cube_y;
                        if (fabsf(hx) <= 1.0f && fabsf(hy) <= 1.0f)
                            t_front = t;
                    }
                }

                float t_right = -1.0f;
                if (fabsf(rd_cube_x) > 1e-5f) {
                    float t = (1.0f - ro_cube_x) / rd_cube_x;
                    if (t > 0.0f) {
                        float hz = ro_cube_z + t * rd_cube_z;
                        float hy = ro_cube_y + t * rd_cube_y;
                        if (fabsf(hz) <= 1.0f && fabsf(hy) <= 1.0f)
                            t_right = t;
                    }
                }

                if (t_front > 0.0f && (t_right < 0.0f || t_front < t_right)) {
                    float hx = ro_cube_x + t_front * rd_cube_x;
                    float hy = ro_cube_y + t_front * rd_cube_y;
                    int sx = (int)((hx * 0.5f + 0.5f) * width);
                    int sy = (int)((hy * 0.5f + 0.5f) * height);
                    if (sx < 0)
                        sx = 0;
                    else if (sx >= (int)width)
                        sx = width - 1;
                    if (sy < 0)
                        sy = 0;
                    else if (sy >= (int)height)
                        sy = height - 1;
                    uint32_t c = old_pixels[sy * width + sx];
                    float shade = 0.65f + 0.35f * cos_t;
                    dst_row[x] = blend_pixel_scalar(0xFF000000, c, (uint16_t)(shade * 256.0f));
                } else if (t_right > 0.0f) {
                    float hz = ro_cube_z + t_right * rd_cube_z;
                    float hy = ro_cube_y + t_right * rd_cube_y;
                    int sx = (int)(((hz + 1.0f) * 0.5f) * width);
                    int sy = (int)((hy * 0.5f + 0.5f) * height);
                    if (sx < 0)
                        sx = 0;
                    else if (sx >= (int)width)
                        sx = width - 1;
                    if (sy < 0)
                        sy = 0;
                    else if (sy >= (int)height)
                        sy = height - 1;
                    uint32_t c = new_pixels[sy * width + sx];
                    float shade = 0.65f + 0.35f * sin_t;
                    dst_row[x] = blend_pixel_scalar(0xFF000000, c, (uint16_t)(shade * 256.0f));
                } else {
                    dst_row[x] = 0xFF08080C;
                }
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_LUMA: {
        float threshold = progress * 1.2f - 0.1f;
        float softness = 0.08f;
        float aspect = (float)width / (float)height;

        for (uint32_t y = 0; y < height; ++y) {
            float vy = (float)y / (float)height;
            uint32_t *dst_row = dst + y * stride_pixels;
            for (uint32_t x = 0; x < width; ++x) {
                float vx = ((float)x / (float)width) * aspect;
                float n = value_noise2(vx * 4.0f, vy * 4.0f);
                float alpha = smoothstepf(threshold - softness, threshold + softness, n);
                float mix_val = 1.0f - alpha;
                uint16_t wb = (uint16_t)(mix_val * 256.0f);
                uint32_t ca = old_pixels[y * width + x];
                uint32_t cb = new_pixels[y * width + x];
                dst_row[x] = blend_pixel_scalar(ca, cb, wb);
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_LIGHT_LEAK: {
        float mix_factor = smoothstepf(0.25f, 0.75f, progress);
        uint16_t wb = (uint16_t)(mix_factor * 256.0f);
        float sweep = progress * 1.6f - 0.3f;
        float env = sinf(progress * 3.1415926535f);
        env = env * env;

        for (uint32_t y = 0; y < height; ++y) {
            float vy = (float)y / (float)height;
            uint32_t *dst_row = dst + y * stride_pixels;
            for (uint32_t x = 0; x < width; ++x) {
                float vx = (float)x / (float)width;
                float dist = fabsf(vx + vy * 0.25f - sweep);
                float flare_beam = expf(-dist * dist * 32.0f);

                uint32_t ca = old_pixels[y * width + x];
                uint32_t cb = new_pixels[y * width + x];
                uint32_t blended = blend_pixel_scalar(ca, cb, wb);

                int flash = (int)(flare_beam * 180.0f * env);
                dst_row[x] = add_saturate_rgb(blended, flash);
            }
        }
        return true;
    }

    case WAYWAL_TRANSITION_PAGE_CURL: {
        float R = 0.12f;
        float line = 1.02f - progress * 1.25f;

        for (uint32_t y = 0; y < height; ++y) {
            float vy = (float)y / (float)height;
            uint32_t *dst_row = dst + y * stride_pixels;
            for (uint32_t x = 0; x < width; ++x) {
                float vx = (float)x / (float)width;
                float s = (vx + vy) * 0.5f;

                if (s < line) {
                    dst_row[x] = old_pixels[y * width + x];
                } else if (s < line + R) {
                    float dist = s - line;
                    float curl_u = vx - dist;
                    float curl_v = vy - dist;
                    if (curl_u >= 0.0f && curl_u <= 1.0f && curl_v >= 0.0f && curl_v <= 1.0f) {
                        int sx = (int)(curl_u * width);
                        int sy = (int)(curl_v * height);
                        if (sx >= (int)width)
                            sx = width - 1;
                        if (sy >= (int)height)
                            sy = height - 1;
                        uint32_t col = old_pixels[sy * width + sx];
                        float curl_light = 0.85f + 0.35f * sinf((dist / R) * 3.1415926535f);
                        dst_row[x] =
                            blend_pixel_scalar(0xFF000000, col, (uint16_t)(curl_light * 230.0f));
                    } else {
                        float shadow = 0.5f + 0.5f * smoothstepf(0.0f, R, dist);
                        uint32_t col = new_pixels[y * width + x];
                        dst_row[x] =
                            blend_pixel_scalar(0xFF000000, col, (uint16_t)(shadow * 256.0f));
                    }
                } else {
                    dst_row[x] = new_pixels[y * width + x];
                }
            }
        }
        return true;
    }

    default:
        break;
    }

    return false;
}

bool render_engine_execute_transition(render_engine_t *re, dmabuf_bo_t *target_bo,
                                      dmabuf_bo_t *old_bo, dmabuf_bo_t *new_bo,
                                      const waywal_transition_params_t *params)
{
    if (!target_bo || !old_bo || !new_bo || !params) {
        return false;
    }

    if (re && re->has_gpu_compute && re->gpu_ctx) {
        if (gpu_compute_dispatch_transition((gpu_compute_ctx_t *)re->gpu_ctx, target_bo, old_bo,
                                            new_bo, params)) {
            return true;
        }
    }

    /* Fallback path: CPU mapping and software SIMD kernel */
    uint32_t stride_target = 0, stride_old = 0, stride_new = 0;
    void *map_data_target = NULL, *map_data_old = NULL, *map_data_new = NULL;

    void *map_target = gbm_bo_map(target_bo->gbm_bo, 0, 0, target_bo->width, target_bo->height,
                                  GBM_BO_TRANSFER_WRITE, &stride_target, &map_data_target);
    void *map_old = gbm_bo_map(old_bo->gbm_bo, 0, 0, old_bo->width, old_bo->height,
                               GBM_BO_TRANSFER_READ, &stride_old, &map_data_old);
    void *map_new = gbm_bo_map(new_bo->gbm_bo, 0, 0, new_bo->width, new_bo->height,
                               GBM_BO_TRANSFER_READ, &stride_new, &map_data_new);

    bool ok = false;
    if (map_target && map_old && map_new) {
        ok = render_engine_execute_cpu_transition((uint32_t *)map_target, (const uint32_t *)map_old,
                                                  (const uint32_t *)map_new, target_bo->width,
                                                  target_bo->height, stride_target, params);
    }

    if (map_target)
        gbm_bo_unmap(target_bo->gbm_bo, map_data_target);
    if (map_old)
        gbm_bo_unmap(old_bo->gbm_bo, map_data_old);
    if (map_new)
        gbm_bo_unmap(new_bo->gbm_bo, map_data_new);

    return ok;
}
