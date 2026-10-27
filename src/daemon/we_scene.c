// SPDX-License-Identifier: GPL-3.0-or-later
#include "we_scene_internal.h"

#include "waywal/dmabuf.h"
#include "waywal/log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
// clang-format off
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
// clang-format on
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* =========================================================================
 * OpenGL ES 3.1 Initialization, Matrix Math & Scene Graph Rendering
 * ========================================================================= */

static void mat4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void mat4_mul(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            r[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] + a[1 * 4 + row] * b[col * 4 + 1] +
                               a[2 * 4 + row] * b[col * 4 + 2] + a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

/* Top-down orthographic projection:
 * World (0, ortho_h) -> Clip (-1, -1) (FBO row 0, top of screen)
 * World (ortho_w, 0) -> Clip (+1, +1) (FBO row H-1, bottom of screen) */
static void mat4_ortho_topdown(float ortho_w, float ortho_h, float offset_x, float offset_y,
                               float out[16])
{
    mat4_identity(out);
    float w = ortho_w > 1.0f ? ortho_w : 1920.0f;
    float h = ortho_h > 1.0f ? ortho_h : 1080.0f;
    out[0] = 2.0f / w;
    out[5] = -2.0f / h;
    out[10] = -1.0f / 2000.0f;
    out[12] = -1.0f - (2.0f * offset_x / w);
    out[13] = 1.0f + (2.0f * offset_y / h);
    out[14] = 0.0f;
    out[15] = 1.0f;
}

static void compute_node_local_matrix(const we_scene_object_t *node, float parallax_dx,
                                      float parallax_dy, float out[16])
{
    float ox = node->origin[0] + parallax_dx * node->parallax_depth[0];
    float oy = node->origin[1] + parallax_dy * node->parallax_depth[1];
    float oz = node->origin[2];

    float sx = node->scale[0];
    float sy = node->scale[1];
    float sz = node->scale[2];

    float rz = node->angles[2];
    float cz = cosf(rz);
    float snz = sinf(rz);

    mat4_identity(out);
    out[0] = cz * sx;
    out[1] = snz * sx;
    out[4] = -snz * sy;
    out[5] = cz * sy;
    out[10] = sz;
    out[12] = ox;
    out[13] = oy;
    out[14] = oz;
}

static void compute_object_node_world_matrix(const we_scene_t *scene, const we_scene_object_t *obj,
                                             float parallax_dx, float parallax_dy, float out[16])
{
    const we_scene_object_t *chain[16];
    int depth = 0;
    const we_scene_object_t *cur = obj;
    while (cur && depth < 16) {
        chain[depth++] = cur;
        if (cur->parent_id < 0) {
            break;
        }
        const we_scene_object_t *parent = NULL;
        for (uint32_t i = 0; i < scene->object_count; i++) {
            if (scene->objects[i].id == cur->parent_id && &scene->objects[i] != cur) {
                parent = &scene->objects[i];
                break;
            }
        }
        bool cycle = false;
        for (int k = 0; k < depth && parent; k++) {
            if (chain[k] == parent) {
                cycle = true;
                break;
            }
        }
        if (!parent || cycle) {
            break;
        }
        cur = parent;
    }

    mat4_identity(out);
    for (int d = depth - 1; d >= 0; d--) {
        float local[16];
        compute_node_local_matrix(chain[d], parallax_dx, parallax_dy, local);
        mat4_mul(out, local, out);
    }
}

/* Computes object model matrix with Top-Down Orientation Invariant and parent hierarchy:
 * M_model = M_parent_chain * M_local_node * S(size.x * 0.5f, -size.y * 0.5f, 1.0f) */
static void compute_object_model_matrix(const we_scene_t *scene, const we_scene_object_t *obj,
                                        float parallax_dx, float parallax_dy, float out[16])
{
    if (obj->fullscreen) {
        mat4_identity(out);
        out[0] = scene->ortho_width * 0.5f;
        out[5] = -scene->ortho_height * 0.5f;
        out[10] = obj->scale[2];
        out[12] = scene->ortho_width * 0.5f;
        out[13] = scene->ortho_height * 0.5f;
        out[14] = obj->origin[2];
        return;
    }

    float world_node[16];
    compute_object_node_world_matrix(scene, obj, parallax_dx, parallax_dy, world_node);

    float quad_scale[16];
    mat4_identity(quad_scale);
    quad_scale[0] = obj->size[0] * 0.5f;
    quad_scale[5] = -obj->size[1] * 0.5f;
    mat4_mul(world_node, quad_scale, out);
}

static uint32_t create_solid_tex(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint8_t px[4] = {r, g, b, a};
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return (uint32_t)tex;
}

static uint32_t create_noise_tex_64(void)
{
    uint8_t px[64 * 64 * 4];
    uint32_t st = 0x9E3779B9u;
    for (size_t i = 0; i < sizeof(px); i++) {
        st ^= st << 13;
        st ^= st >> 17;
        st ^= st << 5;
        px[i] = (uint8_t)(st & 0xFFu);
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return (uint32_t)tex;
}

static GLint get_audio_uniform_loc(GLuint prog, const char *base_name)
{
    GLint loc = glGetUniformLocation(prog, base_name);
    if (loc < 0) {
        char arr_name[64];
        snprintf(arr_name, sizeof(arr_name), "%s[0]", base_name);
        loc = glGetUniformLocation(prog, arr_name);
    }
    return loc;
}

static void cache_pass_uniform_locs(we_material_pass_t *pass)
{
    if (!pass || pass->gl_prog == 0) {
        return;
    }
    GLuint prog = pass->gl_prog;
    we_pass_uniform_locs_t *ul = &pass->ulocs;
    memset(ul, 0xFF, sizeof(*ul));
    ul->initialized = true;

    ul->time = glGetUniformLocation(prog, "g_Time");
    ul->daytime = glGetUniformLocation(prog, "g_Daytime");
    ul->frametime = glGetUniformLocation(prog, "g_Frametime");
    ul->media_playing = glGetUniformLocation(prog, "u_media_playing");
    ul->pointer_pos = glGetUniformLocation(prog, "g_PointerPosition");
    ul->pointer_pos_last = glGetUniformLocation(prog, "g_PointerPositionLast");
    ul->texel_size = glGetUniformLocation(prog, "g_TexelSize");
    ul->texel_size_half = glGetUniformLocation(prog, "g_TexelSizeHalf");
    ul->screen = glGetUniformLocation(prog, "g_Screen");
    ul->view_up = glGetUniformLocation(prog, "g_ViewUp");
    ul->view_right = glGetUniformLocation(prog, "g_ViewRight");
    ul->orient_up = glGetUniformLocation(prog, "g_OrientationUp");
    ul->orient_right = glGetUniformLocation(prog, "g_OrientationRight");
    ul->orient_forward = glGetUniformLocation(prog, "g_OrientationForward");
    ul->eye_pos = glGetUniformLocation(prog, "g_EyePosition");
    ul->model_matrix = glGetUniformLocation(prog, "g_ModelMatrix");
    ul->view_proj_matrix = glGetUniformLocation(prog, "g_ViewProjectionMatrix");
    ul->mvp = glGetUniformLocation(prog, "g_ModelViewProjectionMatrix");
    ul->model_matrix_inv = glGetUniformLocation(prog, "g_ModelViewProjectionMatrixInverse");
    ul->vp_matrix = glGetUniformLocation(prog, "g_EffectTextureProjectionMatrix");
    ul->parallax_pos = glGetUniformLocation(prog, "g_EffectTextureProjectionMatrixInverse");
    ul->alpha = glGetUniformLocation(prog, "g_Alpha");
    ul->user_alpha = glGetUniformLocation(prog, "g_UserAlpha");
    ul->brightness = glGetUniformLocation(prog, "g_Brightness");
    ul->view_forward = glGetUniformLocation(prog, "g_Contrast");
    ul->color = glGetUniformLocation(prog, "g_Color");
    ul->color4 = glGetUniformLocation(prog, "g_Color4");
    ul->album_art = glGetUniformLocation(prog, "u_album_art");

    ul->audio_l16 = get_audio_uniform_loc(prog, "g_AudioSpectrum16Left");
    ul->audio_r16 = get_audio_uniform_loc(prog, "g_AudioSpectrum16Right");
    ul->audio_l32 = get_audio_uniform_loc(prog, "g_AudioSpectrum32Left");
    ul->audio_r32 = get_audio_uniform_loc(prog, "g_AudioSpectrum32Right");
    ul->audio_l64 = get_audio_uniform_loc(prog, "g_AudioSpectrum64Left");
    ul->audio_r64 = get_audio_uniform_loc(prog, "g_AudioSpectrum64Right");

    for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
        char uname[64];
        snprintf(uname, sizeof(uname), "g_Texture%u", s);
        ul->tex_sampler[s] = glGetUniformLocation(prog, uname);
        snprintf(uname, sizeof(uname), "g_Texture%uResolution", s);
        ul->tex_resolution[s] = glGetUniformLocation(prog, uname);
        snprintf(uname, sizeof(uname), "g_Texture%uTranslation", s);
        ul->tex_translation[s] = glGetUniformLocation(prog, uname);
        snprintf(uname, sizeof(uname), "g_Texture%uRotation", s);
        ul->tex_rotation[s] = glGetUniformLocation(prog, uname);
    }

    for (uint32_t u = 0; u < pass->prep.meta.uniform_count && u < WE_SHADER_MAX_UNIFORMS; u++) {
        const we_shader_uniform_meta_t *um = &pass->prep.meta.uniforms[u];
        if (um->sampler_slot < 0 && um->name[0] != '\0') {
            ul->custom_uniforms[u] = glGetUniformLocation(prog, um->name);
        }
    }

    /* Pre-initialize invariant program uniforms once */
    glUseProgram(prog);
    float ident[16];
    mat4_identity(ident);
    if (ul->view_up >= 0) {
        glUniform3f(ul->view_up, 0.0f, 1.0f, 0.0f);
    }
    if (ul->view_right >= 0) {
        glUniform3f(ul->view_right, 1.0f, 0.0f, 0.0f);
    }
    if (ul->orient_up >= 0) {
        glUniform3f(ul->orient_up, 0.0f, 1.0f, 0.0f);
    }
    if (ul->orient_right >= 0) {
        glUniform3f(ul->orient_right, 1.0f, 0.0f, 0.0f);
    }
    if (ul->orient_forward >= 0) {
        glUniform3f(ul->orient_forward, 0.0f, 0.0f, 1.0f);
    }
    if (ul->model_matrix_inv >= 0) {
        glUniformMatrix4fv(ul->model_matrix_inv, 1, GL_FALSE, ident);
    }
    if (ul->vp_matrix >= 0) {
        glUniformMatrix4fv(ul->vp_matrix, 1, GL_FALSE, ident);
    }
    if (ul->parallax_pos >= 0) {
        glUniformMatrix4fv(ul->parallax_pos, 1, GL_FALSE, ident);
    }
    if (ul->user_alpha >= 0) {
        glUniform1f(ul->user_alpha, 1.0f);
    }
    if (ul->view_forward >= 0) {
        glUniform1f(ul->view_forward, 1.0f);
    }
    GLint loc_sat = glGetUniformLocation(prog, "g_Saturation");
    if (loc_sat >= 0) {
        glUniform1f(loc_sat, 1.0f);
    }
    for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
        if (ul->tex_sampler[s] >= 0) {
            glUniform1i(ul->tex_sampler[s], (GLint)s);
        }
    }
}

static void ensure_egl_procs(we_scene_t *scene)
{
    if (!scene || scene->egl_procs_resolved) {
        return;
    }
    scene->fn_eglCreateImageKHR = __extension__(void *) eglGetProcAddress("eglCreateImageKHR");
    scene->fn_eglDestroyImageKHR = __extension__(void *) eglGetProcAddress("eglDestroyImageKHR");
    scene->fn_glEGLImageTargetTexture2DOES =
        __extension__(void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
    scene->fn_glEGLImageTargetTexStorageEXT =
        __extension__(void *) eglGetProcAddress("glEGLImageTargetTexStorageEXT");
    scene->fn_eglCreateSyncKHR = __extension__(void *) eglGetProcAddress("eglCreateSyncKHR");
    scene->fn_eglClientWaitSyncKHR =
        __extension__(void *) eglGetProcAddress("eglClientWaitSyncKHR");
    scene->fn_eglDestroySyncKHR = __extension__(void *) eglGetProcAddress("eglDestroySyncKHR");
    scene->egl_procs_resolved = true;
}

static void upload_pass_gl(we_material_pass_t *pass)
{
    if (!pass) {
        return;
    }
    for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
        if (pass->textures[s] && pass->textures[s]->gl_tex == 0) {
            we_tex_upload_gl(pass->textures[s]);
        }
    }
    if (pass->prep.vert_glsl && pass->prep.frag_glsl && pass->gl_prog == 0) {
        pass->gl_prog = we_shader_compile_program(pass->prep.vert_glsl, pass->prep.frag_glsl,
                                                  pass->shader_name);
        if (pass->gl_prog == 0 && strcasecmp(pass->shader_name, "genericimage2") != 0) {
            we_shader_preprocessed_t fb;
            if (we_shader_preprocess_pair(NULL, NULL, NULL, pass->combos, pass->combo_count,
                                          pass->bound_tex_mask, false, &fb)) {
                pass->gl_prog =
                    we_shader_compile_program(fb.vert_glsl, fb.frag_glsl, "genericimage2_fallback");
                we_shader_preprocessed_free(&fb);
            }
        }
        if (pass->gl_prog != 0) {
            cache_pass_uniform_locs(pass);
        }
        free(pass->prep.vert_glsl);
        pass->prep.vert_glsl = NULL;
        free(pass->prep.frag_glsl);
        pass->prep.frag_glsl = NULL;
    }
}

static EGLDisplay s_we_shared_dpy = EGL_NO_DISPLAY;
static EGLContext s_we_shared_ctx = EGL_NO_CONTEXT;
static uint32_t s_we_shared_refcount = 0;

bool we_scene_init_gl(we_scene_t *scene, dmabuf_context_t *dmabuf_ctx)
{
    if (!scene) {
        return false;
    }
    if (scene->gl_initialized) {
        return true;
    }

    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    bool owns_egl = false;

    if (dpy != EGL_NO_DISPLAY && ctx != EGL_NO_CONTEXT) {
        if (s_we_shared_refcount > 0 && dpy == s_we_shared_dpy && ctx == s_we_shared_ctx) {
            owns_egl = true;
            s_we_shared_refcount++;
        }
    } else if (s_we_shared_refcount > 0 && s_we_shared_dpy != EGL_NO_DISPLAY &&
               s_we_shared_ctx != EGL_NO_CONTEXT) {
        if (eglMakeCurrent(s_we_shared_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s_we_shared_ctx)) {
            dpy = s_we_shared_dpy;
            ctx = s_we_shared_ctx;
            owns_egl = true;
            s_we_shared_refcount++;
        }
    }

    if (dpy == EGL_NO_DISPLAY || ctx == EGL_NO_CONTEXT) {
        if (!dmabuf_ctx || !dmabuf_ctx->gbm || !dmabuf_ctx->available) {
            return false;
        }
        PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
            (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (get_platform_display) {
            dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, dmabuf_ctx->gbm, NULL);
        }
        if (dpy == EGL_NO_DISPLAY) {
            dpy = eglGetDisplay((EGLNativeDisplayType)dmabuf_ctx->gbm);
        }
        if (dpy == EGL_NO_DISPLAY) {
            return false;
        }
        EGLint major = 0, minor = 0;
        if (!eglInitialize(dpy, &major, &minor) || !eglBindAPI(EGL_OPENGL_ES_API)) {
            eglTerminate(dpy);
            return false;
        }
        static const EGLint ctx_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE,
        };
        ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attribs);
        if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            if (ctx != EGL_NO_CONTEXT) {
                eglDestroyContext(dpy, ctx);
            }
            eglTerminate(dpy);
            return false;
        }
        owns_egl = true;
        s_we_shared_dpy = dpy;
        s_we_shared_ctx = ctx;
        s_we_shared_refcount = 1;
    }

    scene->egl_display = dpy;
    scene->egl_context = ctx;
    scene->owns_egl = owns_egl;
    ensure_egl_procs(scene);

    /* Unit quad VAO/VBO (Top-Down Orientation Invariant:
     * v0: (-1,-1,0) uv(0,0), v1: (+1,-1,0) uv(1,0), v2: (+1,+1,0) uv(1,1), v3: (-1,+1,0) uv(0,1))
     */
    static const float quad_verts[] = {
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f,  -1.0f, 0.0f, 1.0f, 0.0f,
        1.0f,  1.0f,  0.0f, 1.0f, 1.0f, -1.0f, 1.0f,  0.0f, 0.0f, 1.0f,
    };

    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_verts), quad_verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *)(3 * sizeof(float)));
    glBindVertexArray(0);

    scene->quad_vao = vao;
    scene->quad_vbo = vbo;

    /* Presentation shader: blits scene_fbo_tex into target_bo with forced opaque alpha 1.0 */
    static const char present_vs[] = "#version 310 es\n"
                                     "precision highp float;\n"
                                     "layout(location = 0) in vec3 a_Position;\n"
                                     "layout(location = 1) in vec2 a_TexCoord;\n"
                                     "out vec2 v_uv;\n"
                                     "void main() {\n"
                                     "    gl_Position = vec4(a_Position, 1.0);\n"
                                     "    v_uv = a_TexCoord;\n"
                                     "}\n";
    static const char present_fs[] = "#version 310 es\n"
                                     "precision highp float;\n"
                                     "in vec2 v_uv;\n"
                                     "uniform sampler2D u_tex;\n"
                                     "out vec4 fragColor;\n"
                                     "void main() {\n"
                                     "    vec4 c = texture(u_tex, v_uv);\n"
                                     "    fragColor = vec4(c.rgb, 1.0);\n"
                                     "}\n";
    scene->prog_present = we_shader_compile_program(present_vs, present_fs, "we_present");
    scene->present_u_tex_loc =
        scene->prog_present ? glGetUniformLocation(scene->prog_present, "u_tex") : -1;

    /* Default composite shader: draws an effect-processed RGBA texture into scene_fbo */
    static const char default_vs[] =
        "#version 310 es\n"
        "precision highp float;\n"
        "layout(location = 0) in vec3 a_Position;\n"
        "layout(location = 1) in vec2 a_TexCoord;\n"
        "uniform mat4 g_ModelViewProjectionMatrix;\n"
        "out vec2 v_uv;\n"
        "void main() {\n"
        "    gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);\n"
        "    v_uv = a_TexCoord;\n"
        "}\n";
    static const char default_fs[] = "#version 310 es\n"
                                     "precision highp float;\n"
                                     "in vec2 v_uv;\n"
                                     "uniform sampler2D g_Texture0;\n"
                                     "out vec4 fragColor;\n"
                                     "void main() {\n"
                                     "    fragColor = texture(g_Texture0, v_uv);\n"
                                     "}\n";
    scene->default_program = we_shader_compile_program(default_vs, default_fs, "we_default");
    memset(&scene->default_prog_ulocs, 0xFF, sizeof(scene->default_prog_ulocs));
    if (scene->default_program) {
        scene->default_prog_ulocs.initialized = true;
        scene->default_prog_ulocs.mvp =
            glGetUniformLocation(scene->default_program, "g_ModelViewProjectionMatrix");
        scene->default_prog_ulocs.tex_sampler[0] =
            glGetUniformLocation(scene->default_program, "g_Texture0");
    }

    /* Batched SIMD Particle shader: supports per-vertex color/alpha at location = 2 */
    static const char particle_vs[] =
        "#version 310 es\n"
        "precision highp float;\n"
        "layout(location = 0) in vec3 a_Position;\n"
        "layout(location = 1) in vec2 a_TexCoord;\n"
        "layout(location = 2) in vec4 a_Color;\n"
        "uniform mat4 g_ModelViewProjectionMatrix;\n"
        "uniform vec2 g_Texture0Translation;\n"
        "uniform vec4 g_Texture0Rotation;\n"
        "out vec2 v_TexCoord;\n"
        "out vec4 v_Color;\n"
        "void main() {\n"
        "    gl_Position = g_ModelViewProjectionMatrix * vec4(a_Position, 1.0);\n"
        "    v_TexCoord = g_Texture0Translation + a_TexCoord.x * g_Texture0Rotation.xy + "
        "a_TexCoord.y * g_Texture0Rotation.zw;\n"
        "    v_Color = a_Color;\n"
        "}\n";
    static const char particle_fs[] = "#version 310 es\n"
                                      "precision highp float;\n"
                                      "in vec2 v_TexCoord;\n"
                                      "in vec4 v_Color;\n"
                                      "uniform sampler2D g_Texture0;\n"
                                      "uniform vec4 g_Color4;\n"
                                      "out vec4 fragColor;\n"
                                      "void main() {\n"
                                      "    vec4 tex = texture(g_Texture0, v_TexCoord);\n"
                                      "    fragColor = tex * v_Color * g_Color4;\n"
                                      "}\n";
    scene->particle_program = we_shader_compile_program(particle_vs, particle_fs, "we_particle");
    if (scene->particle_program) {
        scene->part_mvp_loc =
            glGetUniformLocation(scene->particle_program, "g_ModelViewProjectionMatrix");
        scene->part_color4_loc = glGetUniformLocation(scene->particle_program, "g_Color4");
        scene->part_trans_loc =
            glGetUniformLocation(scene->particle_program, "g_Texture0Translation");
        scene->part_rot_loc = glGetUniformLocation(scene->particle_program, "g_Texture0Rotation");
        scene->part_tex0_loc = glGetUniformLocation(scene->particle_program, "g_Texture0");
    } else {
        scene->part_mvp_loc = scene->part_color4_loc = scene->part_trans_loc = scene->part_rot_loc =
            scene->part_tex0_loc = -1;
    }

    GLuint tfbo = 0;
    glGenFramebuffers(1, &tfbo);
    scene->target_fbo = tfbo;

    /* Built-in utility textures */
    scene->tex_util_white = create_solid_tex(255, 255, 255, 255);
    scene->tex_util_black = create_solid_tex(0, 0, 0, 255);
    scene->tex_util_flatnormal = create_solid_tex(128, 128, 255, 128);
    scene->tex_util_noise = create_noise_tex_64();
    if (scene->album_art_rgba && scene->album_art_w > 0 && scene->album_art_h > 0) {
        GLuint atex = 0;
        glGenTextures(1, &atex);
        glBindTexture(GL_TEXTURE_2D, atex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)scene->album_art_w,
                     (GLsizei)scene->album_art_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     scene->album_art_rgba);
        scene->tex_album_art = (uint32_t)atex;
    }

    /* Upload all scene object textures, puppet meshes, particle buffers, and compile shaders */
    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];
        if (obj->has_base_pass) {
            upload_pass_gl(&obj->base_pass);
        }
        if (obj->puppet) {
            we_puppet_upload_gl(obj->puppet);
        }
        if (obj->particle_sys) {
            we_particle_upload_gl(obj->particle_sys);
        }
        for (uint32_t e = 0; e < obj->effect_count; e++) {
            for (uint32_t p = 0; p < obj->effects[e].pass_count; p++) {
                upload_pass_gl(&obj->effects[e].passes[p]);
            }
        }
    }

    if (scene->pkg) {
        we_pkg_close(scene->pkg);
        scene->pkg = NULL;
    }

    scene->gl_initialized = true;
    return true;
}

we_scene_t *we_scene_create(dmabuf_context_t *dmabuf_ctx, const char *workshop_dir,
                            const char *prop_overrides)
{
    we_scene_t *scene = we_scene_load_cpu(workshop_dir, prop_overrides);
    if (!scene) {
        return NULL;
    }
    we_scene_init_gl(scene, dmabuf_ctx);
    return scene;
}

static void ensure_fbo_pair(uint32_t *inout_fbo, uint32_t *inout_tex, uint32_t *inout_w,
                            uint32_t *inout_h, uint32_t w, uint32_t h)
{
    if (*inout_fbo && *inout_tex && *inout_w == w && *inout_h == h) {
        return;
    }
    if (*inout_fbo) {
        GLuint f = *inout_fbo;
        glDeleteFramebuffers(1, &f);
    }
    if (*inout_tex) {
        GLuint t = *inout_tex;
        glDeleteTextures(1, &t);
    }
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    *inout_fbo = fbo;
    *inout_tex = tex;
    *inout_w = w;
    *inout_h = h;
}

static we_rt_fbo_t *get_or_create_scene_rt_fbo(we_scene_t *scene, const char *name, uint32_t w,
                                               uint32_t h)
{
    if (!scene || !name || name[0] == '\0' || w == 0 || h == 0) {
        return NULL;
    }
    for (uint32_t i = 0; i < scene->rt_fbo_count; i++) {
        if (strcasecmp(scene->rt_fbos[i].name, name) == 0) {
            ensure_fbo_pair(&scene->rt_fbos[i].gl_fbo, &scene->rt_fbos[i].gl_tex,
                            &scene->rt_fbos[i].width, &scene->rt_fbos[i].height, w, h);
            return &scene->rt_fbos[i];
        }
    }
    if (scene->rt_fbo_count >= WE_MAX_SCENE_RT_FBOS) {
        return NULL;
    }
    we_rt_fbo_t *rt = &scene->rt_fbos[scene->rt_fbo_count++];
    memset(rt, 0, sizeof(*rt));
    snprintf(rt->name, sizeof(rt->name), "%s", name);
    rt->scale = 1;
    ensure_fbo_pair(&rt->gl_fbo, &rt->gl_tex, &rt->width, &rt->height, w, h);
    return rt;
}

static uint32_t resolve_named_rt_tex(we_scene_t *scene, we_effect_t *eff, const char *name,
                                     uint32_t prev_tex, float out_res[4])
{
    if (!name || name[0] == '\0' || strcasecmp(name, "previous") == 0) {
        return prev_tex;
    }
    if (strcasecmp(name, "_rt_FullFrameBuffer") == 0 ||
        strcasecmp(name, "_rt_fullframebuffer") == 0) {
        out_res[0] = (float)scene->scene_fbo_w;
        out_res[1] = (float)scene->scene_fbo_h;
        out_res[2] = out_res[0];
        out_res[3] = out_res[1];
        return scene->full_fb_tex ? scene->full_fb_tex : scene->scene_fbo_tex;
    }
    if (strncasecmp(name, "util/white", 10) == 0 || strcasecmp(name, "_rt_default_white") == 0) {
        out_res[0] = out_res[1] = out_res[2] = out_res[3] = 1.0f;
        return scene->tex_util_white;
    }
    if (strncasecmp(name, "util/black", 10) == 0) {
        out_res[0] = out_res[1] = out_res[2] = out_res[3] = 1.0f;
        return scene->tex_util_black;
    }
    if (strncasecmp(name, "util/flatnormal", 15) == 0) {
        out_res[0] = out_res[1] = out_res[2] = out_res[3] = 1.0f;
        return scene->tex_util_flatnormal;
    }
    if (strncasecmp(name, "util/noise", 10) == 0 || strncasecmp(name, "util/clouds", 11) == 0) {
        out_res[0] = out_res[1] = out_res[2] = out_res[3] = 64.0f;
        return scene->tex_util_noise;
    }
    if (strcasecmp(name, "_rt_AlbumCover") == 0 || strcasecmp(name, "u_album_art") == 0) {
        float aw = scene->album_art_w > 0 ? (float)scene->album_art_w : 1.0f;
        float ah = scene->album_art_h > 0 ? (float)scene->album_art_h : 1.0f;
        out_res[0] = aw;
        out_res[1] = ah;
        out_res[2] = aw;
        out_res[3] = ah;
        return scene->tex_album_art ? scene->tex_album_art : scene->tex_util_black;
    }
    if (eff) {
        for (uint32_t i = 0; i < eff->fbo_count; i++) {
            if (strcasecmp(eff->fbos[i].name, name) == 0 && eff->fbos[i].gl_tex) {
                out_res[0] = (float)eff->fbos[i].width;
                out_res[1] = (float)eff->fbos[i].height;
                out_res[2] = out_res[0];
                out_res[3] = out_res[1];
                return eff->fbos[i].gl_tex;
            }
        }
    }
    for (uint32_t i = 0; i < scene->rt_fbo_count; i++) {
        if (strcasecmp(scene->rt_fbos[i].name, name) == 0 && scene->rt_fbos[i].gl_tex) {
            out_res[0] = (float)scene->rt_fbos[i].width;
            out_res[1] = (float)scene->rt_fbos[i].height;
            out_res[2] = out_res[0];
            out_res[3] = out_res[1];
            return scene->rt_fbos[i].gl_tex;
        }
    }
    return prev_tex;
}

static void bind_pass_uniforms(we_scene_t *scene, we_scene_object_t *obj, we_effect_t *eff,
                               we_material_pass_t *pass, const float model[16], const float vp[16],
                               const float mvp[16], uint32_t prev_tex, uint32_t rt_w, uint32_t rt_h,
                               uint32_t target_w, uint32_t target_h,
                               const waywal_shader_frame_params_t *params)
{
    GLuint prog = pass->gl_prog;
    if (!pass->ulocs.initialized && prog != 0) {
        cache_pass_uniform_locs(pass);
    }
    const we_pass_uniform_locs_t *ul = &pass->ulocs;
    glUseProgram(prog);

    float time_sec = params ? params->time_sec : 0.0f;
    float dt_sec =
        (params && params->time_delta_sec > 0.0f) ? params->time_delta_sec : (1.0f / 60.0f);

    if (ul->time >= 0) {
        glUniform1f(ul->time, time_sec);
    }
    if (ul->daytime >= 0) {
        float daytime = params ? (params->date[3] / 86400.0f) : 0.5f;
        glUniform1f(ul->daytime, daytime);
    }
    if (ul->frametime >= 0) {
        glUniform1f(ul->frametime, dt_sec);
    }
    if (ul->media_playing >= 0) {
        float mplay =
            (params && params->media_playing > 0.0f) ? params->media_playing : scene->media_playing;
        glUniform1f(ul->media_playing, mplay);
    }

    uint32_t norm_w = target_w > 0 ? target_w : rt_w;
    uint32_t norm_h = target_h > 0 ? target_h : rt_h;
    float ptr[2] = {0.5f, 0.5f};
    if (params && norm_w > 0 && norm_h > 0 &&
        (params->mouse[0] > 0.0f || params->mouse[1] > 0.0f)) {
        ptr[0] = params->mouse[0] / (float)norm_w;
        ptr[1] = params->mouse[1] / (float)norm_h;
    }
    if (ul->pointer_pos >= 0) {
        glUniform2f(ul->pointer_pos, ptr[0], ptr[1]);
    }
    if (ul->pointer_pos_last >= 0) {
        glUniform2f(ul->pointer_pos_last, scene->last_pointer[0], scene->last_pointer[1]);
    }

    float fw = rt_w > 0 ? (float)rt_w : 1920.0f;
    float fh = rt_h > 0 ? (float)rt_h : 1080.0f;
    if (ul->texel_size >= 0) {
        glUniform2f(ul->texel_size, 1.0f / fw, 1.0f / fh);
    }
    if (ul->texel_size_half >= 0) {
        glUniform2f(ul->texel_size_half, 0.5f / fw, 0.5f / fh);
    }
    if (ul->screen >= 0) {
        glUniform3f(ul->screen, fw, fh, fw / fh);
    }
    if (ul->eye_pos >= 0) {
        glUniform3fv(ul->eye_pos, 1, scene->camera_eye);
    }

    float ident[16];
    mat4_identity(ident);
    if (ul->model_matrix >= 0) {
        glUniformMatrix4fv(ul->model_matrix, 1, GL_FALSE, model ? model : ident);
    }
    if (ul->view_proj_matrix >= 0) {
        glUniformMatrix4fv(ul->view_proj_matrix, 1, GL_FALSE, vp ? vp : ident);
    }
    if (ul->mvp >= 0) {
        glUniformMatrix4fv(ul->mvp, 1, GL_FALSE, mvp ? mvp : ident);
    }

    /* Object color & alpha uniforms */
    float alpha = obj ? obj->alpha : 1.0f;
    float brightness = obj ? obj->brightness : 1.0f;
    const float *col = obj ? obj->color : (const float[]){1.0f, 1.0f, 1.0f};
    if (ul->alpha >= 0) {
        glUniform1f(ul->alpha, alpha);
    }
    if (ul->brightness >= 0) {
        glUniform1f(ul->brightness, brightness);
    }
    if (ul->color >= 0) {
        glUniform3fv(ul->color, 1, col);
    }
    if (ul->color4 >= 0) {
        glUniform4f(ul->color4, col[0], col[1], col[2], alpha);
    }

    /* Audio spectrum uniforms */
    const int32_t audio_locs[6] = {
        ul->audio_l16, ul->audio_r16, ul->audio_l32, ul->audio_r32, ul->audio_l64, ul->audio_r64,
    };
    bool any_audio_loc = false;
    for (int a = 0; a < 6; a++) {
        if (audio_locs[a] >= 0) {
            any_audio_loc = true;
            break;
        }
    }
    if (any_audio_loc) {
        float spec64[64] = {0};
        if (params && params->audio) {
            for (int i = 0; i < 64; i++) {
                int band = (i * (int)WAYWAL_AUDIO_BARK_BANDS) / 64;
                spec64[i] = params->audio->bands[band];
            }
        }
        static const int audio_counts[6] = {16, 16, 32, 32, 64, 64};
        for (int a = 0; a < 6; a++) {
            if (audio_locs[a] >= 0) {
                glUniform1fv(audio_locs[a], audio_counts[a], spec64);
            }
        }
    }

    /* Texture slots 0..7 */
    for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
        GLuint tex_id = scene->tex_util_white;
        float res[4] = {fw, fh, fw, fh};
        float trans[2] = {0.0f, 0.0f};
        float rot[4] = {1.0f, 0.0f, 0.0f, 1.0f};

        if (s == 0 && prev_tex != 0) {
            tex_id = prev_tex;
            if (obj && obj->ping_w > 0 && obj->ping_h > 0) {
                res[0] = (float)obj->ping_w;
                res[1] = (float)obj->ping_h;
                res[2] = res[0];
                res[3] = res[1];
            }
        } else if (pass->textures[s] && pass->textures[s]->gl_tex != 0) {
            we_tex_t *wt = pass->textures[s];
            bool should_advance = (wt->last_advance_frame != scene->frame_counter);
            wt->last_advance_frame = scene->frame_counter;
            if (wt->is_video_mp4 && should_advance) {
                (void)we_tex_advance_video(wt, dt_sec);
            }
            if ((wt->flags & WE_TEX_FLAG_IS_GIF) != 0 && wt->frame_count > 0) {
                we_tex_advance_spritesheet(wt, should_advance ? dt_sec : 0.0f, trans, rot);
            }
            tex_id = wt->gl_tex;
            res[0] = (float)wt->texture_width;
            res[1] = (float)wt->texture_height;
            res[2] = (float)wt->image_width;
            res[3] = (float)wt->image_height;
        } else if (pass->tex_paths[s][0] != '\0') {
            tex_id = resolve_named_rt_tex(scene, eff, pass->tex_paths[s], prev_tex, res);
        }

        for (uint32_t b = 0; b < pass->bind_count; b++) {
            if (pass->binds[b].slot == (int32_t)s) {
                tex_id =
                    resolve_named_rt_tex(scene, eff, pass->binds[b].target_name, prev_tex, res);
            }
        }

        if (ul->tex_translation[s] >= 0) {
            glUniform2fv(ul->tex_translation[s], 1, trans);
        }
        if (ul->tex_rotation[s] >= 0) {
            glUniform4fv(ul->tex_rotation[s], 1, rot);
        }

        glActiveTexture(GL_TEXTURE0 + s);
        glBindTexture(GL_TEXTURE_2D, tex_id);

        if (ul->tex_sampler[s] >= 0) {
            glUniform1i(ul->tex_sampler[s], (GLint)s);
        }
        if (ul->tex_resolution[s] >= 0) {
            glUniform4fv(ul->tex_resolution[s], 1, res);
        }
    }
    if (ul->album_art >= 0) {
        GLuint art_tex = scene->tex_album_art ? scene->tex_album_art : scene->tex_util_black;
        glActiveTexture(GL_TEXTURE0 + WE_MAX_PASS_TEXTURES);
        glBindTexture(GL_TEXTURE_2D, art_tex);
        glUniform1i(ul->album_art, (GLint)WE_MAX_PASS_TEXTURES);
    }
    glActiveTexture(GL_TEXTURE0);

    /* Apply inline uniform defaults and material constantshadervalues */
    for (uint32_t u = 0; u < pass->prep.meta.uniform_count && u < WE_SHADER_MAX_UNIFORMS; u++) {
        const we_shader_uniform_meta_t *um = &pass->prep.meta.uniforms[u];
        if (um->sampler_slot >= 0) {
            continue;
        }
        GLint loc = ul->custom_uniforms[u];
        if (loc < 0) {
            continue;
        }

        const float *val = um->has_default_val ? um->default_val : NULL;
        uint32_t ncomp = um->num_components;
        bool has_script_cv = false;
        bool has_explicit_cv = false;

        /* Check if overridden in pass->const_vals by material_key or uniform name */
        float cond_val[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (uint32_t c = 0; c < pass->const_val_count; c++) {
            const we_const_val_t *cv = &pass->const_vals[c];
            if ((um->material_key[0] != '\0' && strcasecmp(cv->name, um->material_key) == 0) ||
                strcasecmp(cv->name, um->name) == 0) {
                if (cv->script_src && cv->script_src[0] != '\0' && cv->num_components > 0) {
                    val = cv->val;
                    ncomp = cv->num_components;
                    has_script_cv = true;
                    has_explicit_cv = true;
                } else if (cv->user_prop[0] != '\0' && strchr(cv->user_prop, '=') != NULL) {
                    bool bval = (cv->num_components > 0) ? (cv->val[0] > 0.5f) : false;
                    (void)we_project_eval_prop_bool(&scene->project, cv->user_prop, &bval);
                    cond_val[0] = bval ? 1.0f : 0.0f;
                    val = cond_val;
                    ncomp = 1;
                    has_explicit_cv = true;
                } else {
                    const we_user_prop_t *up =
                        cv->user_prop[0] ? project_find_prop(&scene->project, cv->user_prop) : NULL;
                    if (up && up->num_components > 0) {
                        val = up->val;
                        ncomp = up->num_components;
                        has_explicit_cv = true;
                    } else if (cv->num_components > 0) {
                        val = cv->val;
                        ncomp = cv->num_components;
                        has_explicit_cv = true;
                    }
                }
                break;
            }
        }

        if (!has_script_cv && !has_explicit_cv) {
            const we_user_prop_t *up = NULL;
            if (um->material_key[0] != '\0') {
                up = project_find_prop(&scene->project, um->material_key);
            }
            if (!up && um->name[0] != '\0') {
                up = project_find_prop(&scene->project, um->name);
            }
            if (up && up->num_components > 0) {
                val = up->val;
                ncomp = up->num_components;
            }
        }

        if (val && ncomp > 0) {
            if (strcmp(um->type, "float") == 0 || strcmp(um->type, "half") == 0) {
                glUniform1f(loc, val[0]);
            } else if (strcmp(um->type, "int") == 0 || strcmp(um->type, "bool") == 0) {
                glUniform1i(loc, (GLint)lroundf(val[0]));
            } else if (strcmp(um->type, "int2") == 0 || strcmp(um->type, "ivec2") == 0 ||
                       strcmp(um->type, "bvec2") == 0) {
                glUniform2i(loc, (GLint)lroundf(val[0]),
                            (GLint)lroundf(ncomp >= 2 ? val[1] : val[0]));
            } else if (strcmp(um->type, "int3") == 0 || strcmp(um->type, "ivec3") == 0 ||
                       strcmp(um->type, "bvec3") == 0) {
                glUniform3i(loc, (GLint)lroundf(val[0]),
                            (GLint)lroundf(ncomp >= 2 ? val[1] : val[0]),
                            (GLint)lroundf(ncomp >= 3 ? val[2] : val[0]));
            } else if (strcmp(um->type, "int4") == 0 || strcmp(um->type, "ivec4") == 0 ||
                       strcmp(um->type, "bvec4") == 0) {
                glUniform4i(loc, (GLint)lroundf(val[0]),
                            (GLint)lroundf(ncomp >= 2 ? val[1] : val[0]),
                            (GLint)lroundf(ncomp >= 3 ? val[2] : val[0]),
                            (GLint)lroundf(ncomp >= 4 ? val[3] : 1.0f));
            } else if (strcmp(um->type, "vec2") == 0 || strcmp(um->type, "float2") == 0 ||
                       strcmp(um->type, "half2") == 0) {
                glUniform2f(loc, val[0], ncomp >= 2 ? val[1] : val[0]);
            } else if (strcmp(um->type, "vec3") == 0 || strcmp(um->type, "float3") == 0 ||
                       strcmp(um->type, "half3") == 0) {
                glUniform3f(loc, val[0], ncomp >= 2 ? val[1] : val[0],
                            ncomp >= 3 ? val[2] : val[0]);
            } else if (strcmp(um->type, "vec4") == 0 || strcmp(um->type, "float4") == 0 ||
                       strcmp(um->type, "half4") == 0) {
                glUniform4f(loc, val[0], ncomp >= 2 ? val[1] : val[0], ncomp >= 3 ? val[2] : val[0],
                            ncomp >= 4 ? val[3] : 1.0f);
            }
        }
    }
}

static void apply_gl_blend_mode(we_blend_mode_t mode)
{
    switch (mode) {
    case WE_BLEND_DISABLED:
        glDisable(GL_BLEND);
        break;
    case WE_BLEND_ADDITIVE:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case WE_BLEND_MULTIPLY:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case WE_BLEND_SCREEN:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case WE_BLEND_NORMAL:
    default:
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    }
}

static void step_object_scripts(we_scene_t *scene, we_scene_object_t *obj, uint32_t norm_w,
                                uint32_t norm_h, float dt_sec, float time_sec, bool cursor_moved,
                                const waywal_shader_frame_params_t *params)
{
    if (!scene || !obj || obj->script_count == 0) {
        return;
    }
    we_script_env_t env;
    build_script_env(scene, norm_w, norm_h, dt_sec, time_sec, params, &env);

    for (uint32_t s = 0; s < obj->script_count; s++) {
        we_obj_script_t *sc = &obj->scripts[s];
        if (!sc->vm) {
            continue;
        }
        bind_project_props_to_script(sc->vm, &scene->project);

        we_script_layer_state_t ls;
        if (cursor_moved) {
            sync_scene_layers_to_vm(scene, sc->vm);
            sync_layer_from_obj(obj, &ls);
            if (we_script_eval_cursor_move(sc->vm, &env, &ls)) {
                sync_obj_from_layer(obj, &ls);
                sync_scene_layers_from_vm(scene, sc->vm);
            }
        }

        sync_scene_layers_to_vm(scene, sc->vm);
        sync_layer_from_obj(obj, &ls);

        float in_val[4] = {0};
        float out_val[4] = {0};
        uint32_t ncomp = 1;
        we_const_val_t *target_cv = NULL;

        switch (sc->target) {
        case WE_SCRIPT_PROP_ORIGIN:
            memcpy(in_val, obj->origin, 3 * sizeof(float));
            ncomp = 3;
            break;
        case WE_SCRIPT_PROP_SCALE:
            memcpy(in_val, obj->scale, 3 * sizeof(float));
            ncomp = 3;
            break;
        case WE_SCRIPT_PROP_ANGLES:
            memcpy(in_val, obj->angles, 3 * sizeof(float));
            ncomp = 3;
            break;
        case WE_SCRIPT_PROP_ALPHA:
            in_val[0] = obj->alpha;
            ncomp = 1;
            break;
        case WE_SCRIPT_PROP_COLOR:
            memcpy(in_val, obj->color, 3 * sizeof(float));
            ncomp = 3;
            break;
        case WE_SCRIPT_PROP_VISIBLE:
            in_val[0] = obj->visible ? 1.0f : 0.0f;
            ncomp = 1;
            break;
        case WE_SCRIPT_PROP_UNIFORM:
            for (uint32_t c = 0; c < obj->base_pass.const_val_count; c++) {
                if (strcasecmp(obj->base_pass.const_vals[c].name, sc->uniform_name) == 0) {
                    target_cv = &obj->base_pass.const_vals[c];
                    break;
                }
            }
            if (target_cv) {
                ncomp = target_cv->num_components > 0 ? target_cv->num_components : 1u;
                memcpy(in_val, target_cv->val, ncomp * sizeof(float));
            }
            break;
        }

        memcpy(out_val, in_val, ncomp * sizeof(float));
        if (we_script_eval_update(sc->vm, &env, &ls, in_val, ncomp, out_val)) {
            sync_obj_from_layer(obj, &ls);
            sync_scene_layers_from_vm(scene, sc->vm);
            if (sc->target == WE_SCRIPT_PROP_UNIFORM ||
                memcmp(out_val, in_val, ncomp * sizeof(float)) != 0) {
                switch (sc->target) {
                case WE_SCRIPT_PROP_ORIGIN:
                    memcpy(obj->origin, out_val, 3 * sizeof(float));
                    break;
                case WE_SCRIPT_PROP_SCALE:
                    memcpy(obj->scale, out_val, 3 * sizeof(float));
                    break;
                case WE_SCRIPT_PROP_ANGLES:
                    memcpy(obj->angles, out_val, 3 * sizeof(float));
                    break;
                case WE_SCRIPT_PROP_ALPHA:
                    obj->alpha = out_val[0];
                    break;
                case WE_SCRIPT_PROP_COLOR:
                    memcpy(obj->color, out_val, 3 * sizeof(float));
                    break;
                case WE_SCRIPT_PROP_VISIBLE:
                    obj->visible = (out_val[0] != 0.0f);
                    break;
                case WE_SCRIPT_PROP_UNIFORM:
                    if (target_cv) {
                        memcpy(target_cv->val, out_val, ncomp * sizeof(float));
                    }
                    break;
                }
            }
        }
    }
}

static void render_scene_to_fbo(we_scene_t *scene, uint32_t target_w, uint32_t target_h,
                                const waywal_shader_frame_params_t *params)
{
    scene->frame_counter++;
    if (scene->frame_counter == 0) {
        scene->frame_counter = 1;
    }

    uint32_t sw = (uint32_t)(scene->ortho_width > 1.0f ? scene->ortho_width : (float)target_w);
    uint32_t sh = (uint32_t)(scene->ortho_height > 1.0f ? scene->ortho_height : (float)target_h);
    if (sw == 0) {
        sw = target_w > 0 ? target_w : 1920;
    }
    if (sh == 0) {
        sh = target_h > 0 ? target_h : 1080;
    }
    uint32_t norm_w = target_w > 0 ? target_w : sw;
    uint32_t norm_h = target_h > 0 ? target_h : sh;

    float dt_sec =
        (params && params->time_delta_sec > 0.0f) ? params->time_delta_sec : (1.0f / 60.0f);
    if (scene->first_frame_done) {
        scene->sim_time += dt_sec;
    }
    float time_sec = (params && params->time_sec > 0.0f) ? params->time_sec : scene->sim_time;

    float cur_ptr[2] = {0.5f, 0.5f};
    if (params && norm_w > 0 && norm_h > 0 &&
        (params->mouse[0] > 0.0f || params->mouse[1] > 0.0f)) {
        cur_ptr[0] = params->mouse[0] / (float)norm_w;
        cur_ptr[1] = params->mouse[1] / (float)norm_h;
    }
    bool cursor_moved =
        scene->first_frame_done && (fabsf(cur_ptr[0] - scene->last_pointer[0]) > 1e-5f ||
                                    fabsf(cur_ptr[1] - scene->last_pointer[1]) > 1e-5f);
    scene->first_frame_done = true;

    ensure_fbo_pair(&scene->scene_fbo, &scene->scene_fbo_tex, &scene->scene_fbo_w,
                    &scene->scene_fbo_h, sw, sh);

    glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
    glViewport(0, 0, (GLsizei)sw, (GLsizei)sh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    if (scene->clear_enabled) {
        glClearColor(scene->clear_color[0], scene->clear_color[1], scene->clear_color[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    /* Compute camera parallax & shake offsets normalized by target_w/target_h */
    float pdx = 0.0f, pdy = 0.0f;
    if (scene->camera_parallax && params && norm_w > 0 && norm_h > 0) {
        float mx = (params->mouse[0] / (float)norm_w) * 2.0f - 1.0f;
        float my = (params->mouse[1] / (float)norm_h) * 2.0f - 1.0f;
        float target_px =
            mx * scene->camera_parallax_amount * scene->camera_parallax_mouse_influence * 25.0f;
        float target_py =
            my * scene->camera_parallax_amount * scene->camera_parallax_mouse_influence * 25.0f;
        float alpha = scene->camera_parallax_delay > 0.001f ? 0.15f : 1.0f;
        scene->smooth_parallax[0] += (target_px - scene->smooth_parallax[0]) * alpha;
        scene->smooth_parallax[1] += (target_py - scene->smooth_parallax[1]) * alpha;
        pdx = scene->smooth_parallax[0];
        pdy = scene->smooth_parallax[1];
    }

    float shake_x = 0.0f, shake_y = 0.0f;
    if (scene->camera_shake && params) {
        float t = time_sec * scene->camera_shake_speed;
        shake_x = sinf(t * 1.7f) * scene->camera_shake_amp * 4.0f;
        shake_y = cosf(t * 2.3f) * scene->camera_shake_amp * 4.0f;
    }

    float vp[16];
    mat4_ortho_topdown((float)sw, (float)sh, shake_x, shake_y, vp);

    glBindVertexArray(scene->quad_vao);

    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];

        /* Refresh dynamic user-bound properties */
        if (obj->visible_user_prop[0] != '\0') {
            (void)we_project_eval_prop_bool(&scene->project, obj->visible_user_prop, &obj->visible);
        }
        if (obj->alpha_user_prop[0] != '\0') {
            const we_user_prop_t *up = project_find_prop(&scene->project, obj->alpha_user_prop);
            if (up && up->num_components > 0) {
                obj->alpha = up->val[0];
            }
        }
        if (obj->color_user_prop[0] != '\0') {
            const we_user_prop_t *up = project_find_prop(&scene->project, obj->color_user_prop);
            if (up && up->num_components >= 3) {
                obj->color[0] = up->val[0];
                obj->color[1] = up->val[1];
                obj->color[2] = up->val[2];
            }
        }
    }

    /* Evaluate SceneScript update / cursorMove callbacks across all objects before rendering */
    for (uint32_t i = 0; i < scene->object_count; i++) {
        step_object_scripts(scene, &scene->objects[i], norm_w, norm_h, dt_sec, time_sec,
                            cursor_moved, params);
    }

    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];

        if (!obj->visible || obj->alpha <= 0.0001f) {
            continue;
        }

        /* Particle system simulation & batched rendering */
        if (obj->particle_sys) {
            float world_node[16], mvp[16];
            compute_object_node_world_matrix(scene, obj, pdx, pdy, world_node);
            mat4_mul(vp, world_node, mvp);

            float dx = cur_ptr[0] * (float)sw - world_node[12];
            float dy = (1.0f - cur_ptr[1]) * (float)sh - world_node[13];
            float m00 = world_node[0], m01 = world_node[4];
            float m10 = world_node[1], m11 = world_node[5];
            float det = m00 * m11 - m01 * m10;
            float lx = dx, ly = dy;
            if (fabsf(det) > 1e-8f) {
                float inv_det = 1.0f / det;
                lx = (m11 * dx - m01 * dy) * inv_det;
                ly = (-m10 * dx + m00 * dy) * inv_det;
            }
            we_particle_set_pointer(obj->particle_sys, lx, ly, 0.0f);
            we_particle_update(obj->particle_sys, dt_sec);

            if (scene->particle_program != 0) {
                glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
                glViewport(0, 0, (GLsizei)sw, (GLsizei)sh);
                apply_gl_blend_mode(obj->base_pass.blending);
                glUseProgram(scene->particle_program);

                if (scene->part_mvp_loc >= 0) {
                    glUniformMatrix4fv(scene->part_mvp_loc, 1, GL_FALSE, mvp);
                }
                if (scene->part_color4_loc >= 0) {
                    glUniform4f(scene->part_color4_loc, obj->color[0] * obj->brightness,
                                obj->color[1] * obj->brightness, obj->color[2] * obj->brightness,
                                obj->alpha);
                }
                GLuint ptex = scene->tex_util_white;
                float trans[2] = {0.0f, 0.0f};
                float rot[4] = {1.0f, 0.0f, 0.0f, 1.0f};
                if (obj->base_pass.textures[0] && obj->base_pass.textures[0]->gl_tex != 0) {
                    we_tex_t *wt = obj->base_pass.textures[0];
                    bool should_advance = (wt->last_advance_frame != scene->frame_counter);
                    wt->last_advance_frame = scene->frame_counter;
                    if (wt->is_video_mp4 && should_advance) {
                        (void)we_tex_advance_video(wt, dt_sec);
                    }
                    if ((wt->flags & WE_TEX_FLAG_IS_GIF) != 0 && wt->frame_count > 0) {
                        we_tex_advance_spritesheet(wt, should_advance ? dt_sec : 0.0f, trans, rot);
                    }
                    ptex = wt->gl_tex;
                }
                if (scene->part_trans_loc >= 0) {
                    glUniform2fv(scene->part_trans_loc, 1, trans);
                }
                if (scene->part_rot_loc >= 0) {
                    glUniform4fv(scene->part_rot_loc, 1, rot);
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, ptex);
                if (scene->part_tex0_loc >= 0) {
                    glUniform1i(scene->part_tex0_loc, 0);
                }
                we_particle_draw_gl(obj->particle_sys, scene->particle_program);
                glBindVertexArray(scene->quad_vao);
            }
            continue;
        }

        if (!obj->has_base_pass || obj->base_pass.gl_prog == 0) {
            continue;
        }

        /* Count active effects */
        uint32_t active_effs = 0;
        for (uint32_t e = 0; e < obj->effect_count; e++) {
            if (obj->effects[e].visible_user_prop[0] != '\0') {
                (void)we_project_eval_prop_bool(&scene->project, obj->effects[e].visible_user_prop,
                                                &obj->effects[e].visible);
            }
            if (obj->effects[e].visible && obj->effects[e].pass_count > 0) {
                active_effs++;
            }
        }

        if (obj->needs_full_fb_copy) {
            ensure_fbo_pair(&scene->full_fb_fbo, &scene->full_fb_tex, &scene->full_fb_w,
                            &scene->full_fb_h, sw, sh);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->scene_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->full_fb_fbo);
            glBlitFramebuffer(0, 0, (GLint)sw, (GLint)sh, 0, 0, (GLint)sw, (GLint)sh,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }

        if (active_effs == 0) {
            /* Fast path: render object directly into scene_fbo */
            float model[16], mvp[16];
            if (obj->puppet) {
                compute_object_node_world_matrix(scene, obj, pdx, pdy, model);
            } else {
                compute_object_model_matrix(scene, obj, pdx, pdy, model);
            }
            mat4_mul(vp, model, mvp);

            glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
            glViewport(0, 0, (GLsizei)sw, (GLsizei)sh);
            apply_gl_blend_mode(obj->base_pass.blending);
            bind_pass_uniforms(scene, obj, NULL, &obj->base_pass, model, vp, mvp, 0, sw, sh, norm_w,
                               norm_h, params);
            if (obj->puppet) {
                we_puppet_draw_gl(obj->puppet, obj->base_pass.gl_prog, (double)time_sec);
                glBindVertexArray(scene->quad_vao);
            } else {
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            }
        } else {
            /* Ping-pong effect chain path */
            uint32_t pw = (uint32_t)(obj->size[0] > 1.0f ? obj->size[0] : (float)sw);
            uint32_t ph = (uint32_t)(obj->size[1] > 1.0f ? obj->size[1] : (float)sh);
            if (obj->fullscreen) {
                pw = sw;
                ph = sh;
            }
            ensure_fbo_pair(&obj->ping_fbo[0], &obj->ping_tex[0], &obj->ping_w, &obj->ping_h, pw,
                            ph);
            uint32_t dummy_w = obj->ping_w, dummy_h = obj->ping_h;
            if (!obj->ping_fbo[1]) {
                dummy_w = 0;
            }
            ensure_fbo_pair(&obj->ping_fbo[1], &obj->ping_tex[1], &dummy_w, &dummy_h, pw, ph);

            uint32_t cur_idx = 0;
            float ident[16];
            mat4_identity(ident);

            /* Seed ping_fbo[0] by drawing obj->base_pass so compressed S3TC/BC7 textures,
             * swizzles, and spritesheets render cleanly into ping_fbo[0] */
            glBindFramebuffer(GL_FRAMEBUFFER, obj->ping_fbo[0]);
            glViewport(0, 0, (GLsizei)pw, (GLsizei)ph);
            glDisable(GL_BLEND);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (obj->puppet) {
                float p_mvp[16];
                mat4_identity(p_mvp);
                p_mvp[0] = 2.0f / (float)pw;
                p_mvp[5] = -2.0f / (float)ph;
                p_mvp[10] = -1.0f / 2000.0f;
                bind_pass_uniforms(scene, obj, NULL, &obj->base_pass, ident, p_mvp, p_mvp, 0, pw,
                                   ph, norm_w, norm_h, params);
                we_puppet_draw_gl(obj->puppet, obj->base_pass.gl_prog, (double)time_sec);
                glBindVertexArray(scene->quad_vao);
            } else {
                bind_pass_uniforms(scene, obj, NULL, &obj->base_pass, ident, ident, ident, 0, pw,
                                   ph, norm_w, norm_h, params);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            }
            GLuint cur_tex = obj->ping_tex[0];

            for (uint32_t e = 0; e < obj->effect_count; e++) {
                we_effect_t *eff = &obj->effects[e];
                if (!eff->visible) {
                    continue;
                }
                for (uint32_t f = 0; f < eff->fbo_count; f++) {
                    uint32_t sc = eff->fbos[f].scale > 0 ? eff->fbos[f].scale : 1;
                    uint32_t fw = pw / sc > 0 ? pw / sc : 1;
                    uint32_t fh = ph / sc > 0 ? ph / sc : 1;
                    ensure_fbo_pair(&eff->fbos[f].gl_fbo, &eff->fbos[f].gl_tex, &eff->fbos[f].width,
                                    &eff->fbos[f].height, fw, fh);
                }
                for (uint32_t p = 0; p < eff->pass_count; p++) {
                    we_material_pass_t *ep = &eff->passes[p];
                    if (strcasecmp(ep->command, "swap") == 0) {
                        cur_idx ^= 1u;
                        cur_tex = obj->ping_tex[cur_idx];
                        continue;
                    }
                    if (strcasecmp(ep->command, "copy") == 0) {
                        if (ep->target_fbo[0] != '\0') {
                            we_rt_fbo_t *dst_rt = NULL;
                            for (uint32_t f = 0; f < eff->fbo_count; f++) {
                                if (strcasecmp(eff->fbos[f].name, ep->target_fbo) == 0 &&
                                    eff->fbos[f].gl_fbo) {
                                    dst_rt = &eff->fbos[f];
                                    break;
                                }
                            }
                            if (!dst_rt) {
                                dst_rt = get_or_create_scene_rt_fbo(scene, ep->target_fbo, pw, ph);
                            }
                            if (dst_rt && dst_rt->gl_fbo) {
                                glBindFramebuffer(GL_READ_FRAMEBUFFER, obj->ping_fbo[cur_idx]);
                                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_rt->gl_fbo);
                                glBlitFramebuffer(0, 0, (GLint)pw, (GLint)ph, 0, 0,
                                                  (GLint)dst_rt->width, (GLint)dst_rt->height,
                                                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
                            }
                        }
                        continue;
                    }
                    if (!ep->gl_prog) {
                        continue;
                    }

                    uint32_t next_idx = cur_idx ^ 1u;
                    GLuint dst_fbo = obj->ping_fbo[next_idx];
                    GLuint dst_tex = obj->ping_tex[next_idx];
                    uint32_t dst_w = pw, dst_h = ph;
                    bool wrote_ping = true;

                    if (ep->target_fbo[0] != '\0') {
                        we_rt_fbo_t *dst_rt = NULL;
                        for (uint32_t f = 0; f < eff->fbo_count; f++) {
                            if (strcasecmp(eff->fbos[f].name, ep->target_fbo) == 0 &&
                                eff->fbos[f].gl_fbo) {
                                dst_rt = &eff->fbos[f];
                                break;
                            }
                        }
                        if (!dst_rt) {
                            dst_rt = get_or_create_scene_rt_fbo(scene, ep->target_fbo, pw, ph);
                        }
                        if (dst_rt && dst_rt->gl_fbo) {
                            dst_fbo = dst_rt->gl_fbo;
                            dst_tex = dst_rt->gl_tex;
                            dst_w = dst_rt->width;
                            dst_h = dst_rt->height;
                            wrote_ping = false;
                        }
                    }

                    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
                    glViewport(0, 0, (GLsizei)dst_w, (GLsizei)dst_h);
                    apply_gl_blend_mode(ep->blending);
                    bind_pass_uniforms(scene, obj, eff, ep, ident, ident, ident, cur_tex, dst_w,
                                       dst_h, norm_w, norm_h, params);
                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

                    if (wrote_ping) {
                        cur_idx = next_idx;
                        cur_tex = dst_tex;
                    }
                }
            }

            if (obj->id >= 0) {
                char comp_name[64];
                snprintf(comp_name, sizeof(comp_name), "_rt_imageLayerComposite_%d_a", obj->id);
                we_rt_fbo_t *comp_rt = get_or_create_scene_rt_fbo(scene, comp_name, pw, ph);
                if (comp_rt && comp_rt->gl_fbo) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, obj->ping_fbo[cur_idx]);
                    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, comp_rt->gl_fbo);
                    glBlitFramebuffer(0, 0, (GLint)pw, (GLint)ph, 0, 0, (GLint)comp_rt->width,
                                      (GLint)comp_rt->height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
                }
            }

            /* Final object composite pass into scene_fbo using default_program */
            float model[16], mvp[16];
            compute_object_model_matrix(scene, obj, pdx, pdy, model);
            mat4_mul(vp, model, mvp);

            glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
            glViewport(0, 0, (GLsizei)sw, (GLsizei)sh);
            apply_gl_blend_mode(obj->base_pass.blending);
            if (scene->default_program != 0) {
                glUseProgram(scene->default_program);
                if (scene->default_prog_ulocs.mvp >= 0) {
                    glUniformMatrix4fv(scene->default_prog_ulocs.mvp, 1, GL_FALSE, mvp);
                }
                if (scene->default_prog_ulocs.tex_sampler[0] >= 0) {
                    glUniform1i(scene->default_prog_ulocs.tex_sampler[0], 0);
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, cur_tex);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            } else {
                bind_pass_uniforms(scene, obj, NULL, &obj->base_pass, model, vp, mvp, cur_tex, sw,
                                   sh, norm_w, norm_h, params);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, cur_tex);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            }

            static const GLenum k_color_attachment = GL_COLOR_ATTACHMENT0;
            for (int k = 0; k < 2; k++) {
                if (obj->ping_fbo[k] != 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, obj->ping_fbo[k]);
                    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, &k_color_attachment);
                }
            }
            glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
        }
    }

    scene->last_pointer[0] = cur_ptr[0];
    scene->last_pointer[1] = cur_ptr[1];
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(0);
}

static bool import_scene_bo_tex(we_scene_t *scene, dmabuf_bo_t *bo, GLuint *out_tex)
{
    if (!scene || !bo || !out_tex) {
        return false;
    }

    for (size_t i = 0; i < scene->num_imported_bos; i++) {
        if (scene->imported_bos[i].bo == bo && scene->imported_bos[i].gl_tex != 0 &&
            scene->imported_bos[i].egl_image != NULL) {
            *out_tex = scene->imported_bos[i].gl_tex;
            return true;
        }
    }

    ensure_egl_procs(scene);
    PFNEGLCREATEIMAGEKHRPROC create_img =
        __extension__(PFNEGLCREATEIMAGEKHRPROC) scene->fn_eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC destroy_img =
        __extension__(PFNEGLDESTROYIMAGEKHRPROC) scene->fn_eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC target_tex2d =
        __extension__(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC) scene->fn_glEGLImageTargetTexture2DOES;
    PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC target_storage =
        __extension__(PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC) scene->fn_glEGLImageTargetTexStorageEXT;

    if (!create_img || !target_tex2d) {
        return false;
    }

    static const EGLint plane_fd_keys[4] = {
        EGL_DMA_BUF_PLANE0_FD_EXT,
        EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT,
        EGL_DMA_BUF_PLANE3_FD_EXT,
    };
    static const EGLint plane_offset_keys[4] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT,
        EGL_DMA_BUF_PLANE3_OFFSET_EXT,
    };
    static const EGLint plane_pitch_keys[4] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT,
        EGL_DMA_BUF_PLANE3_PITCH_EXT,
    };
    static const EGLint plane_mod_lo_keys[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
    };
    static const EGLint plane_mod_hi_keys[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
    };

    uint32_t num_planes = bo->num_planes > 0 ? bo->num_planes : 1u;
    if (num_planes > 4u) {
        num_planes = 4u;
    }

    EGLint attribs[64];
    int n = 0;
    attribs[n++] = EGL_WIDTH;
    attribs[n++] = (EGLint)bo->width;
    attribs[n++] = EGL_HEIGHT;
    attribs[n++] = (EGLint)bo->height;
    attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[n++] = (EGLint)bo->drm_format;

    for (uint32_t p = 0; p < num_planes; p++) {
        if (bo->fd[p] < 0) {
            continue;
        }
        attribs[n++] = plane_fd_keys[p];
        attribs[n++] = bo->fd[p];
        attribs[n++] = plane_offset_keys[p];
        attribs[n++] = (EGLint)bo->offset[p];
        attribs[n++] = plane_pitch_keys[p];
        attribs[n++] = (EGLint)bo->stride[p];
        if (bo->modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[n++] = plane_mod_lo_keys[p];
            attribs[n++] = (EGLint)(bo->modifier & 0xFFFFFFFFu);
            attribs[n++] = plane_mod_hi_keys[p];
            attribs[n++] = (EGLint)((bo->modifier >> 32u) & 0xFFFFFFFFu);
        }
    }
    attribs[n] = EGL_NONE;

    EGLImageKHR img =
        create_img(scene->egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (img == EGL_NO_IMAGE_KHR) {
        return false;
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (target_storage) {
        target_storage(GL_TEXTURE_2D, (GLeglImageOES)img, NULL);
    } else {
        target_tex2d(GL_TEXTURE_2D, img);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Track imported textures per scene rather than mutating bo->gl_tex across EGL contexts */
    size_t slot = scene->num_imported_bos;
    for (size_t i = 0; i < scene->num_imported_bos; i++) {
        if (scene->imported_bos[i].bo == bo) {
            if (scene->imported_bos[i].owns_egl) {
                if (scene->imported_bos[i].gl_tex && scene->imported_bos[i].gl_tex != tex) {
                    glDeleteTextures(1, &scene->imported_bos[i].gl_tex);
                }
                if (scene->imported_bos[i].egl_image && scene->imported_bos[i].egl_image != img &&
                    destroy_img) {
                    destroy_img(scene->egl_display, scene->imported_bos[i].egl_image);
                }
            }
            slot = i;
            break;
        }
    }
    if (slot >= 32u) {
        if (scene->imported_bos[0].owns_egl) {
            if (scene->imported_bos[0].gl_tex) {
                GLuint del_tex = scene->imported_bos[0].gl_tex;
                glDeleteTextures(1, &del_tex);
            }
            if (scene->imported_bos[0].egl_image && destroy_img) {
                void *del_img = scene->imported_bos[0].egl_image;
                destroy_img(scene->egl_display, del_img);
            }
        }
        for (size_t j = 1; j < 32u; j++) {
            scene->imported_bos[j - 1] = scene->imported_bos[j];
        }
        scene->num_imported_bos = 31u;
        slot = 31u;
    }

    scene->imported_bos[slot].bo = bo;
    scene->imported_bos[slot].gl_tex = tex;
    scene->imported_bos[slot].egl_image = img;
    scene->imported_bos[slot].owns_egl = true;
    if (slot == scene->num_imported_bos) {
        scene->num_imported_bos++;
    }
    *out_tex = tex;
    return true;
}

bool we_scene_render_frame(we_scene_t *scene, dmabuf_bo_t *target_bo,
                           const waywal_shader_frame_params_t *params)
{
    if (!scene || !target_bo || target_bo->width == 0 || target_bo->height == 0) {
        return false;
    }
    if (!scene->gl_initialized) {
        return false;
    }

    if (scene->egl_display && scene->egl_context && eglGetCurrentContext() != scene->egl_context) {
        eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, scene->egl_context);
    }

    GLuint bo_gl_tex = 0;
    if (!import_scene_bo_tex(scene, target_bo, &bo_gl_tex) || bo_gl_tex == 0) {
        return false;
    }

    render_scene_to_fbo(scene, target_bo->width, target_bo->height, params);

    /* Blit scene_fbo_tex into bo_gl_tex with opaque alpha 1.0 so transparent shaders do not render
     * black on Wayland */
    glBindFramebuffer(GL_FRAMEBUFFER, scene->target_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bo_gl_tex, 0);
    glViewport(0, 0, (GLsizei)target_bo->width, (GLsizei)target_bo->height);
    glDisable(GL_BLEND);

    glUseProgram(scene->prog_present);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene->scene_fbo_tex);
    if (scene->present_u_tex_loc >= 0) {
        glUniform1i(scene->present_u_tex_loc, 0);
    }
    glBindVertexArray(scene->quad_vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    /* Synchronize GPU completion before returning the DMA-BUF to Wayland */
    ensure_egl_procs(scene);
    PFNEGLCREATESYNCKHRPROC create_sync =
        __extension__(PFNEGLCREATESYNCKHRPROC) scene->fn_eglCreateSyncKHR;
    PFNEGLCLIENTWAITSYNCKHRPROC wait_sync =
        __extension__(PFNEGLCLIENTWAITSYNCKHRPROC) scene->fn_eglClientWaitSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync =
        __extension__(PFNEGLDESTROYSYNCKHRPROC) scene->fn_eglDestroySyncKHR;

    if (create_sync && wait_sync && destroy_sync && scene->egl_display != EGL_NO_DISPLAY) {
        EGLSyncKHR sync = create_sync(scene->egl_display, EGL_SYNC_FENCE_KHR, NULL);
        if (sync != EGL_NO_SYNC_KHR) {
            wait_sync(scene->egl_display, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 100000000ULL);
            destroy_sync(scene->egl_display, sync);
        } else {
            glFinish();
        }
    } else {
        glFinish();
    }

    return true;
}

static void cpu_blend_pixel(uint8_t *dp, float sr, float sg, float sb, float sa,
                            we_blend_mode_t blend)
{
    if (sa <= 0.0001f) {
        return;
    }
    float dr = dp[0] / 255.0f;
    float dg = dp[1] / 255.0f;
    float db = dp[2] / 255.0f;

    float or_val = dr, og_val = dg, ob_val = db;
    if (blend == WE_BLEND_ADDITIVE) {
        or_val = dr + sr * sa;
        og_val = dg + sg * sa;
        ob_val = db + sb * sa;
    } else if (blend == WE_BLEND_MULTIPLY) {
        or_val = dr * (1.0f - sa) + (dr * sr) * sa;
        og_val = dg * (1.0f - sa) + (dg * sg) * sa;
        ob_val = db * (1.0f - sa) + (db * sb) * sa;
    } else if (blend == WE_BLEND_SCREEN) {
        or_val = 1.0f - (1.0f - dr) * (1.0f - sr * sa);
        og_val = 1.0f - (1.0f - dg) * (1.0f - sg * sa);
        ob_val = 1.0f - (1.0f - db) * (1.0f - sb * sa);
    } else if (blend == WE_BLEND_DISABLED) {
        or_val = sr;
        og_val = sg;
        ob_val = sb;
    } else {
        or_val = dr * (1.0f - sa) + sr * sa;
        og_val = dg * (1.0f - sa) + sg * sa;
        ob_val = db * (1.0f - sa) + sb * sa;
    }

    dp[0] = (uint8_t)(fminf(fmaxf(or_val, 0.0f), 1.0f) * 255.0f + 0.5f);
    dp[1] = (uint8_t)(fminf(fmaxf(og_val, 0.0f), 1.0f) * 255.0f + 0.5f);
    dp[2] = (uint8_t)(fminf(fmaxf(ob_val, 0.0f), 1.0f) * 255.0f + 0.5f);
    dp[3] = 255;
}

static void cpu_rasterize_triangle(uint8_t *out_rgba, uint32_t width, uint32_t height,
                                   float ortho_w, float ortho_h, const float model[16],
                                   const float p0[3], const float uv0[2], const float c0[4],
                                   const float p1[3], const float uv1[2], const float c1[4],
                                   const float p2[3], const float uv2[2], const float c2[4],
                                   const uint8_t *src_rgba, uint32_t tw, uint32_t th, float r_mul,
                                   float g_mul, float b_mul, float alpha, we_blend_mode_t blend)
{
    float wx0 = model[0] * p0[0] + model[4] * p0[1] + model[12];
    float wy0 = model[1] * p0[0] + model[5] * p0[1] + model[13];
    float wx1 = model[0] * p1[0] + model[4] * p1[1] + model[12];
    float wy1 = model[1] * p1[0] + model[5] * p1[1] + model[13];
    float wx2 = model[0] * p2[0] + model[4] * p2[1] + model[12];
    float wy2 = model[1] * p2[0] + model[5] * p2[1] + model[13];

    float sx0 = (wx0 / ortho_w) * (float)width;
    float sy0 = (1.0f - wy0 / ortho_h) * (float)height;
    float sx1 = (wx1 / ortho_w) * (float)width;
    float sy1 = (1.0f - wy1 / ortho_h) * (float)height;
    float sx2 = (wx2 / ortho_w) * (float)width;
    float sy2 = (1.0f - wy2 / ortho_h) * (float)height;

    float min_x = fminf(sx0, fminf(sx1, sx2));
    float max_x = fmaxf(sx0, fmaxf(sx1, sx2));
    float min_y = fminf(sy0, fminf(sy1, sy2));
    float max_y = fmaxf(sy0, fmaxf(sy1, sy2));

    int32_t x0 = (int32_t)floorf(min_x);
    int32_t x1 = (int32_t)ceilf(max_x);
    int32_t y0 = (int32_t)floorf(min_y);
    int32_t y1 = (int32_t)ceilf(max_y);
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (int32_t)width) {
        x1 = (int32_t)width;
    }
    if (y1 > (int32_t)height) {
        y1 = (int32_t)height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    float area = (sx1 - sx0) * (sy2 - sy0) - (sy1 - sy0) * (sx2 - sx0);
    if (fabsf(area) < 1e-6f) {
        return;
    }
    float inv_area = 1.0f / area;

    for (int32_t y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        for (int32_t x = x0; x < x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = ((sx1 - px) * (sy2 - py) - (sy1 - py) * (sx2 - px)) * inv_area;
            float w1 = ((sx2 - px) * (sy0 - py) - (sy2 - py) * (sx0 - px)) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) {
                continue;
            }
            float u = fminf(fmaxf(w0 * uv0[0] + w1 * uv1[0] + w2 * uv2[0], 0.0f), 0.999999f);
            float v = fminf(fmaxf(w0 * uv0[1] + w1 * uv1[1] + w2 * uv2[1], 0.0f), 0.999999f);
            uint32_t tx_i = (uint32_t)(u * (float)tw);
            uint32_t ty_i = (uint32_t)(v * (float)th);
            const uint8_t *sp = &src_rgba[(ty_i * tw + tx_i) * 4];

            float vc_r = w0 * c0[0] + w1 * c1[0] + w2 * c2[0];
            float vc_g = w0 * c0[1] + w1 * c1[1] + w2 * c2[1];
            float vc_b = w0 * c0[2] + w1 * c1[2] + w2 * c2[2];
            float vc_a = w0 * c0[3] + w1 * c1[3] + w2 * c2[3];

            float sr = (sp[0] / 255.0f) * vc_r * r_mul;
            float sg = (sp[1] / 255.0f) * vc_g * g_mul;
            float sb = (sp[2] / 255.0f) * vc_b * b_mul;
            float sa = (sp[3] / 255.0f) * vc_a * alpha;

            cpu_blend_pixel(&out_rgba[(y * (int32_t)width + x) * 4], sr, sg, sb, sa, blend);
        }
    }
}

bool we_scene_render_rgba8(we_scene_t *scene, uint32_t width, uint32_t height,
                           const waywal_shader_frame_params_t *params, uint8_t *out_rgba)
{
    if (!scene || width == 0 || height == 0 || !out_rgba) {
        return false;
    }

    if (scene->gl_initialized) {
        if (scene->egl_display && scene->egl_context &&
            eglGetCurrentContext() != scene->egl_context) {
            eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, scene->egl_context);
        }
        render_scene_to_fbo(scene, width, height, params);
        if (scene->scene_fbo_w == width && scene->scene_fbo_h == height) {
            glBindFramebuffer(GL_FRAMEBUFFER, scene->scene_fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE,
                         out_rgba);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            /* Blit scene->scene_fbo into scene->target_fbo when requested dimensions differ */
            GLuint tmp_tex = 0;
            glGenTextures(1, &tmp_tex);
            glBindTexture(GL_TEXTURE_2D, tmp_tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, NULL);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene->target_fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   tmp_tex, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, scene->scene_fbo);
            glBlitFramebuffer(0, 0, (GLint)scene->scene_fbo_w, (GLint)scene->scene_fbo_h, 0, 0,
                              (GLint)width, (GLint)height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, scene->target_fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE,
                         out_rgba);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteTextures(1, &tmp_tex);
        }
        return true;
    }

    /* Software CPU compositor fallback when running headless without an EGL/GLES context */
    scene->frame_counter++;
    if (scene->frame_counter == 0) {
        scene->frame_counter = 1;
    }

    uint8_t cr = (uint8_t)(fminf(fmaxf(scene->clear_color[0], 0.0f), 1.0f) * 255.0f + 0.5f);
    uint8_t cg = (uint8_t)(fminf(fmaxf(scene->clear_color[1], 0.0f), 1.0f) * 255.0f + 0.5f);
    uint8_t cb = (uint8_t)(fminf(fmaxf(scene->clear_color[2], 0.0f), 1.0f) * 255.0f + 0.5f);

    size_t npx = (size_t)width * (size_t)height;
    for (size_t i = 0; i < npx; i++) {
        out_rgba[i * 4 + 0] = cr;
        out_rgba[i * 4 + 1] = cg;
        out_rgba[i * 4 + 2] = cb;
        out_rgba[i * 4 + 3] = 255;
    }

    float ortho_w = scene->ortho_width > 1.0f ? scene->ortho_width : 1920.0f;
    float ortho_h = scene->ortho_height > 1.0f ? scene->ortho_height : 1080.0f;

    float dt_sec =
        (params && params->time_delta_sec > 0.0f) ? params->time_delta_sec : (1.0f / 60.0f);
    if (scene->first_frame_done) {
        scene->sim_time += dt_sec;
    }
    float time_sec = (params && params->time_sec > 0.0f) ? params->time_sec : scene->sim_time;

    float cur_ptr[2] = {0.5f, 0.5f};
    if (params && width > 0 && height > 0 && (params->mouse[0] > 0.0f || params->mouse[1] > 0.0f)) {
        cur_ptr[0] = params->mouse[0] / (float)width;
        cur_ptr[1] = params->mouse[1] / (float)height;
    }
    bool cursor_moved =
        scene->first_frame_done && (fabsf(cur_ptr[0] - scene->last_pointer[0]) > 1e-5f ||
                                    fabsf(cur_ptr[1] - scene->last_pointer[1]) > 1e-5f);
    scene->first_frame_done = true;

    static const uint8_t white_px[4] = {255, 255, 255, 255};
    static const float white_c4[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];
        if (obj->visible_user_prop[0] != '\0') {
            (void)we_project_eval_prop_bool(&scene->project, obj->visible_user_prop, &obj->visible);
        }
        if (obj->alpha_user_prop[0] != '\0') {
            const we_user_prop_t *up = project_find_prop(&scene->project, obj->alpha_user_prop);
            if (up && up->num_components > 0) {
                obj->alpha = up->val[0];
            }
        }
        if (obj->color_user_prop[0] != '\0') {
            const we_user_prop_t *up = project_find_prop(&scene->project, obj->color_user_prop);
            if (up && up->num_components >= 3) {
                obj->color[0] = up->val[0];
                obj->color[1] = up->val[1];
                obj->color[2] = up->val[2];
            }
        }
    }

    for (uint32_t i = 0; i < scene->object_count; i++) {
        step_object_scripts(scene, &scene->objects[i], width, height, dt_sec, time_sec,
                            cursor_moved, params);
    }

    for (uint32_t i = 0; i < scene->object_count; i++) {
        we_scene_object_t *obj = &scene->objects[i];

        if (!obj->visible || obj->alpha <= 0.0001f) {
            continue;
        }

        float alpha = fminf(fmaxf(obj->alpha, 0.0f), 1.0f);
        float r_mul = fminf(fmaxf(obj->color[0] * obj->brightness, 0.0f), 1.0f);
        float g_mul = fminf(fmaxf(obj->color[1] * obj->brightness, 0.0f), 1.0f);
        float b_mul = fminf(fmaxf(obj->color[2] * obj->brightness, 0.0f), 1.0f);

        uint8_t *decoded_rgba = NULL;
        const uint8_t *src_rgba = white_px;
        uint32_t tw = 1, th = 1;
        we_tex_t *tex0 = obj->base_pass.textures[0];
        bool should_advance_tex0 = false;
        if (tex0) {
            should_advance_tex0 = (tex0->last_advance_frame != scene->frame_counter);
            tex0->last_advance_frame = scene->frame_counter;
        }
        if (tex0 && tex0->is_video_mp4 && should_advance_tex0) {
            (void)we_tex_advance_video(tex0, dt_sec);
        }
        if (tex0 && we_tex_decode_rgba8(tex0, &decoded_rgba, &tw, &th) && decoded_rgba && tw > 0 &&
            th > 0) {
            src_rgba = decoded_rgba;
        }

        if (obj->particle_sys) {
            float world_node[16];
            compute_object_node_world_matrix(scene, obj, 0.0f, 0.0f, world_node);
            float dx = cur_ptr[0] * ortho_w - world_node[12];
            float dy = (1.0f - cur_ptr[1]) * ortho_h - world_node[13];
            float m00 = world_node[0], m01 = world_node[4];
            float m10 = world_node[1], m11 = world_node[5];
            float det = m00 * m11 - m01 * m10;
            float lx = dx, ly = dy;
            if (fabsf(det) > 1e-8f) {
                float inv_det = 1.0f / det;
                lx = (m11 * dx - m01 * dy) * inv_det;
                ly = (-m10 * dx + m00 * dy) * inv_det;
            }
            we_particle_set_pointer(obj->particle_sys, lx, ly, 0.0f);
            we_particle_update(obj->particle_sys, dt_sec);

            float trans[2] = {0.0f, 0.0f};
            float rot[4] = {1.0f, 0.0f, 0.0f, 1.0f};
            if (tex0 && (tex0->flags & WE_TEX_FLAG_IS_GIF) != 0 && tex0->frame_count > 0) {
                we_tex_advance_spritesheet(tex0, should_advance_tex0 ? dt_sec : 0.0f, trans, rot);
            }

            we_particle_vertex_t *pverts = NULL;
            uint32_t vcnt = we_particle_build_vertices(obj->particle_sys, &pverts);
            uint32_t qcnt = vcnt / 4u;
            for (uint32_t q = 0; q < qcnt; q++) {
                const we_particle_vertex_t *v = &pverts[q * 4u];
                float uv0[2] = {trans[0] + v[0].uv[0] * rot[0] + v[0].uv[1] * rot[2],
                                trans[1] + v[0].uv[0] * rot[1] + v[0].uv[1] * rot[3]};
                float uv1[2] = {trans[0] + v[1].uv[0] * rot[0] + v[1].uv[1] * rot[2],
                                trans[1] + v[1].uv[0] * rot[1] + v[1].uv[1] * rot[3]};
                float uv2[2] = {trans[0] + v[2].uv[0] * rot[0] + v[2].uv[1] * rot[2],
                                trans[1] + v[2].uv[0] * rot[1] + v[2].uv[1] * rot[3]};
                float uv3[2] = {trans[0] + v[3].uv[0] * rot[0] + v[3].uv[1] * rot[2],
                                trans[1] + v[3].uv[0] * rot[1] + v[3].uv[1] * rot[3]};
                cpu_rasterize_triangle(out_rgba, width, height, ortho_w, ortho_h, world_node,
                                       v[0].pos, uv0, v[0].color, v[1].pos, uv1, v[1].color,
                                       v[2].pos, uv2, v[2].color, src_rgba, tw, th, r_mul, g_mul,
                                       b_mul, alpha, obj->base_pass.blending);
                cpu_rasterize_triangle(out_rgba, width, height, ortho_w, ortho_h, world_node,
                                       v[2].pos, uv2, v[2].color, v[3].pos, uv3, v[3].color,
                                       v[0].pos, uv0, v[0].color, src_rgba, tw, th, r_mul, g_mul,
                                       b_mul, alpha, obj->base_pass.blending);
            }
            free(decoded_rgba);
            continue;
        }

        if (obj->puppet) {
            we_puppet_t *p = obj->puppet;
            we_puppet_eval_skin_matrices(p, p->layers, p->layer_count, (double)time_sec,
                                         p->skin_mats);
            we_puppet_skin_vertices_cpu(p, p->skin_mats, p->skinned_verts);

            float world_node[16];
            compute_object_node_world_matrix(scene, obj, 0.0f, 0.0f, world_node);
            uint32_t tri_cnt = p->index_count / 3u;
            for (uint32_t t = 0; t < tri_cnt; t++) {
                uint16_t i0 = p->indices[t * 3u + 0u];
                uint16_t i1 = p->indices[t * 3u + 1u];
                uint16_t i2 = p->indices[t * 3u + 2u];
                if (i0 >= p->vertex_count || i1 >= p->vertex_count || i2 >= p->vertex_count) {
                    continue;
                }
                const we_skinned_vertex_t *v0 = &p->skinned_verts[i0];
                const we_skinned_vertex_t *v1 = &p->skinned_verts[i1];
                const we_skinned_vertex_t *v2 = &p->skinned_verts[i2];
                cpu_rasterize_triangle(out_rgba, width, height, ortho_w, ortho_h, world_node,
                                       v0->pos, v0->uv, white_c4, v1->pos, v1->uv, white_c4,
                                       v2->pos, v2->uv, white_c4, src_rgba, tw, th, r_mul, g_mul,
                                       b_mul, alpha, obj->base_pass.blending);
            }
            free(decoded_rgba);
            continue;
        }

        if (!obj->has_base_pass || !decoded_rgba) {
            free(decoded_rgba);
            continue;
        }

        float model[16];
        compute_object_model_matrix(scene, obj, 0.0f, 0.0f, model);
        float m00 = model[0], m01 = model[4], tx = model[12];
        float m10 = model[1], m11 = model[5], ty = model[13];
        float det = m00 * m11 - m01 * m10;
        if (fabsf(det) < 1e-8f) {
            free(decoded_rgba);
            continue;
        }
        float inv_det = 1.0f / det;

        for (uint32_t y = 0; y < height; y++) {
            float wy = (1.0f - ((float)y + 0.5f) / (float)height) * ortho_h;
            for (uint32_t x = 0; x < width; x++) {
                float wx = (((float)x + 0.5f) / (float)width) * ortho_w;
                float dx = wx - tx;
                float dy = wy - ty;
                float qx = (m11 * dx - m01 * dy) * inv_det;
                float qy = (-m10 * dx + m00 * dy) * inv_det;
                if (qx < -1.0f || qx > 1.0f || qy < -1.0f || qy > 1.0f) {
                    continue;
                }
                float u = fminf(fmaxf((qx + 1.0f) * 0.5f, 0.0f), 0.999999f);
                float v = fminf(fmaxf((qy + 1.0f) * 0.5f, 0.0f), 0.999999f);
                uint32_t sx = (uint32_t)(u * (float)tw);
                uint32_t sy = (uint32_t)(v * (float)th);
                const uint8_t *sp = &src_rgba[(sy * tw + sx) * 4];

                float sa = (sp[3] / 255.0f) * alpha;
                float sr = (sp[0] / 255.0f) * r_mul;
                float sg = (sp[1] / 255.0f) * g_mul;
                float sb = (sp[2] / 255.0f) * b_mul;

                cpu_blend_pixel(&out_rgba[(y * width + x) * 4], sr, sg, sb, sa,
                                obj->base_pass.blending);
            }
        }
        free(decoded_rgba);
    }

    scene->last_pointer[0] = cur_ptr[0];
    scene->last_pointer[1] = cur_ptr[1];
    return true;
}

static void sync_project_prop_vec(we_project_t *proj, const char *key, const char *val_str,
                                  const float *values, uint32_t num_components)
{
    if (!proj || !key || key[0] == '\0') {
        return;
    }
    (void)we_project_apply_override(proj, key, val_str);
    for (uint32_t i = 0; i < proj->prop_count; ++i) {
        if (strcasecmp(proj->props[i].key, key) == 0) {
            memset(proj->props[i].val, 0, sizeof(proj->props[i].val));
            memcpy(proj->props[i].val, values, num_components * sizeof(float));
            proj->props[i].num_components = num_components;
            break;
        }
    }
}

static bool user_prop_matches_key(const char *prop_expr, const char *key)
{
    if (!prop_expr || !key || prop_expr[0] == '\0' || key[0] == '\0') {
        return false;
    }
    const char *p = prop_expr;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    while (*p == '!') {
        p++;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
    }
    const char *end = p;
    while (*end && *end != '!' && *end != '=' && !isspace((unsigned char)*end)) {
        end++;
    }
    size_t len = (size_t)(end - p);
    return len > 0 && strlen(key) == len && strncasecmp(p, key, len) == 0;
}

static void update_pass_uniform_override(we_scene_t *scene, we_material_pass_t *mp,
                                         const char *name, const char *val_str, const float *values,
                                         uint32_t num_components)
{
    if (!scene || !mp || !name) {
        return;
    }
    bool matched_cv = false;
    for (uint32_t c = 0; c < mp->const_val_count; ++c) {
        we_const_val_t *cv = &mp->const_vals[c];
        if (cv->user_prop[0] != '\0' && strchr(cv->user_prop, '=') != NULL &&
            user_prop_matches_key(cv->user_prop, name)) {
            bool bval = (cv->num_components > 0) ? (cv->val[0] > 0.5f) : false;
            if (we_project_eval_prop_bool(&scene->project, cv->user_prop, &bval)) {
                memset(cv->val, 0, sizeof(cv->val));
                cv->val[0] = bval ? 1.0f : 0.0f;
                cv->num_components = 1;
            }
            matched_cv = true;
        } else if (strcasecmp(cv->name, name) == 0 ||
                   (cv->user_prop[0] != '\0' && strcasecmp(cv->user_prop, name) == 0)) {
            memset(cv->val, 0, sizeof(cv->val));
            memcpy(cv->val, values, num_components * sizeof(float));
            cv->num_components = num_components;
            if (cv->user_prop[0] != '\0' && strchr(cv->user_prop, '=') == NULL) {
                sync_project_prop_vec(&scene->project, cv->user_prop, val_str, values,
                                      num_components);
            }
            matched_cv = true;
        }
    }
    for (uint32_t u = 0; u < mp->prep.meta.uniform_count; ++u) {
        const we_shader_uniform_meta_t *um = &mp->prep.meta.uniforms[u];
        if (strcasecmp(um->name, name) == 0 ||
            (um->material_key[0] != '\0' && strcasecmp(um->material_key, name) == 0)) {
            if (um->material_key[0] != '\0') {
                sync_project_prop_vec(&scene->project, um->material_key, val_str, values,
                                      num_components);
            }
            bool found_for_um = false;
            for (uint32_t c = 0; c < mp->const_val_count; ++c) {
                we_const_val_t *cv = &mp->const_vals[c];
                if (strcasecmp(cv->name, um->name) == 0 ||
                    (um->material_key[0] != '\0' && strcasecmp(cv->name, um->material_key) == 0)) {
                    memset(cv->val, 0, sizeof(cv->val));
                    memcpy(cv->val, values, num_components * sizeof(float));
                    cv->num_components = num_components;
                    if (cv->user_prop[0] != '\0' && strchr(cv->user_prop, '=') == NULL) {
                        sync_project_prop_vec(&scene->project, cv->user_prop, val_str, values,
                                              num_components);
                    }
                    found_for_um = true;
                }
            }
            if (!found_for_um && !matched_cv && mp->const_val_count < WE_MAX_CONST_VALS) {
                we_const_val_t *ncv = &mp->const_vals[mp->const_val_count++];
                memset(ncv, 0, sizeof(*ncv));
                snprintf(ncv->name, sizeof(ncv->name), "%s", um->name);
                memcpy(ncv->val, values, num_components * sizeof(float));
                ncv->num_components = num_components;
            }
        }
    }
}

bool we_scene_set_uniform_override(we_scene_t *scene, const char *name, const float *values,
                                   uint32_t num_components)
{
    if (!scene || !name || name[0] == '\0' || !values || num_components < 1 || num_components > 4) {
        return false;
    }
    for (uint32_t i = 0; i < num_components; ++i) {
        if (!isfinite(values[i])) {
            return false;
        }
    }

    char val_str[128];
    if (num_components == 1) {
        snprintf(val_str, sizeof(val_str), "%.6g", values[0]);
    } else if (num_components == 2) {
        snprintf(val_str, sizeof(val_str), "%.6g %.6g", values[0], values[1]);
    } else if (num_components == 3) {
        snprintf(val_str, sizeof(val_str), "%.6g %.6g %.6g", values[0], values[1], values[2]);
    } else {
        snprintf(val_str, sizeof(val_str), "%.6g %.6g %.6g %.6g", values[0], values[1], values[2],
                 values[3]);
    }

    if (!we_project_apply_override(&scene->project, name, val_str)) {
        return false;
    }
    sync_project_prop_vec(&scene->project, name, val_str, values, num_components);

    for (uint32_t i = 0; i < scene->object_count; ++i) {
        we_scene_object_t *obj = &scene->objects[i];
        if (obj->color_user_prop[0] != '\0' && strcasecmp(obj->color_user_prop, name) == 0 &&
            num_components >= 3) {
            obj->color[0] = values[0];
            obj->color[1] = values[1];
            obj->color[2] = values[2];
        }
        if (obj->alpha_user_prop[0] != '\0' && strcasecmp(obj->alpha_user_prop, name) == 0) {
            obj->alpha = values[0];
        }
        if (obj->visible_user_prop[0] != '\0' &&
            user_prop_matches_key(obj->visible_user_prop, name)) {
            (void)we_project_eval_prop_bool(&scene->project, obj->visible_user_prop, &obj->visible);
        }
        if (obj->has_base_pass) {
            update_pass_uniform_override(scene, &obj->base_pass, name, val_str, values,
                                         num_components);
        }
        for (uint32_t e = 0; e < obj->effect_count; ++e) {
            we_effect_t *eff = &obj->effects[e];
            if (eff->visible_user_prop[0] != '\0' &&
                user_prop_matches_key(eff->visible_user_prop, name)) {
                (void)we_project_eval_prop_bool(&scene->project, eff->visible_user_prop,
                                                &eff->visible);
            }
            for (uint32_t p = 0; p < eff->pass_count; ++p) {
                update_pass_uniform_override(scene, &eff->passes[p], name, val_str, values,
                                             num_components);
            }
        }
        for (uint32_t s = 0; s < obj->script_count; ++s) {
            if (obj->scripts[s].vm) {
                bind_project_props_to_script(obj->scripts[s].vm, &scene->project);
            }
        }
    }

    apply_scene_script_user_properties(scene);
    return true;
}

void waywal_we_scene_forget_bo(we_scene_t *scene, const dmabuf_bo_t *bo)
{
    if (!scene || !bo) {
        return;
    }
    ensure_egl_procs(scene);
    PFNEGLDESTROYIMAGEKHRPROC destroy_img =
        __extension__(PFNEGLDESTROYIMAGEKHRPROC) scene->fn_eglDestroyImageKHR;

    for (size_t i = 0; i < scene->num_imported_bos; i++) {
        if (scene->imported_bos[i].bo == bo) {
            if (scene->gl_initialized && scene->egl_display && scene->egl_context &&
                scene->imported_bos[i].owns_egl) {
                if (eglGetCurrentContext() != scene->egl_context) {
                    eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                   scene->egl_context);
                }
                if (scene->imported_bos[i].gl_tex) {
                    GLuint del_tex = scene->imported_bos[i].gl_tex;
                    glDeleteTextures(1, &del_tex);
                }
                if (scene->imported_bos[i].egl_image && destroy_img) {
                    void *del_img = scene->imported_bos[i].egl_image;
                    destroy_img(scene->egl_display, del_img);
                }
            }
            for (size_t j = i + 1; j < scene->num_imported_bos; j++) {
                scene->imported_bos[j - 1] = scene->imported_bos[j];
            }
            scene->num_imported_bos--;
            break;
        }
    }
}

static void free_material_pass(we_material_pass_t *pass, bool gl_init)
{
    if (!pass) {
        return;
    }
    for (uint32_t s = 0; s < WE_MAX_PASS_TEXTURES; s++) {
        if (pass->textures[s]) {
            we_tex_destroy(pass->textures[s]);
            pass->textures[s] = NULL;
        }
    }
    for (uint32_t c = 0; c < pass->const_val_count; c++) {
        free(pass->const_vals[c].script_src);
        pass->const_vals[c].script_src = NULL;
    }
    we_shader_preprocessed_free(&pass->prep);
    if (gl_init && pass->gl_prog != 0) {
        glDeleteProgram(pass->gl_prog);
        pass->gl_prog = 0;
    }
}

void we_scene_destroy(we_scene_t *scene)
{
    if (!scene) {
        return;
    }

    if (scene->gl_initialized && scene->egl_display && scene->egl_context) {
        if (eglGetCurrentContext() != scene->egl_context) {
            eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, scene->egl_context);
        }
    }

    if (scene->objects) {
        for (uint32_t i = 0; i < scene->object_count; i++) {
            we_scene_object_t *obj = &scene->objects[i];
            if (obj->puppet) {
                we_puppet_destroy(obj->puppet);
                obj->puppet = NULL;
            }
            if (obj->particle_sys) {
                we_particle_destroy(obj->particle_sys);
                obj->particle_sys = NULL;
            }
            for (uint32_t s = 0; s < obj->script_count; s++) {
                if (obj->scripts[s].vm) {
                    we_script_destroy(obj->scripts[s].vm);
                    obj->scripts[s].vm = NULL;
                }
            }
            free_material_pass(&obj->base_pass, scene->gl_initialized);
            if (obj->effects) {
                for (uint32_t e = 0; e < obj->effect_count; e++) {
                    we_effect_t *eff = &obj->effects[e];
                    if (scene->gl_initialized) {
                        for (uint32_t f = 0; f < eff->fbo_count; f++) {
                            if (eff->fbos[f].gl_fbo) {
                                glDeleteFramebuffers(1, &eff->fbos[f].gl_fbo);
                            }
                            if (eff->fbos[f].gl_tex) {
                                glDeleteTextures(1, &eff->fbos[f].gl_tex);
                            }
                        }
                    }
                    if (eff->passes) {
                        for (uint32_t p = 0; p < eff->pass_count; p++) {
                            free_material_pass(&eff->passes[p], scene->gl_initialized);
                        }
                        free(eff->passes);
                    }
                }
                free(obj->effects);
            }
            if (scene->gl_initialized) {
                for (int k = 0; k < 2; k++) {
                    if (obj->ping_fbo[k]) {
                        glDeleteFramebuffers(1, &obj->ping_fbo[k]);
                    }
                    if (obj->ping_tex[k]) {
                        glDeleteTextures(1, &obj->ping_tex[k]);
                    }
                }
            }
        }
        free(scene->objects);
    }

    if (scene->gl_initialized) {
        ensure_egl_procs(scene);
        PFNEGLDESTROYIMAGEKHRPROC destroy_img =
            __extension__(PFNEGLDESTROYIMAGEKHRPROC) scene->fn_eglDestroyImageKHR;
        for (size_t i = 0; i < scene->num_imported_bos; i++) {
            if (!scene->imported_bos[i].owns_egl) {
                continue;
            }
            if (scene->imported_bos[i].gl_tex) {
                GLuint del_tex = scene->imported_bos[i].gl_tex;
                glDeleteTextures(1, &del_tex);
            }
            if (scene->imported_bos[i].egl_image && destroy_img) {
                void *del_img = scene->imported_bos[i].egl_image;
                destroy_img(scene->egl_display, del_img);
            }
        }
        scene->num_imported_bos = 0;

        for (uint32_t r = 0; r < scene->rt_fbo_count; r++) {
            if (scene->rt_fbos[r].gl_fbo) {
                glDeleteFramebuffers(1, &scene->rt_fbos[r].gl_fbo);
            }
            if (scene->rt_fbos[r].gl_tex) {
                glDeleteTextures(1, &scene->rt_fbos[r].gl_tex);
            }
        }
        scene->rt_fbo_count = 0;

        if (scene->scene_fbo) {
            glDeleteFramebuffers(1, &scene->scene_fbo);
        }
        if (scene->scene_fbo_tex) {
            glDeleteTextures(1, &scene->scene_fbo_tex);
        }
        if (scene->full_fb_fbo) {
            glDeleteFramebuffers(1, &scene->full_fb_fbo);
        }
        if (scene->full_fb_tex) {
            glDeleteTextures(1, &scene->full_fb_tex);
        }
        if (scene->target_fbo) {
            glDeleteFramebuffers(1, &scene->target_fbo);
        }
        if (scene->prog_present) {
            glDeleteProgram(scene->prog_present);
        }
        if (scene->default_program) {
            glDeleteProgram(scene->default_program);
        }
        if (scene->particle_program) {
            glDeleteProgram(scene->particle_program);
        }
        if (scene->quad_vao) {
            glDeleteVertexArrays(1, &scene->quad_vao);
        }
        if (scene->quad_vbo) {
            glDeleteBuffers(1, &scene->quad_vbo);
        }
        if (scene->tex_util_white) {
            glDeleteTextures(1, &scene->tex_util_white);
        }
        if (scene->tex_util_black) {
            glDeleteTextures(1, &scene->tex_util_black);
        }
        if (scene->tex_util_flatnormal) {
            glDeleteTextures(1, &scene->tex_util_flatnormal);
        }
        if (scene->tex_util_noise) {
            glDeleteTextures(1, &scene->tex_util_noise);
        }
        if (scene->tex_album_art) {
            glDeleteTextures(1, &scene->tex_album_art);
        }

        if (scene->owns_egl && scene->egl_display != EGL_NO_DISPLAY &&
            scene->egl_context != EGL_NO_CONTEXT) {
            if (s_we_shared_refcount > 0) {
                s_we_shared_refcount--;
            }
            if (s_we_shared_refcount == 0) {
                eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                eglDestroyContext(scene->egl_display, scene->egl_context);
                s_we_shared_dpy = EGL_NO_DISPLAY;
                s_we_shared_ctx = EGL_NO_CONTEXT;
            }
        }
    }

    free(scene->album_art_rgba);
    scene->album_art_rgba = NULL;
    if (scene->pkg) {
        we_pkg_close(scene->pkg);
    }
    free(scene);
}

void we_scene_set_album_art(we_scene_t *scene, const uint8_t *rgba, uint32_t w, uint32_t h)
{
    if (!scene) {
        return;
    }
    free(scene->album_art_rgba);
    scene->album_art_rgba = NULL;
    scene->album_art_w = 0;
    scene->album_art_h = 0;

    if (rgba && w > 0 && h > 0) {
        size_t sz = (size_t)w * (size_t)h * 4u;
        scene->album_art_rgba = (uint8_t *)malloc(sz);
        if (scene->album_art_rgba) {
            memcpy(scene->album_art_rgba, rgba, sz);
            scene->album_art_w = w;
            scene->album_art_h = h;
        }
    }

    if (scene->gl_initialized && scene->egl_display && scene->egl_context) {
        if (eglGetCurrentContext() != scene->egl_context) {
            eglMakeCurrent(scene->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, scene->egl_context);
        }
        if (scene->tex_album_art) {
            glDeleteTextures(1, &scene->tex_album_art);
            scene->tex_album_art = 0;
        }
        if (scene->album_art_rgba && scene->album_art_w > 0 && scene->album_art_h > 0) {
            GLuint atex = 0;
            glGenTextures(1, &atex);
            glBindTexture(GL_TEXTURE_2D, atex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)scene->album_art_w,
                         (GLsizei)scene->album_art_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         scene->album_art_rgba);
            scene->tex_album_art = (uint32_t)atex;
        }
    }
}

void we_scene_set_album_art_bgra(we_scene_t *scene, const uint8_t *bgra, uint32_t w, uint32_t h)
{
    if (!scene) {
        return;
    }
    if (!bgra || w == 0 || h == 0) {
        we_scene_set_album_art(scene, NULL, 0, 0);
        return;
    }
    size_t npx = (size_t)w * (size_t)h;
    uint8_t *rgba = (uint8_t *)malloc(npx * 4u);
    if (!rgba) {
        return;
    }
    for (size_t i = 0; i < npx; ++i) {
        rgba[i * 4u + 0u] = bgra[i * 4u + 2u];
        rgba[i * 4u + 1u] = bgra[i * 4u + 1u];
        rgba[i * 4u + 2u] = bgra[i * 4u + 0u];
        rgba[i * 4u + 3u] = bgra[i * 4u + 3u];
    }
    we_scene_set_album_art(scene, rgba, w, h);
    free(rgba);
}

void we_scene_set_media_playing(we_scene_t *scene, float playing)
{
    if (!scene) {
        return;
    }
    scene->media_playing = playing;
}
