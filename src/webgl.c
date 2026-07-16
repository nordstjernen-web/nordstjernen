/* Nordstjernen — minimalist WebGL: canvas.getContext mapped onto GLES via GTK.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include "webgl.h"

#if defined(NS_ENABLE_WEBGL)

#include <stdint.h>
#include <string.h>

#include <epoxy/gl.h>

#include "config.h"
#include "glctx.h"
#include "js.h"
#include "net.h"

#define NS_UNPACK_FLIP_Y_WEBGL              0x9240
#define NS_UNPACK_PREMULTIPLY_ALPHA_WEBGL   0x9241
#define NS_CONTEXT_LOST_WEBGL               0x9242
#define NS_UNPACK_COLORSPACE_CONVERSION_WEBGL 0x9243
#define NS_BROWSER_DEFAULT_WEBGL            0x9244
#define NS_UNMASKED_VENDOR_WEBGL           0x9245
#define NS_UNMASKED_RENDERER_WEBGL         0x9246
#define NS_MAX_TEXTURE_MAX_ANISOTROPY_EXT  0x84FF
#define NS_TEXTURE_MAX_ANISOTROPY_EXT      0x84FE

typedef struct ns_webgl {
    ns_js         *js;
    JSContext     *ctx;
    JSValue        js_obj;
    const ns_node *canvas;
    int            version;
    ns_gl_context *gl;
    GLuint         fbo, color_tex, depth_rb;
    GLuint         draw_fbo, msaa_color_rb, msaa_depth_rb;
    GLuint         user_draw_fbo, user_read_fbo;
    GLuint         bound_draw_fbo, bound_read_fbo;
    GLuint         bound_array_buffer, bound_element_array_buffer;
    int            samples;
    int            w, h;
    guint32        size_attr_gen;
    gboolean       size_synced;
    cairo_surface_t *surf;
    uint8_t       *readback;
    size_t         readback_len;
    gboolean       dirty;
    gboolean       repaint_queued;
    gboolean       unpack_flip_y;
    gboolean       premultiply;
    gboolean       premultiplied_alpha;
    gboolean       depth, stencil, alpha, antialias, preserve;
    GHashTable    *syncs;
    GHashTable    *bound_buffers;
    GHashTable    *buffer_sizes;
    int            next_sync;
} ns_webgl;

static JSClassID ns_webgl_class_id;
static GHashTable *g_webgl_by_node;

static GHashTable *g_webgl_decisions;
static char *g_webgl_pending;

static void
ns_webgl_record_decision(const char *origin, gboolean allow)
{
    if (!g_webgl_decisions)
        g_webgl_decisions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
    g_hash_table_insert(g_webgl_decisions, g_strdup(origin),
                        GINT_TO_POINTER(allow ? 1 : 2));
}

char *
ns_webgl_take_pending_origin(void)
{
    char *origin = g_webgl_pending;
    g_webgl_pending = NULL;
    return origin;
}

void
ns_webgl_set_decision(const char *origin, int allow)
{
    if (!origin || !*origin) return;
    ns_webgl_record_decision(origin, allow ? TRUE : FALSE);
    if (allow) {
        ns_config *cfg = ns_config_mut();
        if (cfg && !cfg->webgl_enabled) {
            cfg->webgl_enabled = TRUE;
            ns_config_save(NULL);
        }
    }
    if (g_webgl_pending && g_strcmp0(g_webgl_pending, origin) == 0)
        g_clear_pointer(&g_webgl_pending, g_free);
}

static gboolean
ns_webgl_permission(ns_js *js)
{
    const char *url = ns_js_current_url(js);
    char *origin = ns_url_origin_from(url);
    if (!origin || !*origin) {
        g_free(origin);
        origin = g_strdup(url && *url ? url : "this page");
    }

    if (!g_webgl_decisions)
        g_webgl_decisions = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
    if (g_hash_table_contains(g_webgl_decisions, origin)) {
        g_free(origin);
        return TRUE;
    }

    g_hash_table_insert(g_webgl_decisions, g_strdup(origin),
                        GINT_TO_POINTER(1));
    g_free(g_webgl_pending);
    g_webgl_pending = g_strdup(origin);
    g_free(origin);
    return TRUE;
}

static int
ns_webgl_dim(const ns_node *el, const char *name, int defv)
{
    const char *s = ns_element_get_attr(el, name);
    if (!s || !*s) return defv;
    long v = strtol(s, NULL, 10);
    if (v <= 0) return defv;
    if (v > 8192) v = 8192;
    return (int)v;
}

static GLuint
ns_webgl_draw_target(ns_webgl *g)
{
    return g->samples > 1 ? g->draw_fbo : g->fbo;
}

static void
wgl_bind_framebuffer(ns_webgl *g, GLenum target, GLuint fbo)
{
    if (!g) return;
    if (target == GL_FRAMEBUFFER) {
        if (g->bound_draw_fbo == fbo && g->bound_read_fbo == fbo)
            return;
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        g->bound_draw_fbo = fbo;
        g->bound_read_fbo = fbo;
    } else if (target == GL_DRAW_FRAMEBUFFER) {
        if (g->bound_draw_fbo == fbo)
            return;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        g->bound_draw_fbo = fbo;
    } else if (target == GL_READ_FRAMEBUFFER) {
        if (g->bound_read_fbo == fbo)
            return;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        g->bound_read_fbo = fbo;
    } else {
        glBindFramebuffer(target, fbo);
    }
}

static void
wgl_bind_current_targets(ns_webgl *g)
{
    GLuint dt = ns_webgl_draw_target(g);
    GLuint draw = g->user_draw_fbo ? g->user_draw_fbo : dt;
    GLuint read = g->user_read_fbo ? g->user_read_fbo : dt;
    if (draw == read) {
        wgl_bind_framebuffer(g, GL_FRAMEBUFFER, draw);
    } else {
        wgl_bind_framebuffer(g, GL_DRAW_FRAMEBUFFER, draw);
        wgl_bind_framebuffer(g, GL_READ_FRAMEBUFFER, read);
    }
}

static void
wgl_mark_dirty(ns_webgl *g)
{
    if (!g) return;
    g->dirty = TRUE;
    if (!g->repaint_queued) {
        ns_js_request_repaint(g->js);
        g->repaint_queued = TRUE;
    }
}

static uint8_t *
wgl_readback_buffer(ns_webgl *g, size_t need)
{
    if (!g || need == 0) return NULL;
    if (need <= g->readback_len) return g->readback;
    uint8_t *p = g_try_realloc(g->readback, need);
    if (!p) return NULL;
    g->readback = p;
    g->readback_len = need;
    return p;
}

static void
wgl_copy_opaque_rgba_row(uint8_t *dst, const uint8_t *src, int w,
                         gboolean force_alpha)
{
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
    for (int x = 0; x < w; x++) {
        uint32_t v;
        memcpy(&v, src + (size_t)x * 4, sizeof v);
        uint32_t a = force_alpha ? 0xff000000u : (v & 0xff000000u);
        v = a | (v & 0x0000ff00u) |
            ((v & 0x000000ffu) << 16) |
            ((v & 0x00ff0000u) >> 16);
        memcpy(dst + (size_t)x * 4, &v, sizeof v);
    }
#else
    for (int x = 0; x < w; x++) {
        dst[x * 4 + 0] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4 + 0];
        dst[x * 4 + 3] = force_alpha ? 255u : src[x * 4 + 3];
    }
#endif
}

static gboolean
ns_webgl_alloc_storage(ns_webgl *g, int w, int h)
{
    GLenum ds_format = (g->depth && g->stencil) ? GL_DEPTH24_STENCIL8
                     : g->stencil               ? GL_STENCIL_INDEX8
                     : g->depth                  ? GL_DEPTH_COMPONENT16
                                                 : 0;
    GLenum ds_attach = (g->depth && g->stencil) ? GL_DEPTH_STENCIL_ATTACHMENT
                     : g->stencil               ? GL_STENCIL_ATTACHMENT
                                                 : GL_DEPTH_ATTACHMENT;

    wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->fbo);
    glBindTexture(GL_TEXTURE_2D, g->color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g->color_tex, 0);

    if (g->samples > 1) {
        wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->draw_fbo);
        glBindRenderbuffer(GL_RENDERBUFFER, g->msaa_color_rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, g->samples,
                                         GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, g->msaa_color_rb);
        if (ds_format) {
            glBindRenderbuffer(GL_RENDERBUFFER, g->msaa_depth_rb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, g->samples,
                                             ds_format, w, h);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, ds_attach,
                                      GL_RENDERBUFFER, g->msaa_depth_rb);
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            g->samples = 1;
    }

    if (g->samples <= 1 && ds_format) {
        wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->fbo);
        glBindRenderbuffer(GL_RENDERBUFFER, g->depth_rb);
        glRenderbufferStorage(GL_RENDERBUFFER, ds_format, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, ds_attach,
                                  GL_RENDERBUFFER, g->depth_rb);
    }

    wgl_bind_framebuffer(g, GL_FRAMEBUFFER, ns_webgl_draw_target(g));
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void
ns_webgl_sync_size(ns_webgl *g)
{
    guint32 gen = g->canvas ? g->canvas->attr_gen : 0;
    if (g->size_synced && gen == g->size_attr_gen) return;
    g->size_attr_gen = gen;
    g->size_synced = TRUE;

    int w = ns_webgl_dim(g->canvas, "width", 300);
    int h = ns_webgl_dim(g->canvas, "height", 150);
    if (w == g->w && h == g->h) return;
    g->w = w;
    g->h = h;
    ns_webgl_alloc_storage(g, w, h);
    glViewport(0, 0, w, h);
    wgl_mark_dirty(g);
    if (g->surf) { cairo_surface_destroy(g->surf); g->surf = NULL; }
    if (!JS_IsUndefined(g->js_obj)) {
        JS_SetPropertyStr(g->ctx, g->js_obj, "drawingBufferWidth",
                          JS_NewInt32(g->ctx, w));
        JS_SetPropertyStr(g->ctx, g->js_obj, "drawingBufferHeight",
                          JS_NewInt32(g->ctx, h));
    }
}

static gboolean
ns_webgl_attr(JSContext *ctx, JSValueConst attrs, const char *name, gboolean defv)
{
    if (!JS_IsObject(attrs)) return defv;
    JSValue v = JS_GetPropertyStr(ctx, attrs, name);
    gboolean r = defv;
    if (!JS_IsUndefined(v) && !JS_IsNull(v))
        r = JS_ToBool(ctx, v) ? TRUE : FALSE;
    JS_FreeValue(ctx, v);
    return r;
}

static void ns_webgl_free(ns_webgl *g);

static ns_webgl *
ns_webgl_make(JSContext *ctx, ns_js *js, const ns_node *canvas, int version,
              JSValueConst attrs)
{
    ns_gl_context *gl = ns_gl_context_create();
    if (!gl) return NULL;
    if (!ns_gl_context_make_current(gl)) {
        ns_gl_context_destroy(gl);
        return NULL;
    }

    ns_webgl *g = g_new0(ns_webgl, 1);
    g->js = js;
    g->canvas = canvas;
    g->version = version;
    g->gl = gl;
    g->js_obj = JS_UNDEFINED;
    g->alpha     = ns_webgl_attr(ctx, attrs, "alpha", TRUE);
    g->depth     = ns_webgl_attr(ctx, attrs, "depth", TRUE);
    g->stencil   = ns_webgl_attr(ctx, attrs, "stencil", FALSE);
    g->antialias = ns_webgl_attr(ctx, attrs, "antialias", TRUE);
    g->preserve  = ns_webgl_attr(ctx, attrs, "preserveDrawingBuffer", FALSE);
    g->premultiplied_alpha = ns_webgl_attr(ctx, attrs, "premultipliedAlpha", TRUE);
    g->w = ns_webgl_dim(canvas, "width", 300);
    g->h = ns_webgl_dim(canvas, "height", 150);
    g->dirty = TRUE;

    g->samples = 1;
    if (g->antialias) {
        GLint max_samples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
        g->samples = max_samples >= 4 ? 4 : (max_samples > 1 ? max_samples : 1);
    }
    g->antialias = g->samples > 1;

    glGenFramebuffers(1, &g->fbo);
    glGenTextures(1, &g->color_tex);
    glGenRenderbuffers(1, &g->depth_rb);
    if (g->samples > 1) {
        glGenFramebuffers(1, &g->draw_fbo);
        glGenRenderbuffers(1, &g->msaa_color_rb);
        glGenRenderbuffers(1, &g->msaa_depth_rb);
    }
    if (!ns_webgl_alloc_storage(g, g->w, g->h)) {
        ns_webgl_free(g);
        return NULL;
    }
    glViewport(0, 0, g->w, g->h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    return g;
}

static void
ns_webgl_free(ns_webgl *g)
{
    if (!g) return;
    if (g->gl) {
        ns_gl_context_make_current(g->gl);
        glDeleteFramebuffers(1, &g->fbo);
        glDeleteTextures(1, &g->color_tex);
        glDeleteRenderbuffers(1, &g->depth_rb);
        if (g->draw_fbo) glDeleteFramebuffers(1, &g->draw_fbo);
        if (g->msaa_color_rb) glDeleteRenderbuffers(1, &g->msaa_color_rb);
        if (g->msaa_depth_rb) glDeleteRenderbuffers(1, &g->msaa_depth_rb);
        if (g->syncs) {
            GHashTableIter it;
            gpointer k, v;
            g_hash_table_iter_init(&it, g->syncs);
            while (g_hash_table_iter_next(&it, &k, &v))
                glDeleteSync((GLsync)v);
        }
        ns_gl_context_release(g->gl);
        ns_gl_context_destroy(g->gl);
    }
    if (g->syncs) g_hash_table_destroy(g->syncs);
    if (g->bound_buffers) g_hash_table_destroy(g->bound_buffers);
    if (g->buffer_sizes) g_hash_table_destroy(g->buffer_sizes);
    if (g->surf) cairo_surface_destroy(g->surf);
    g_free(g->readback);
    g_free(g);
}

static void
ns_webgl_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    ns_webgl *g = JS_GetOpaque(val, ns_webgl_class_id);
    if (!g) return;
    if (g_webgl_by_node)
        g_hash_table_remove(g_webgl_by_node, g->canvas);
    ns_webgl_free(g);
}

static JSClassDef ns_webgl_class = {
    "WebGLRenderingContext",
    .finalizer = ns_webgl_finalizer,
};

static ns_webgl *
wgl_cur(JSContext *ctx, JSValueConst this_val)
{
    (void)ctx;
    ns_webgl *g = JS_GetOpaque(this_val, ns_webgl_class_id);
    if (!g || !g->gl) return NULL;
    ns_gl_context_make_current(g->gl);
    ns_webgl_sync_size(g);
    wgl_bind_current_targets(g);
    return g;
}

static int
argi(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    int32_t v = 0;
    if (i < argc) JS_ToInt32(ctx, &v, argv[i]);
    return v;
}

static double
argd(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    double v = 0;
    if (i < argc) JS_ToFloat64(ctx, &v, argv[i]);
    return v;
}

static gboolean
argbool(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    return (i < argc) ? (JS_ToBool(ctx, argv[i]) ? TRUE : FALSE) : FALSE;
}

static int
wgl_name(JSContext *ctx, JSValueConst v)
{
    if (!JS_IsObject(v)) return 0;
    JSValue p = JS_GetPropertyStr(ctx, v, "_n");
    int32_t n = 0;
    JS_ToInt32(ctx, &n, p);
    JS_FreeValue(ctx, p);
    return n;
}

static int
wgl_loc(JSContext *ctx, JSValueConst v)
{
    if (!JS_IsObject(v)) return -1;
    JSValue p = JS_GetPropertyStr(ctx, v, "_loc");
    int32_t n = -1;
    JS_ToInt32(ctx, &n, p);
    JS_FreeValue(ctx, p);
    return n;
}

static JSValue
wgl_wrap(JSContext *ctx, GLuint name, const char *kind)
{
    if (!name) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "_n", JS_NewInt32(ctx, (int)name));
    JS_SetPropertyStr(ctx, o, "_t", JS_NewString(ctx, kind));
    return o;
}

static JSValue
wgl_typed_array(JSContext *ctx, JSValueConst buf, JSTypedArrayEnum type)
{
    JSValueConst args[3] = { buf, JS_UNDEFINED, JS_UNDEFINED };
    return JS_NewTypedArray(ctx, 3, args, type);
}

static const uint8_t *
view_bytes(JSContext *ctx, JSValueConst v, size_t *out_len, JSValue *hold)
{
    *hold = JS_UNDEFINED;
    *out_len = 0;
    if (JS_IsArrayBuffer(v)) {
        size_t n = 0;
        uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
        *out_len = n;
        return p;
    }
    size_t off = 0, len = 0, bpe = 0;
    JSValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &bpe);
    if (JS_IsException(buf)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }
    size_t tot = 0;
    uint8_t *base = JS_GetArrayBuffer(ctx, &tot, buf);
    if (!base || off + len > tot) {
        JS_FreeValue(ctx, buf);
        return NULL;
    }
    *hold = buf;
    *out_len = len;
    return base + off;
}

static GLuint
wgl_bound_buffer(ns_webgl *g, GLenum target)
{
    if (!g) return 0;
    if (target == GL_ARRAY_BUFFER) return g->bound_array_buffer;
    if (target == GL_ELEMENT_ARRAY_BUFFER) return g->bound_element_array_buffer;
    if (!g->bound_buffers) return 0;
    return GPOINTER_TO_UINT(g_hash_table_lookup(g->bound_buffers,
                                                GUINT_TO_POINTER(target)));
}

static void
wgl_set_bound_buffer(ns_webgl *g, GLenum target, GLuint name)
{
    if (!g) return;
    if (target == GL_ARRAY_BUFFER) {
        g->bound_array_buffer = name;
    } else if (target == GL_ELEMENT_ARRAY_BUFFER) {
        g->bound_element_array_buffer = name;
    } else {
        if (!g->bound_buffers)
            g->bound_buffers = g_hash_table_new(g_direct_hash, g_direct_equal);
        if (name)
            g_hash_table_insert(g->bound_buffers, GUINT_TO_POINTER(target),
                                GUINT_TO_POINTER(name));
        else
            g_hash_table_remove(g->bound_buffers, GUINT_TO_POINTER(target));
    }
}

static void
wgl_set_buffer_size(ns_webgl *g, GLuint name, size_t size)
{
    if (!g || !name) return;
    if (!g->buffer_sizes)
        g->buffer_sizes = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(g->buffer_sizes, GUINT_TO_POINTER(name),
                        GSIZE_TO_POINTER(size));
}

static size_t
wgl_buffer_size(ns_webgl *g, GLuint name)
{
    if (!g || !name || !g->buffer_sizes) return 0;
    return GPOINTER_TO_SIZE(g_hash_table_lookup(g->buffer_sizes,
                                                GUINT_TO_POINTER(name)));
}

static int
wgl_floats(JSContext *ctx, JSValueConst v, float *out, int max)
{
    if (JS_GetTypedArrayType(v) == JS_TYPED_ARRAY_FLOAT32) {
        JSValue hold;
        size_t n = 0;
        const uint8_t *b = view_bytes(ctx, v, &n, &hold);
        int cnt = b ? (int)(n / sizeof(float)) : 0;
        if (cnt > max) cnt = max;
        if (b) memcpy(out, b, (size_t)cnt * sizeof(float));
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return cnt;
    }
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    int cnt = (int)len;
    if (cnt > max) cnt = max;
    for (int i = 0; i < cnt; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        double d = 0;
        JS_ToFloat64(ctx, &d, e);
        JS_FreeValue(ctx, e);
        out[i] = (float)d;
    }
    return cnt;
}

static int
wgl_ints(JSContext *ctx, JSValueConst v, GLint *out, int max)
{
    int t = JS_GetTypedArrayType(v);
    if (t == JS_TYPED_ARRAY_INT32 || t == JS_TYPED_ARRAY_UINT32) {
        JSValue hold;
        size_t n = 0;
        const uint8_t *b = view_bytes(ctx, v, &n, &hold);
        int cnt = b ? (int)(n / sizeof(GLint)) : 0;
        if (cnt > max) cnt = max;
        if (b) memcpy(out, b, (size_t)cnt * sizeof(GLint));
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return cnt;
    }
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    uint32_t len = 0;
    JS_ToUint32(ctx, &len, lv);
    JS_FreeValue(ctx, lv);
    int cnt = (int)len;
    if (cnt > max) cnt = max;
    for (int i = 0; i < cnt; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, (uint32_t)i);
        int32_t d = 0;
        JS_ToInt32(ctx, &d, e);
        JS_FreeValue(ctx, e);
        out[i] = d;
    }
    return cnt;
}

#define NS_WEBGL_MAX_ALLOC    (1024u * 1024u * 1024u)
#define NS_WEBGL_MAX_CONTEXTS 32
#define NS_WEBGL_MAX_SHADER   (4u * 1024u * 1024u)

static int
wgl_components(int format)
{
    switch (format) {
    case GL_RGBA: case GL_RGBA_INTEGER:            return 4;
    case GL_RGB:  case GL_RGB_INTEGER:             return 3;
    case GL_RG:   case GL_RG_INTEGER:
    case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL: return 2;
    default:                                       return 1;
    }
}

static int
wgl_type_bytes(int type)
{
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE:                return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2;
    case GL_INT: case GL_UNSIGNED_INT: case GL_FLOAT:   return 4;
    default:                                            return 1;
    }
}

static size_t
wgl_pixel_bytes(int format, int type)
{
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    default:
        return (size_t)wgl_components(format) * (size_t)wgl_type_bytes(type);
    }
}

static size_t
wgl_transfer_bytes(ns_webgl *g, int w, int h, int depth,
                   int format, int type, gboolean pack)
{
    if (w <= 0 || h <= 0 || depth <= 0) return 0;
    size_t pix = wgl_pixel_bytes(format, type);
    GLint align = 4, rowlen = 0, skiprows = 0, skippix = 0;
    GLint imgh = 0, skipimg = 0;
    glGetIntegerv(pack ? GL_PACK_ALIGNMENT : GL_UNPACK_ALIGNMENT, &align);
    if (g->version >= 2) {
        glGetIntegerv(pack ? GL_PACK_ROW_LENGTH : GL_UNPACK_ROW_LENGTH, &rowlen);
        glGetIntegerv(pack ? GL_PACK_SKIP_ROWS : GL_UNPACK_SKIP_ROWS, &skiprows);
        glGetIntegerv(pack ? GL_PACK_SKIP_PIXELS : GL_UNPACK_SKIP_PIXELS, &skippix);
        if (!pack) {
            glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &imgh);
            glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skipimg);
        }
    }
    if (align < 1) align = 1;
    if (rowlen < 0) rowlen = 0;
    if (skiprows < 0) skiprows = 0;
    if (skippix < 0) skippix = 0;
    if (imgh < 0) imgh = 0;
    if (skipimg < 0) skipimg = 0;

    size_t row, ih, full_rows, last, total;
    if (__builtin_mul_overflow(pix, (size_t)(rowlen > 0 ? rowlen : w), &row))
        return SIZE_MAX;
    size_t rem = row % (size_t)align;
    if (rem) row += (size_t)align - rem;
    ih = (size_t)(imgh > 0 ? imgh : h);
    if (__builtin_add_overflow((size_t)skipimg, (size_t)(depth - 1), &full_rows) ||
        __builtin_mul_overflow(ih, full_rows, &full_rows) ||
        __builtin_add_overflow(full_rows, (size_t)skiprows, &full_rows) ||
        __builtin_add_overflow(full_rows, (size_t)(h - 1), &full_rows))
        return SIZE_MAX;
    if (__builtin_add_overflow((size_t)skippix, (size_t)w, &last) ||
        __builtin_mul_overflow(last, pix, &last) ||
        __builtin_mul_overflow(row, full_rows, &total) ||
        __builtin_add_overflow(total, last, &total))
        return SIZE_MAX;
    return total;
}

static gboolean
wgl_flip_fits(int w, int h, int bpp, size_t len)
{
    size_t row, total;
    if (w <= 0 || h <= 0 || bpp <= 0) return FALSE;
    if (__builtin_mul_overflow((size_t)w, (size_t)bpp, &row) ||
        __builtin_mul_overflow(row, (size_t)h, &total))
        return FALSE;
    return total <= len;
}

static uint8_t *
wgl_flip_rows(const uint8_t *src, int w, int h, int bpp)
{
    size_t row = (size_t)w * (size_t)bpp;
    uint8_t *dst = g_try_malloc(row * (size_t)h);
    if (!dst) return NULL;
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)y * row,
               src + (size_t)(h - 1 - y) * row, row);
    return dst;
}

static gboolean
wgl_flip_safe(ns_webgl *g, int w, int h, int format, int type,
              size_t need, size_t len)
{
    if (!g->unpack_flip_y || type != GL_UNSIGNED_BYTE) return FALSE;
    int bpp = wgl_components(format);
    size_t tight;
    if (__builtin_mul_overflow((size_t)w, (size_t)h, &tight) ||
        __builtin_mul_overflow(tight, (size_t)bpp, &tight))
        return FALSE;
    if (need != tight) return FALSE;
    return wgl_flip_fits(w, h, bpp, len);
}

#define WGL_GET(name) \
    ns_webgl *g = wgl_cur(ctx, this_val); \
    if (!g) return JS_UNDEFINED; \
    (void)g; (void)name; (void)argc; (void)argv

static JSValue
wgl_clearColor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glClearColor((float)argd(ctx, argc, argv, 0), (float)argd(ctx, argc, argv, 1),
                 (float)argd(ctx, argc, argv, 2), (float)argd(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_clearDepth(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glClearDepthf((float)argd(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_clearStencil(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glClearStencil(argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glClear((GLbitfield)argi(ctx, argc, argv, 0));
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_viewport(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glViewport(argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
               argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_scissor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glScissor(argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
              argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_enable(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glEnable((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_disable(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDisable((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_isEnabled(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    return JS_NewBool(ctx, glIsEnabled((GLenum)argi(ctx, argc, argv, 0)));
}

static JSValue
wgl_depthFunc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDepthFunc((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_depthMask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDepthMask(argbool(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_depthRange(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDepthRangef((float)argd(ctx, argc, argv, 0), (float)argd(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_colorMask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glColorMask(argbool(ctx, argc, argv, 0), argbool(ctx, argc, argv, 1),
                argbool(ctx, argc, argv, 2), argbool(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_stencilMask(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glStencilMask((GLuint)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_stencilFunc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glStencilFunc((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                  (GLuint)argi(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_stencilOp(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glStencilOp((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                (GLenum)argi(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_blendFunc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlendFunc((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_blendFuncSeparate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlendFuncSeparate((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                        (GLenum)argi(ctx, argc, argv, 2), (GLenum)argi(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_blendEquation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlendEquation((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_blendEquationSeparate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlendEquationSeparate((GLenum)argi(ctx, argc, argv, 0),
                            (GLenum)argi(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_blendColor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlendColor((float)argd(ctx, argc, argv, 0), (float)argd(ctx, argc, argv, 1),
                 (float)argd(ctx, argc, argv, 2), (float)argd(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_cullFace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCullFace((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_frontFace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glFrontFace((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_lineWidth(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glLineWidth((float)argd(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_polygonOffset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glPolygonOffset((float)argd(ctx, argc, argv, 0), (float)argd(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_hint(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glHint((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_finish(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    glFinish();
    return JS_UNDEFINED;
}

static JSValue
wgl_flush(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    glFlush();
    return JS_UNDEFINED;
}

static JSValue
wgl_pixelStorei(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    int pname = argi(ctx, argc, argv, 0);
    int param = argi(ctx, argc, argv, 1);
    if (pname == NS_UNPACK_FLIP_Y_WEBGL) {
        g->unpack_flip_y = param ? TRUE : FALSE;
    } else if (pname == NS_UNPACK_PREMULTIPLY_ALPHA_WEBGL) {
        g->premultiply = param ? TRUE : FALSE;
    } else if (pname == NS_UNPACK_COLORSPACE_CONVERSION_WEBGL) {
        /* no-op */
    } else if (g->version < 2 &&
               pname != GL_PACK_ALIGNMENT && pname != GL_UNPACK_ALIGNMENT) {
        glPixelStorei(0, 0);
    } else {
        glPixelStorei((GLenum)pname, param);
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_getError(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    return JS_NewInt32(ctx, (int)glGetError());
}

static GLint
wgl_param_cap(GLenum pname)
{
    switch (pname) {
    case GL_MAX_TEXTURE_SIZE:                 return 16384;
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE:        return 16384;
    case GL_MAX_RENDERBUFFER_SIZE:            return 16384;
    case GL_MAX_VERTEX_ATTRIBS:               return 16;
    case GL_MAX_VERTEX_UNIFORM_VECTORS:       return 1024;
    case GL_MAX_VARYING_VECTORS:              return 30;
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS:     return 1024;
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:   return 16;
    case GL_MAX_TEXTURE_IMAGE_UNITS:          return 16;
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: return 32;
#ifdef GL_MAX_3D_TEXTURE_SIZE
    case GL_MAX_3D_TEXTURE_SIZE:              return 2048;
#endif
#ifdef GL_MAX_ARRAY_TEXTURE_LAYERS
    case GL_MAX_ARRAY_TEXTURE_LAYERS:         return 2048;
#endif
#ifdef GL_MAX_DRAW_BUFFERS
    case GL_MAX_DRAW_BUFFERS:                 return 8;
#endif
#ifdef GL_MAX_COLOR_ATTACHMENTS
    case GL_MAX_COLOR_ATTACHMENTS:            return 8;
#endif
#ifdef GL_MAX_SAMPLES
    case GL_MAX_SAMPLES:                      return 4;
#endif
#ifdef GL_MAX_VERTEX_UNIFORM_COMPONENTS
    case GL_MAX_VERTEX_UNIFORM_COMPONENTS:    return 4096;
#endif
#ifdef GL_MAX_FRAGMENT_UNIFORM_COMPONENTS
    case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS:  return 4096;
#endif
#ifdef GL_MAX_VERTEX_OUTPUT_COMPONENTS
    case GL_MAX_VERTEX_OUTPUT_COMPONENTS:     return 64;
#endif
#ifdef GL_MAX_FRAGMENT_INPUT_COMPONENTS
    case GL_MAX_FRAGMENT_INPUT_COMPONENTS:    return 120;
#endif
#ifdef GL_MAX_VARYING_COMPONENTS
    case GL_MAX_VARYING_COMPONENTS:           return 120;
#endif
#ifdef GL_MAX_VERTEX_UNIFORM_BLOCKS
    case GL_MAX_VERTEX_UNIFORM_BLOCKS:        return 12;
#endif
#ifdef GL_MAX_FRAGMENT_UNIFORM_BLOCKS
    case GL_MAX_FRAGMENT_UNIFORM_BLOCKS:      return 12;
#endif
#ifdef GL_MAX_COMBINED_UNIFORM_BLOCKS
    case GL_MAX_COMBINED_UNIFORM_BLOCKS:      return 24;
#endif
#ifdef GL_MAX_UNIFORM_BUFFER_BINDINGS
    case GL_MAX_UNIFORM_BUFFER_BINDINGS:      return 24;
#endif
#ifdef GL_MAX_UNIFORM_BLOCK_SIZE
    case GL_MAX_UNIFORM_BLOCK_SIZE:           return 16384;
#endif
    default:                                  return 0;
    }
}

static JSValue
wgl_getParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 0);
    switch (pname) {
    case GL_VENDOR:
        return JS_NewString(ctx, "WebKit");
    case NS_UNMASKED_VENDOR_WEBGL:
        return JS_NewString(ctx, "Google Inc. (Intel)");
    case GL_RENDERER:
        return JS_NewString(ctx, "WebKit WebGL");
    case NS_UNMASKED_RENDERER_WEBGL:
        return JS_NewString(ctx,
            "ANGLE (Intel, Mesa Intel(R) UHD Graphics (CML GT2), OpenGL 4.6)");
    case GL_VERSION:
        return JS_NewString(ctx, g->version >= 2
            ? "WebGL 2.0 (OpenGL ES 3.0 Chromium)"
            : "WebGL 1.0 (OpenGL ES 2.0 Chromium)");
    case GL_SHADING_LANGUAGE_VERSION:
        return JS_NewString(ctx, g->version >= 2
            ? "WebGL GLSL ES 3.00 (OpenGL ES GLSL ES 3.0 Chromium)"
            : "WebGL GLSL ES 1.0 (OpenGL ES GLSL ES 1.0 Chromium)");
    case GL_VIEWPORT:
    case GL_SCISSOR_BOX:
    case GL_MAX_VIEWPORT_DIMS: {
        GLint v[4] = { 0, 0, 0, 0 };
        glGetIntegerv(pname, v);
        int n = (pname == GL_MAX_VIEWPORT_DIMS) ? 2 : 4;
        if (pname == GL_MAX_VIEWPORT_DIMS) {
            if (v[0] > 16384) v[0] = 16384;
            if (v[1] > 16384) v[1] = 16384;
        }
        JSValue a = JS_NewArrayBufferCopy(ctx, (const uint8_t *)v,
                                          (size_t)n * sizeof(GLint));
        JSValue ta = wgl_typed_array(ctx, a, JS_TYPED_ARRAY_INT32);
        JS_FreeValue(ctx, a);
        return ta;
    }
    case GL_COLOR_CLEAR_VALUE:
    case GL_DEPTH_CLEAR_VALUE:
    case GL_BLEND_COLOR:
    case GL_DEPTH_RANGE:
    case GL_ALIASED_LINE_WIDTH_RANGE:
    case GL_ALIASED_POINT_SIZE_RANGE: {
        GLfloat v[4] = { 0, 0, 0, 0 };
        glGetFloatv(pname, v);
        int n = (pname == GL_DEPTH_CLEAR_VALUE)                 ? 1
              : (pname == GL_COLOR_CLEAR_VALUE ||
                 pname == GL_BLEND_COLOR)                       ? 4
                                                               : 2;
        if (pname == GL_ALIASED_LINE_WIDTH_RANGE) {
            v[0] = 1.0f; v[1] = 1.0f;
        } else if (pname == GL_ALIASED_POINT_SIZE_RANGE) {
            v[0] = 1.0f;
            if (v[1] > 1024.0f) v[1] = 1024.0f;
        }
        JSValue a = JS_NewArrayBufferCopy(ctx, (const uint8_t *)v,
                                          (size_t)n * sizeof(GLfloat));
        JSValue ta = wgl_typed_array(ctx, a, JS_TYPED_ARRAY_FLOAT32);
        JS_FreeValue(ctx, a);
        return ta;
    }
    case GL_NUM_COMPRESSED_TEXTURE_FORMATS:
        return JS_NewInt32(ctx, 0);
    case NS_UNPACK_FLIP_Y_WEBGL:
        return JS_NewBool(ctx, g->unpack_flip_y);
    case NS_UNPACK_PREMULTIPLY_ALPHA_WEBGL:
        return JS_NewBool(ctx, g->premultiply);
    case GL_DEPTH_TEST:
    case GL_BLEND:
    case GL_CULL_FACE:
    case GL_STENCIL_TEST:
    case GL_SCISSOR_TEST:
    case GL_DITHER:
        return JS_NewBool(ctx, glIsEnabled(pname));
    case GL_COMPRESSED_TEXTURE_FORMATS: {
        GLint none = 0;
        JSValue a = JS_NewArrayBufferCopy(ctx, (const uint8_t *)&none, 0);
        JSValue ta = wgl_typed_array(ctx, a, JS_TYPED_ARRAY_UINT32);
        JS_FreeValue(ctx, a);
        return ta;
    }
    default: {
        GLint v[32] = { 0 };
        glGetIntegerv(pname, v);
        GLint cap = wgl_param_cap(pname);
        if (cap && v[0] > cap) v[0] = cap;
        return JS_NewInt32(ctx, v[0]);
    }
    }
}

static JSValue
wgl_getContextAttributes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    ns_webgl *g = JS_GetOpaque(this_val, ns_webgl_class_id);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "alpha", JS_NewBool(ctx, g ? g->alpha : TRUE));
    JS_SetPropertyStr(ctx, o, "depth", JS_NewBool(ctx, g ? g->depth : TRUE));
    JS_SetPropertyStr(ctx, o, "stencil", JS_NewBool(ctx, g ? g->stencil : FALSE));
    JS_SetPropertyStr(ctx, o, "antialias", JS_NewBool(ctx, g ? g->antialias : TRUE));
    JS_SetPropertyStr(ctx, o, "premultipliedAlpha",
                      JS_NewBool(ctx, g ? g->premultiplied_alpha : TRUE));
    JS_SetPropertyStr(ctx, o, "preserveDrawingBuffer", JS_NewBool(ctx, g ? g->preserve : FALSE));
    JS_SetPropertyStr(ctx, o, "powerPreference", JS_NewString(ctx, "default"));
    JS_SetPropertyStr(ctx, o, "failIfMajorPerformanceCaveat", JS_FALSE);
    JS_SetPropertyStr(ctx, o, "desynchronized", JS_FALSE);
    return o;
}

static JSValue
wgl_isContextLost(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_NewBool(ctx, FALSE);
}

static void set_const(JSContext *ctx, JSValueConst obj, const char *name,
                      int value);

static const char *const wgl_supported_extensions[] = {
    "WEBGL_debug_renderer_info",
    "EXT_texture_filter_anisotropic",
    NULL,
};

static JSValue
wgl_getSupportedExtensions(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; wgl_supported_extensions[i]; i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i,
                             JS_NewString(ctx, wgl_supported_extensions[i]));
    return arr;
}

static JSValue
wgl_getExtension(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;
    JSValue ext = JS_NULL;
    if (g_ascii_strcasecmp(name, "WEBGL_debug_renderer_info") == 0) {
        ext = JS_NewObject(ctx);
        set_const(ctx, ext, "UNMASKED_VENDOR_WEBGL", NS_UNMASKED_VENDOR_WEBGL);
        set_const(ctx, ext, "UNMASKED_RENDERER_WEBGL", NS_UNMASKED_RENDERER_WEBGL);
    } else if (g_ascii_strcasecmp(name, "EXT_texture_filter_anisotropic") == 0) {
        ext = JS_NewObject(ctx);
        set_const(ctx, ext, "MAX_TEXTURE_MAX_ANISOTROPY_EXT",
                  NS_MAX_TEXTURE_MAX_ANISOTROPY_EXT);
        set_const(ctx, ext, "TEXTURE_MAX_ANISOTROPY_EXT",
                  NS_TEXTURE_MAX_ANISOTROPY_EXT);
    }
    JS_FreeCString(ctx, name);
    return ext;
}

static JSValue
wgl_activeTexture(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glActiveTexture((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_createShader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint s = glCreateShader((GLenum)argi(ctx, argc, argv, 0));
    return wgl_wrap(ctx, s, "shader");
}

static JSValue
wgl_deleteShader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDeleteShader((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
wgl_shaderSource(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    if (argc < 2) return JS_UNDEFINED;
    size_t slen = 0;
    const char *src = JS_ToCStringLen(ctx, &slen, argv[1]);
    if (src) {
        if (slen <= NS_WEBGL_MAX_SHADER) {
            const GLchar *p = src;
            GLint plen = (GLint)slen;
#ifdef NS_HAVE_CGL
            char *prefixed = NULL;
            if (!strstr(src, "#version")) {
                prefixed = g_strconcat("#version 100\n", src, NULL);
                p = prefixed;
                plen = (GLint)strlen(prefixed);
            }
            glShaderSource((GLuint)wgl_name(ctx, argv[0]), 1, &p, &plen);
            g_free(prefixed);
#else
            glShaderSource((GLuint)wgl_name(ctx, argv[0]), 1, &p, &plen);
#endif
        }
        JS_FreeCString(ctx, src);
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_compileShader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCompileShader((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
wgl_getShaderParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 1);
    GLint v = 0;
    glGetShaderiv((GLuint)wgl_name(ctx, argv[0]), pname, &v);
    if (pname == GL_COMPILE_STATUS || pname == GL_DELETE_STATUS)
        return JS_NewBool(ctx, v);
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_gl_log(JSContext *ctx, GLuint name,
           void (*get_iv)(GLuint, GLenum, GLint *), GLenum len_pname,
           void (*get_str)(GLuint, GLsizei, GLsizei *, GLchar *))
{
    GLint len = 0;
    get_iv(name, len_pname, &len);
    if (len <= 0) return JS_NewString(ctx, "");
    char *buf = g_malloc((size_t)len + 1);
    get_str(name, len, NULL, buf);
    buf[len] = 0;
    JSValue r = JS_NewString(ctx, buf);
    g_free(buf);
    return r;
}

static JSValue
wgl_getShaderInfoLog(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    return wgl_gl_log(ctx, (GLuint)wgl_name(ctx, argv[0]),
                      glGetShaderiv, GL_INFO_LOG_LENGTH, glGetShaderInfoLog);
}

static JSValue
wgl_getShaderSource(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    return wgl_gl_log(ctx, (GLuint)wgl_name(ctx, argv[0]),
                      glGetShaderiv, GL_SHADER_SOURCE_LENGTH, glGetShaderSource);
}

static JSValue
wgl_createProgram(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    return wgl_wrap(ctx, glCreateProgram(), "program");
}

static JSValue
wgl_deleteProgram(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDeleteProgram((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
wgl_attachShader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glAttachShader((GLuint)wgl_name(ctx, argv[0]), (GLuint)wgl_name(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
wgl_detachShader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDetachShader((GLuint)wgl_name(ctx, argv[0]), (GLuint)wgl_name(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
wgl_linkProgram(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glLinkProgram((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
wgl_validateProgram(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glValidateProgram((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
wgl_useProgram(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glUseProgram((GLuint)wgl_name(ctx, argv[0]));
    return JS_UNDEFINED;
}

static JSValue
wgl_getProgramParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 1);
    GLint v = 0;
    glGetProgramiv((GLuint)wgl_name(ctx, argv[0]), pname, &v);
    if (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS ||
        pname == GL_DELETE_STATUS)
        return JS_NewBool(ctx, v);
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_getProgramInfoLog(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    return wgl_gl_log(ctx, (GLuint)wgl_name(ctx, argv[0]),
                      glGetProgramiv, GL_INFO_LOG_LENGTH, glGetProgramInfoLog);
}

static JSValue
wgl_bindAttribLocation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    const char *name = argc >= 3 ? JS_ToCString(ctx, argv[2]) : NULL;
    if (name) {
        glBindAttribLocation((GLuint)wgl_name(ctx, argv[0]),
                             (GLuint)argi(ctx, argc, argv, 1), name);
        JS_FreeCString(ctx, name);
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_getAttribLocation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    const char *name = argc >= 2 ? JS_ToCString(ctx, argv[1]) : NULL;
    GLint loc = -1;
    if (name) {
        loc = glGetAttribLocation((GLuint)wgl_name(ctx, argv[0]), name);
        JS_FreeCString(ctx, name);
    }
    return JS_NewInt32(ctx, loc);
}

static JSValue
wgl_getUniformLocation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    const char *name = argc >= 2 ? JS_ToCString(ctx, argv[1]) : NULL;
    GLint loc = -1;
    if (name) {
        loc = glGetUniformLocation((GLuint)wgl_name(ctx, argv[0]), name);
        JS_FreeCString(ctx, name);
    }
    if (loc < 0) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "_loc", JS_NewInt32(ctx, loc));
    return o;
}

static JSValue
wgl_active_var(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
               void (*fn)(GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *))
{
    WGL_GET(0);
    char name[256] = { 0 };
    GLint size = 0;
    GLenum type = 0;
    fn((GLuint)wgl_name(ctx, argv[0]), (GLuint)argi(ctx, argc, argv, 1),
       sizeof(name) - 1, NULL, &size, &type, name);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt32(ctx, size));
    JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, (int)type));
    JS_SetPropertyStr(ctx, o, "name", JS_NewString(ctx, name));
    return o;
}

static JSValue wgl_getActiveAttrib(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_active_var(c, t, a, v, glGetActiveAttrib); }
static JSValue wgl_getActiveUniform(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_active_var(c, t, a, v, glGetActiveUniform); }

static JSValue
wgl_getShaderPrecisionFormat(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum shader_type = (GLenum)argi(ctx, argc, argv, 0);
    GLenum precision_type = (GLenum)argi(ctx, argc, argv, 1);
    GLint range[2] = { 0, 0 };
    GLint precision = 0;
    glGetShaderPrecisionFormat(shader_type, precision_type, range, &precision);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "rangeMin", JS_NewInt32(ctx, range[0]));
    JS_SetPropertyStr(ctx, o, "rangeMax", JS_NewInt32(ctx, range[1]));
    JS_SetPropertyStr(ctx, o, "precision", JS_NewInt32(ctx, precision));
    return o;
}

static JSValue
wgl_gen_obj(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
            const char *kind, void (*gen)(GLsizei, GLuint *))
{
    WGL_GET(0);
    GLuint n = 0;
    gen(1, &n);
    return wgl_wrap(ctx, n, kind);
}

static JSValue
wgl_del_obj(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
            void (*del)(GLsizei, const GLuint *))
{
    WGL_GET(0);
    GLuint n = (GLuint)wgl_name(ctx, argv[0]);
    del(1, &n);
    return JS_UNDEFINED;
}

static JSValue wgl_createBuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "buffer", glGenBuffers); }
static JSValue
wgl_deleteBuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint n = argc >= 1 ? (GLuint)wgl_name(ctx, argv[0]) : 0;
    if (n) {
        if (g->bound_array_buffer == n) g->bound_array_buffer = 0;
        if (g->bound_element_array_buffer == n) g->bound_element_array_buffer = 0;
        if (g->bound_buffers) {
            GHashTableIter it;
            gpointer key, value;
            g_hash_table_iter_init(&it, g->bound_buffers);
            while (g_hash_table_iter_next(&it, &key, &value)) {
                if (GPOINTER_TO_UINT(value) == n)
                    g_hash_table_iter_remove(&it);
            }
        }
        if (g->buffer_sizes)
            g_hash_table_remove(g->buffer_sizes, GUINT_TO_POINTER(n));
        glDeleteBuffers(1, &n);
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_bindBuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLuint n = (GLuint)wgl_name(ctx, argv[1]);
    glBindBuffer(target, n);
    wgl_set_bound_buffer(g, target, n);
    return JS_UNDEFINED;
}

static gboolean
wgl_buffer_range_ok(ns_webgl *g, GLenum target, GLintptr offset, size_t len)
{
    if (offset < 0) return FALSE;
    size_t size = wgl_buffer_size(g, wgl_bound_buffer(g, target));
    if (size == 0) return FALSE;
    uint64_t end;
    if (__builtin_add_overflow((uint64_t)offset, (uint64_t)len, &end))
        return FALSE;
    return end <= size;
}

static JSValue
wgl_bufferData(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLenum usage = (GLenum)argi(ctx, argc, argv, 2);
    if (argc >= 2 && JS_IsNumber(argv[1])) {
        int64_t size = 0;
        JS_ToInt64(ctx, &size, argv[1]);
        if (size < 0 || (uint64_t)size > NS_WEBGL_MAX_ALLOC) return JS_UNDEFINED;
        glBufferData(target, (GLsizeiptr)size, NULL, usage);
        wgl_set_buffer_size(g, wgl_bound_buffer(g, target), (size_t)size);
        return JS_UNDEFINED;
    }
    JSValue hold;
    size_t len = 0;
    const uint8_t *p = (argc >= 2) ? view_bytes(ctx, argv[1], &len, &hold) : NULL;
    if (len > NS_WEBGL_MAX_ALLOC) {
        if (p && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return JS_UNDEFINED;
    }
    glBufferData(target, (GLsizeiptr)len, p, usage);
    wgl_set_buffer_size(g, wgl_bound_buffer(g, target), len);
    if (p && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static JSValue
wgl_bufferSubData(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 1);
    JSValue hold;
    size_t len = 0;
    const uint8_t *p = (argc >= 3) ? view_bytes(ctx, argv[2], &len, &hold) : NULL;
    if (p) {
        if (wgl_buffer_range_ok(g, target, offset, len))
            glBufferSubData(target, offset, (GLsizeiptr)len, p);
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_enableVertexAttribArray(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glEnableVertexAttribArray((GLuint)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_disableVertexAttribArray(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glDisableVertexAttribArray((GLuint)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribPointer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    GLint size = argi(ctx, argc, argv, 1);
    GLenum type = (GLenum)argi(ctx, argc, argv, 2);
    GLboolean norm = argbool(ctx, argc, argv, 3);
    GLsizei stride = argi(ctx, argc, argv, 4);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 5);
    if (size < 0 || stride < 0 || offset < 0) return JS_UNDEFINED;
    glVertexAttribPointer(index, size, type, norm, stride,
                          (const void *)offset);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttrib_f(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    float v[4] = { 0, 0, 0, 1 };
    for (int i = 0; i < n; i++) v[i] = (float)argd(ctx, argc, argv, i + 1);
    switch (n) {
    case 1: glVertexAttrib1f(index, v[0]); break;
    case 2: glVertexAttrib2f(index, v[0], v[1]); break;
    case 3: glVertexAttrib3f(index, v[0], v[1], v[2]); break;
    default: glVertexAttrib4f(index, v[0], v[1], v[2], v[3]); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_vertexAttrib1f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_f(c, t, a, v, 1); }
static JSValue wgl_vertexAttrib2f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_f(c, t, a, v, 2); }
static JSValue wgl_vertexAttrib3f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_f(c, t, a, v, 3); }
static JSValue wgl_vertexAttrib4f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_f(c, t, a, v, 4); }

static JSValue
wgl_uniform_f(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    float v[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) v[i] = (float)argd(ctx, argc, argv, i + 1);
    switch (n) {
    case 1: glUniform1f(loc, v[0]); break;
    case 2: glUniform2f(loc, v[0], v[1]); break;
    case 3: glUniform3f(loc, v[0], v[1], v[2]); break;
    default: glUniform4f(loc, v[0], v[1], v[2], v[3]); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_f(c, t, a, v, 1); }
static JSValue wgl_uniform2f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_f(c, t, a, v, 2); }
static JSValue wgl_uniform3f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_f(c, t, a, v, 3); }
static JSValue wgl_uniform4f(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_f(c, t, a, v, 4); }

static JSValue
wgl_uniform_i(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    GLint v[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) v[i] = argi(ctx, argc, argv, i + 1);
    switch (n) {
    case 1: glUniform1i(loc, v[0]); break;
    case 2: glUniform2i(loc, v[0], v[1]); break;
    case 3: glUniform3i(loc, v[0], v[1], v[2]); break;
    default: glUniform4i(loc, v[0], v[1], v[2], v[3]); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1i(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_i(c, t, a, v, 1); }
static JSValue wgl_uniform2i(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_i(c, t, a, v, 2); }
static JSValue wgl_uniform3i(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_i(c, t, a, v, 3); }
static JSValue wgl_uniform4i(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_i(c, t, a, v, 4); }

static JSValue
wgl_uniform_fv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    float buf[4096];
    int cnt = (argc >= 2) ? wgl_floats(ctx, argv[1], buf, 4096) : 0;
    GLsizei count = cnt / n;
    switch (n) {
    case 1: glUniform1fv(loc, count, buf); break;
    case 2: glUniform2fv(loc, count, buf); break;
    case 3: glUniform3fv(loc, count, buf); break;
    default: glUniform4fv(loc, count, buf); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_fv(c, t, a, v, 1); }
static JSValue wgl_uniform2fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_fv(c, t, a, v, 2); }
static JSValue wgl_uniform3fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_fv(c, t, a, v, 3); }
static JSValue wgl_uniform4fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_fv(c, t, a, v, 4); }

static JSValue
wgl_uniform_iv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    GLint buf[4096];
    int cnt = (argc >= 2) ? wgl_ints(ctx, argv[1], buf, 4096) : 0;
    GLsizei count = cnt / n;
    switch (n) {
    case 1: glUniform1iv(loc, count, buf); break;
    case 2: glUniform2iv(loc, count, buf); break;
    case 3: glUniform3iv(loc, count, buf); break;
    default: glUniform4iv(loc, count, buf); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1iv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_iv(c, t, a, v, 1); }
static JSValue wgl_uniform2iv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_iv(c, t, a, v, 2); }
static JSValue wgl_uniform3iv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_iv(c, t, a, v, 3); }
static JSValue wgl_uniform4iv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_iv(c, t, a, v, 4); }

static JSValue
wgl_uniformMatrix_fv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    float buf[4096];
    int cnt = (argc >= 3) ? wgl_floats(ctx, argv[2], buf, 4096) : 0;
    GLsizei count = cnt / (n * n);
    switch (n) {
    case 2: glUniformMatrix2fv(loc, count, GL_FALSE, buf); break;
    case 3: glUniformMatrix3fv(loc, count, GL_FALSE, buf); break;
    default: glUniformMatrix4fv(loc, count, GL_FALSE, buf); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniformMatrix2fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_fv(c, t, a, v, 2); }
static JSValue wgl_uniformMatrix3fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_fv(c, t, a, v, 3); }
static JSValue wgl_uniformMatrix4fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_fv(c, t, a, v, 4); }

static int
wgl_index_bytes(GLenum type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:  return 1;
    case GL_UNSIGNED_SHORT: return 2;
    case GL_UNSIGNED_INT:   return 4;
    default:                return 0;
    }
}

static gboolean
wgl_elements_in_range(ns_webgl *g, GLsizei count, GLenum type, GLintptr offset)
{
    if (count < 0 || offset < 0) return FALSE;
    int isz = wgl_index_bytes(type);
    if (!isz) return FALSE;
    size_t size = wgl_buffer_size(g, g ? g->bound_element_array_buffer : 0);
    if (size == 0) return FALSE;
    uint64_t span;
    if (__builtin_mul_overflow((uint64_t)count, (uint64_t)isz, &span) ||
        __builtin_add_overflow(span, (uint64_t)offset, &span))
        return FALSE;
    return span <= size;
}

static JSValue
wgl_drawArrays(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint first = argi(ctx, argc, argv, 1);
    GLsizei count = argi(ctx, argc, argv, 2);
    if (first < 0 || count < 0) return JS_UNDEFINED;
    glDrawArrays((GLenum)argi(ctx, argc, argv, 0), first, count);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_drawElements(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLsizei count = argi(ctx, argc, argv, 1);
    GLenum type = (GLenum)argi(ctx, argc, argv, 2);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 3);
    if (!wgl_elements_in_range(g, count, type, offset)) return JS_UNDEFINED;
    glDrawElements((GLenum)argi(ctx, argc, argv, 0), count, type,
                   (const void *)offset);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue wgl_createTexture(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "texture", glGenTextures); }
static JSValue wgl_deleteTexture(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteTextures); }

static JSValue
wgl_bindTexture(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindTexture((GLenum)argi(ctx, argc, argv, 0), (GLuint)wgl_name(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
wgl_texParameteri(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glTexParameteri((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                    argi(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_texParameterf(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glTexParameterf((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                    (float)argd(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_generateMipmap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glGenerateMipmap((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static const uint8_t *
wgl_imagedata_bytes(JSContext *ctx, JSValueConst src, int *w, int *h,
                    size_t *out_len, JSValue *hold)
{
    *hold = JS_UNDEFINED;
    *out_len = 0;
    JSValue wv = JS_GetPropertyStr(ctx, src, "width");
    JSValue hv = JS_GetPropertyStr(ctx, src, "height");
    JSValue dv = JS_GetPropertyStr(ctx, src, "data");
    int32_t iw = 0, ih = 0;
    JS_ToInt32(ctx, &iw, wv);
    JS_ToInt32(ctx, &ih, hv);
    JS_FreeValue(ctx, wv);
    JS_FreeValue(ctx, hv);
    *w = iw;
    *h = ih;
    if (iw <= 0 || ih <= 0 || !JS_IsObject(dv)) {
        JS_FreeValue(ctx, dv);
        return NULL;
    }
    size_t len = 0;
    const uint8_t *p = view_bytes(ctx, dv, &len, hold);
    JS_FreeValue(ctx, dv);
    *out_len = len;
    return p;
}

static uint8_t *
wgl_source_rgba(JSContext *ctx, JSValueConst src, int format,
                gboolean flip_y, gboolean premultiply, int *out_w, int *out_h)
{
    int w = 0, h = 0;
    cairo_surface_t *s = ns_js_drawimage_source_surface(ctx, src, &w, &h);
    if (!s) return NULL;
    const unsigned char *data = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    if (w <= 0 || h <= 0 || !data) {
        cairo_surface_destroy(s);
        return NULL;
    }
    int comps = wgl_components(format);
    guint64 total = (guint64)w * (guint64)h * (guint64)comps;
    if (total == 0 || total > NS_WEBGL_MAX_ALLOC) {
        cairo_surface_destroy(s);
        return NULL;
    }
    uint8_t *out = g_try_malloc((size_t)total);
    if (!out) {
        cairo_surface_destroy(s);
        return NULL;
    }
    for (int y = 0; y < h; y++) {
        const unsigned char *srow = data + (size_t)(flip_y ? h - 1 - y : y) * stride;
        uint8_t *orow = out + (size_t)y * (size_t)w * (size_t)comps;
        for (int x = 0; x < w; x++) {
            const unsigned char *p = srow + x * 4;
            unsigned b = p[0], gg = p[1], r = p[2], a = p[3];
            if (!premultiply && a > 0 && a < 255) {
                r = (r * 255u + a / 2) / a;
                gg = (gg * 255u + a / 2) / a;
                b = (b * 255u + a / 2) / a;
                if (r > 255) r = 255;
                if (gg > 255) gg = 255;
                if (b > 255) b = 255;
            }
            uint8_t *o = orow + x * comps;
            switch (format) {
            case GL_RGBA: o[0] = (uint8_t)r; o[1] = (uint8_t)gg; o[2] = (uint8_t)b; o[3] = (uint8_t)a; break;
            case GL_RGB:  o[0] = (uint8_t)r; o[1] = (uint8_t)gg; o[2] = (uint8_t)b; break;
            case GL_LUMINANCE_ALPHA:
                o[0] = (uint8_t)((r * 77 + gg * 150 + b * 29) >> 8); o[1] = (uint8_t)a; break;
            case GL_ALPHA: o[0] = (uint8_t)a; break;
            default: o[0] = (uint8_t)((r * 77 + gg * 150 + b * 29) >> 8); break;
            }
        }
    }
    cairo_surface_destroy(s);
    *out_w = w;
    *out_h = h;
    return out;
}

static JSValue
wgl_texImage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLint level = argi(ctx, argc, argv, 1);
    GLint internalformat = argi(ctx, argc, argv, 2);

    if (argc >= 9 && JS_IsNumber(argv[3])) {
        GLsizei w = argi(ctx, argc, argv, 3);
        GLsizei h = argi(ctx, argc, argv, 4);
        GLint border = argi(ctx, argc, argv, 5);
        GLenum format = (GLenum)argi(ctx, argc, argv, 6);
        GLenum type = (GLenum)argi(ctx, argc, argv, 7);
        JSValue hold = JS_UNDEFINED;
        size_t len = 0;
        const uint8_t *px = NULL;
        if (!JS_IsNull(argv[8]) && !JS_IsUndefined(argv[8]))
            px = view_bytes(ctx, argv[8], &len, &hold);
        size_t need = wgl_transfer_bytes(g, w, h, 1, format, type, FALSE);
        if (need > NS_WEBGL_MAX_ALLOC || (px && len < need)) {
            if (px && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
            return JS_UNDEFINED;
        }
        uint8_t *flipped = NULL;
        uint8_t *zero = NULL;
        if (!px) {
            zero = need ? g_try_malloc0(need) : NULL;
            px = zero;
        } else if (wgl_flip_safe(g, w, h, format, type, need, len)) {
            flipped = wgl_flip_rows(px, w, h, wgl_components(format));
            if (flipped) px = flipped;
        }
        glTexImage2D(target, level, internalformat, w, h, border, format, type, px);
        g_free(flipped);
        g_free(zero);
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return JS_UNDEFINED;
    }

    GLenum format = (GLenum)argi(ctx, argc, argv, 3);
    GLenum type = (GLenum)argi(ctx, argc, argv, 4);
    if (argc >= 6 && JS_IsObject(argv[5])) {
        int w = 0, h = 0;
        size_t len = 0;
        JSValue hold;
        const uint8_t *px = wgl_imagedata_bytes(ctx, argv[5], &w, &h, &len, &hold);
        size_t need = (px && w > 0 && h > 0)
            ? wgl_transfer_bytes(g, w, h, 1, format, type, FALSE) : 0;
        if (px && w > 0 && h > 0 && need > 0 && need <= NS_WEBGL_MAX_ALLOC &&
            len >= need) {
            uint8_t *flipped = NULL;
            if (wgl_flip_safe(g, w, h, format, type, need, len)) {
                flipped = wgl_flip_rows(px, w, h, wgl_components(format));
                if (flipped) px = flipped;
            }
            glTexImage2D(target, level, internalformat, w, h, 0, format, type, px);
            g_free(flipped);
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        } else if (px) {
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        } else if (type == GL_UNSIGNED_BYTE) {
            uint8_t *rgba = wgl_source_rgba(ctx, argv[5], format,
                                            g->unpack_flip_y, g->premultiply, &w, &h);
            if (rgba) {
                glTexImage2D(target, level, internalformat, w, h, 0, format, type, rgba);
                g_free(rgba);
            }
        }
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_texSubImage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLint level = argi(ctx, argc, argv, 1);
    GLint xoff = argi(ctx, argc, argv, 2);
    GLint yoff = argi(ctx, argc, argv, 3);

    if (argc >= 9 && JS_IsNumber(argv[4])) {
        GLsizei w = argi(ctx, argc, argv, 4);
        GLsizei h = argi(ctx, argc, argv, 5);
        GLenum format = (GLenum)argi(ctx, argc, argv, 6);
        GLenum type = (GLenum)argi(ctx, argc, argv, 7);
        JSValue hold = JS_UNDEFINED;
        size_t len = 0;
        const uint8_t *px = NULL;
        if (!JS_IsNull(argv[8]) && !JS_IsUndefined(argv[8]))
            px = view_bytes(ctx, argv[8], &len, &hold);
        size_t need = wgl_transfer_bytes(g, w, h, 1, format, type, FALSE);
        if (need > NS_WEBGL_MAX_ALLOC || (px && len < need)) {
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
            return JS_UNDEFINED;
        }
        uint8_t *flipped = NULL;
        if (px && wgl_flip_safe(g, w, h, format, type, need, len)) {
            flipped = wgl_flip_rows(px, w, h, wgl_components(format));
            if (flipped) px = flipped;
        }
        if (px)
            glTexSubImage2D(target, level, xoff, yoff, w, h, format, type, px);
        g_free(flipped);
        if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return JS_UNDEFINED;
    }

    GLenum format = (GLenum)argi(ctx, argc, argv, 4);
    GLenum type = (GLenum)argi(ctx, argc, argv, 5);
    if (argc >= 7 && JS_IsObject(argv[6])) {
        int w = 0, h = 0;
        size_t len = 0;
        JSValue hold;
        const uint8_t *px = wgl_imagedata_bytes(ctx, argv[6], &w, &h, &len, &hold);
        size_t need = (px && w > 0 && h > 0)
            ? wgl_transfer_bytes(g, w, h, 1, format, type, FALSE) : 0;
        if (px && w > 0 && h > 0 && need > 0 && need <= NS_WEBGL_MAX_ALLOC &&
            len >= need) {
            uint8_t *flipped = NULL;
            if (wgl_flip_safe(g, w, h, format, type, need, len)) {
                flipped = wgl_flip_rows(px, w, h, wgl_components(format));
                if (flipped) px = flipped;
            }
            glTexSubImage2D(target, level, xoff, yoff, w, h, format, type, px);
            g_free(flipped);
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        } else if (px) {
            if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        } else if (type == GL_UNSIGNED_BYTE) {
            uint8_t *rgba = wgl_source_rgba(ctx, argv[6], format,
                                            g->unpack_flip_y, g->premultiply, &w, &h);
            if (rgba) {
                glTexSubImage2D(target, level, xoff, yoff, w, h, format, type, rgba);
                g_free(rgba);
            }
        }
    }
    return JS_UNDEFINED;
}

static JSValue wgl_createFramebuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "framebuffer", glGenFramebuffers); }
static JSValue wgl_deleteFramebuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{
    ns_webgl *g = wgl_cur(c, t);
    if (g && a >= 1) {
        GLuint n = (GLuint)wgl_name(c, v[0]);
        if (n && g->user_draw_fbo == n) g->user_draw_fbo = 0;
        if (n && g->user_read_fbo == n) g->user_read_fbo = 0;
        if (n && g->bound_draw_fbo == n) g->bound_draw_fbo = 0;
        if (n && g->bound_read_fbo == n) g->bound_read_fbo = 0;
    }
    return wgl_del_obj(c, t, a, v, glDeleteFramebuffers);
}

static JSValue
wgl_bindFramebuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLuint f = (argc >= 2) ? (GLuint)wgl_name(ctx, argv[1]) : 0;
    if (target == GL_FRAMEBUFFER) {
        g->user_draw_fbo = f;
        g->user_read_fbo = f;
    } else if (target == GL_DRAW_FRAMEBUFFER) {
        g->user_draw_fbo = f;
    } else if (target == GL_READ_FRAMEBUFFER) {
        g->user_read_fbo = f;
    }
    wgl_bind_framebuffer(g, target, f ? f : ns_webgl_draw_target(g));
    return JS_UNDEFINED;
}

static JSValue
wgl_framebufferTexture2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glFramebufferTexture2D((GLenum)argi(ctx, argc, argv, 0),
                           (GLenum)argi(ctx, argc, argv, 1),
                           (GLenum)argi(ctx, argc, argv, 2),
                           (GLuint)wgl_name(ctx, argv[3]),
                           argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_framebufferRenderbuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glFramebufferRenderbuffer((GLenum)argi(ctx, argc, argv, 0),
                              (GLenum)argi(ctx, argc, argv, 1),
                              (GLenum)argi(ctx, argc, argv, 2),
                              (GLuint)wgl_name(ctx, argv[3]));
    return JS_UNDEFINED;
}

static JSValue
wgl_checkFramebufferStatus(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    return JS_NewInt32(ctx, (int)glCheckFramebufferStatus((GLenum)argi(ctx, argc, argv, 0)));
}

static JSValue wgl_createRenderbuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "renderbuffer", glGenRenderbuffers); }
static JSValue wgl_deleteRenderbuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteRenderbuffers); }

static JSValue
wgl_bindRenderbuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindRenderbuffer((GLenum)argi(ctx, argc, argv, 0), (GLuint)wgl_name(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
wgl_renderbufferStorage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glRenderbufferStorage((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                          argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3));
    return JS_UNDEFINED;
}

static JSValue
wgl_readPixels(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint x = argi(ctx, argc, argv, 0);
    GLint y = argi(ctx, argc, argv, 1);
    GLsizei w = argi(ctx, argc, argv, 2);
    GLsizei h = argi(ctx, argc, argv, 3);
    GLenum format = (GLenum)argi(ctx, argc, argv, 4);
    GLenum type = (GLenum)argi(ctx, argc, argv, 5);
    if (argc < 7 || !JS_IsObject(argv[6])) return JS_UNDEFINED;
    JSValue hold;
    size_t len = 0;
    const uint8_t *p = view_bytes(ctx, argv[6], &len, &hold);
    size_t need = wgl_transfer_bytes(g, w, h, 1, format, type, TRUE);
    if (p && len >= need && need > 0) {
        GLint bound = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
        gboolean resolved = FALSE;
        if (g->samples > 1 && (GLuint)bound == g->draw_fbo) {
            wgl_bind_framebuffer(g, GL_READ_FRAMEBUFFER, g->draw_fbo);
            wgl_bind_framebuffer(g, GL_DRAW_FRAMEBUFFER, g->fbo);
            glBlitFramebuffer(0, 0, g->w, g->h, 0, 0, g->w, g->h,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->fbo);
            resolved = TRUE;
        }
        glReadPixels(x, y, w, h, format, type, (void *)p);
        if (resolved)
            wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->draw_fbo);
    }
    if (p && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static int
wgl_uints(JSContext *ctx, JSValueConst v, GLuint *out, int max)
{
    return wgl_ints(ctx, v, (GLint *)out, max);
}

static JSValue
wgl_is_obj(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
           GLboolean (*fn)(GLuint))
{
    WGL_GET(0);
    GLuint n = (argc >= 1) ? (GLuint)wgl_name(ctx, argv[0]) : 0;
    return JS_NewBool(ctx, n && fn(n));
}

static JSValue wgl_isBuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsBuffer); }
static JSValue wgl_isProgram(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsProgram); }
static JSValue wgl_isShader(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsShader); }
static JSValue wgl_isTexture(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsTexture); }
static JSValue wgl_isFramebuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsFramebuffer); }
static JSValue wgl_isRenderbuffer(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsRenderbuffer); }

static JSValue
wgl_get_target_iv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                  void (*fn)(GLenum, GLenum, GLint *))
{
    WGL_GET(0);
    GLint v = 0;
    fn((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1), &v);
    return JS_NewInt32(ctx, v);
}

static JSValue wgl_getBufferParameter(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_get_target_iv(c, t, a, v, glGetBufferParameteriv); }
static JSValue wgl_getTexParameter(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_get_target_iv(c, t, a, v, glGetTexParameteriv); }
static JSValue wgl_getRenderbufferParameter(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_get_target_iv(c, t, a, v, glGetRenderbufferParameteriv); }

static JSValue
wgl_getFramebufferAttachmentParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint v = 0;
    glGetFramebufferAttachmentParameteriv((GLenum)argi(ctx, argc, argv, 0),
                                          (GLenum)argi(ctx, argc, argv, 1),
                                          (GLenum)argi(ctx, argc, argv, 2), &v);
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_getVertexAttrib(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 1);
    if (pname == GL_CURRENT_VERTEX_ATTRIB) {
        GLfloat v[4] = { 0, 0, 0, 0 };
        glGetVertexAttribfv(index, pname, v);
        JSValue a = JS_NewArrayBufferCopy(ctx, (const uint8_t *)v, sizeof(v));
        JSValue ta = wgl_typed_array(ctx, a, JS_TYPED_ARRAY_FLOAT32);
        JS_FreeValue(ctx, a);
        return ta;
    }
    GLint v = 0;
    glGetVertexAttribiv(index, pname, &v);
    if (pname == GL_VERTEX_ATTRIB_ARRAY_ENABLED ||
        pname == GL_VERTEX_ATTRIB_ARRAY_NORMALIZED)
        return JS_NewBool(ctx, v);
    if (pname == GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING)
        return v ? wgl_wrap(ctx, (GLuint)v, "buffer") : JS_NULL;
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_getVertexAttribOffset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    void *p = NULL;
    glGetVertexAttribPointerv((GLuint)argi(ctx, argc, argv, 0),
                              (GLenum)argi(ctx, argc, argv, 1), &p);
    return JS_NewInt32(ctx, (int)(intptr_t)p);
}

static JSValue
wgl_getUniform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    return JS_NULL;
}

static JSValue
wgl_vertexAttrib_fv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLuint index = (GLuint)argi(ctx, argc, argv, 0);
    float v[4] = { 0, 0, 0, 1 };
    if (argc >= 2) wgl_floats(ctx, argv[1], v, n);
    switch (n) {
    case 1: glVertexAttrib1fv(index, v); break;
    case 2: glVertexAttrib2fv(index, v); break;
    case 3: glVertexAttrib3fv(index, v); break;
    default: glVertexAttrib4fv(index, v); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_vertexAttrib1fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_fv(c, t, a, v, 1); }
static JSValue wgl_vertexAttrib2fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_fv(c, t, a, v, 2); }
static JSValue wgl_vertexAttrib3fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_fv(c, t, a, v, 3); }
static JSValue wgl_vertexAttrib4fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_vertexAttrib_fv(c, t, a, v, 4); }

static JSValue wgl_createVertexArray(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "vertexarray", glGenVertexArrays); }
static JSValue wgl_deleteVertexArray(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteVertexArrays); }

static JSValue
wgl_bindVertexArray(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindVertexArray(argc >= 1 ? (GLuint)wgl_name(ctx, argv[0]) : 0);
    return JS_UNDEFINED;
}

static JSValue
wgl_isVertexArray(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsVertexArray); }

static JSValue
wgl_drawArraysInstanced(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint first = argi(ctx, argc, argv, 1);
    GLsizei count = argi(ctx, argc, argv, 2);
    GLsizei instances = argi(ctx, argc, argv, 3);
    if (first < 0 || count < 0 || instances < 0) return JS_UNDEFINED;
    glDrawArraysInstanced((GLenum)argi(ctx, argc, argv, 0), first, count, instances);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_drawElementsInstanced(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLsizei count = argi(ctx, argc, argv, 1);
    GLenum type = (GLenum)argi(ctx, argc, argv, 2);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 3);
    GLsizei instances = argi(ctx, argc, argv, 4);
    if (instances < 0 || !wgl_elements_in_range(g, count, type, offset))
        return JS_UNDEFINED;
    glDrawElementsInstanced((GLenum)argi(ctx, argc, argv, 0), count, type,
                            (const void *)offset, instances);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribDivisor(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glVertexAttribDivisor((GLuint)argi(ctx, argc, argv, 0), (GLuint)argi(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue
wgl_drawBuffers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint bufs[16];
    int n = (argc >= 1) ? wgl_ints(ctx, argv[0], bufs, 16) : 0;
    if (n > 0) glDrawBuffers(n, (const GLenum *)bufs);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribIPointer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint size = argi(ctx, argc, argv, 1);
    GLsizei stride = argi(ctx, argc, argv, 3);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 4);
    if (size < 0 || stride < 0 || offset < 0) return JS_UNDEFINED;
    glVertexAttribIPointer((GLuint)argi(ctx, argc, argv, 0), size,
                           (GLenum)argi(ctx, argc, argv, 2), stride,
                           (const void *)offset);
    return JS_UNDEFINED;
}

static JSValue
wgl_uniform_ui(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    GLuint v[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) v[i] = (GLuint)argi(ctx, argc, argv, i + 1);
    switch (n) {
    case 1: glUniform1ui(loc, v[0]); break;
    case 2: glUniform2ui(loc, v[0], v[1]); break;
    case 3: glUniform3ui(loc, v[0], v[1], v[2]); break;
    default: glUniform4ui(loc, v[0], v[1], v[2], v[3]); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1ui(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_ui(c, t, a, v, 1); }
static JSValue wgl_uniform2ui(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_ui(c, t, a, v, 2); }
static JSValue wgl_uniform3ui(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_ui(c, t, a, v, 3); }
static JSValue wgl_uniform4ui(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_ui(c, t, a, v, 4); }

static JSValue
wgl_uniform_uiv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    GLuint buf[4096];
    int cnt = (argc >= 2) ? wgl_uints(ctx, argv[1], buf, 4096) : 0;
    GLsizei count = cnt / n;
    switch (n) {
    case 1: glUniform1uiv(loc, count, buf); break;
    case 2: glUniform2uiv(loc, count, buf); break;
    case 3: glUniform3uiv(loc, count, buf); break;
    default: glUniform4uiv(loc, count, buf); break;
    }
    return JS_UNDEFINED;
}

static JSValue wgl_uniform1uiv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_uiv(c, t, a, v, 1); }
static JSValue wgl_uniform2uiv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_uiv(c, t, a, v, 2); }
static JSValue wgl_uniform3uiv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_uiv(c, t, a, v, 3); }
static JSValue wgl_uniform4uiv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniform_uiv(c, t, a, v, 4); }

static JSValue
wgl_uniformMatrix_nxm(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv,
                      int rows, int cols)
{
    WGL_GET(0);
    GLint loc = wgl_loc(ctx, argv[0]);
    float buf[4096];
    int cnt = (argc >= 3) ? wgl_floats(ctx, argv[2], buf, 4096) : 0;
    GLsizei count = cnt / (rows * cols);
    if (rows == 2 && cols == 3) glUniformMatrix2x3fv(loc, count, GL_FALSE, buf);
    else if (rows == 3 && cols == 2) glUniformMatrix3x2fv(loc, count, GL_FALSE, buf);
    else if (rows == 2 && cols == 4) glUniformMatrix2x4fv(loc, count, GL_FALSE, buf);
    else if (rows == 4 && cols == 2) glUniformMatrix4x2fv(loc, count, GL_FALSE, buf);
    else if (rows == 3 && cols == 4) glUniformMatrix3x4fv(loc, count, GL_FALSE, buf);
    else glUniformMatrix4x3fv(loc, count, GL_FALSE, buf);
    return JS_UNDEFINED;
}

static JSValue wgl_uniformMatrix2x3fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 2, 3); }
static JSValue wgl_uniformMatrix3x2fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 3, 2); }
static JSValue wgl_uniformMatrix2x4fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 2, 4); }
static JSValue wgl_uniformMatrix4x2fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 4, 2); }
static JSValue wgl_uniformMatrix3x4fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 3, 4); }
static JSValue wgl_uniformMatrix4x3fv(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_uniformMatrix_nxm(c, t, a, v, 4, 3); }

static JSValue
wgl_texStorage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glTexStorage2D((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                   (GLenum)argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                   argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_renderbufferStorageMultisample(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glRenderbufferStorageMultisample((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                                     (GLenum)argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                                     argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_blitFramebuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBlitFramebuffer(argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                      argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                      argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5),
                      argi(ctx, argc, argv, 6), argi(ctx, argc, argv, 7),
                      (GLbitfield)argi(ctx, argc, argv, 8),
                      (GLenum)argi(ctx, argc, argv, 9));
    return JS_UNDEFINED;
}

static JSValue
wgl_framebufferTextureLayer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glFramebufferTextureLayer((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                              (GLuint)wgl_name(ctx, argv[2]), argi(ctx, argc, argv, 3),
                              argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_invalidateFramebuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint att[16];
    int n = (argc >= 2) ? wgl_ints(ctx, argv[1], att, 16) : 0;
    if (n > 0)
        glInvalidateFramebuffer((GLenum)argi(ctx, argc, argv, 0), n, (const GLenum *)att);
    return JS_UNDEFINED;
}

static JSValue
wgl_readBuffer(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glReadBuffer((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_copyBufferSubData(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCopyBufferSubData((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1),
                        (GLintptr)argi(ctx, argc, argv, 2), (GLintptr)argi(ctx, argc, argv, 3),
                        (GLsizeiptr)argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_getBufferSubData(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 1);
    if (argc < 3 || !JS_IsObject(argv[2])) return JS_UNDEFINED;
    JSValue hold;
    size_t len = 0;
    const uint8_t *dst = view_bytes(ctx, argv[2], &len, &hold);
    if (dst && len > 0 && wgl_buffer_range_ok(g, target, offset, len)) {
        void *src = glMapBufferRange(target, offset, (GLsizeiptr)len, GL_MAP_READ_BIT);
        if (src) {
            memcpy((void *)dst, src, len);
            glUnmapBuffer(target);
        }
    }
    if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static JSValue
wgl_clearBuffer_fv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    float v[4] = { 0, 0, 0, 0 };
    if (argc >= 3) wgl_floats(ctx, argv[2], v, 4);
    glClearBufferfv((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1), v);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_clearBuffer_iv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint v[4] = { 0, 0, 0, 0 };
    if (argc >= 3) wgl_ints(ctx, argv[2], v, 4);
    glClearBufferiv((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1), v);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_clearBuffer_uiv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint v[4] = { 0, 0, 0, 0 };
    if (argc >= 3) wgl_uints(ctx, argv[2], v, 4);
    glClearBufferuiv((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1), v);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_clearBufferfi(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glClearBufferfi((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                    (float)argd(ctx, argc, argv, 2), argi(ctx, argc, argv, 3));
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue wgl_createSampler(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "sampler", glGenSamplers); }
static JSValue wgl_deleteSampler(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteSamplers); }

static JSValue
wgl_bindSampler(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindSampler((GLuint)argi(ctx, argc, argv, 0),
                  argc >= 2 ? (GLuint)wgl_name(ctx, argv[1]) : 0);
    return JS_UNDEFINED;
}

static JSValue
wgl_samplerParameteri(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glSamplerParameteri((GLuint)wgl_name(ctx, argv[0]), (GLenum)argi(ctx, argc, argv, 1),
                        argi(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_samplerParameterf(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glSamplerParameterf((GLuint)wgl_name(ctx, argv[0]), (GLenum)argi(ctx, argc, argv, 1),
                        (float)argd(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_isSampler(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsSampler); }

static JSValue
wgl_getUniformBlockIndex(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    const char *name = argc >= 2 ? JS_ToCString(ctx, argv[1]) : NULL;
    GLuint idx = GL_INVALID_INDEX;
    if (name) {
        idx = glGetUniformBlockIndex((GLuint)wgl_name(ctx, argv[0]), name);
        JS_FreeCString(ctx, name);
    }
    return JS_NewUint32(ctx, idx);
}

static JSValue
wgl_uniformBlockBinding(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glUniformBlockBinding((GLuint)wgl_name(ctx, argv[0]), (GLuint)argi(ctx, argc, argv, 1),
                          (GLuint)argi(ctx, argc, argv, 2));
    return JS_UNDEFINED;
}

static JSValue
wgl_bindBufferBase(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindBufferBase((GLenum)argi(ctx, argc, argv, 0), (GLuint)argi(ctx, argc, argv, 1),
                     argc >= 3 ? (GLuint)wgl_name(ctx, argv[2]) : 0);
    return JS_UNDEFINED;
}

static JSValue
wgl_bindBufferRange(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindBufferRange((GLenum)argi(ctx, argc, argv, 0), (GLuint)argi(ctx, argc, argv, 1),
                      argc >= 3 ? (GLuint)wgl_name(ctx, argv[2]) : 0,
                      (GLintptr)argi(ctx, argc, argv, 3), (GLsizeiptr)argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_copyTexImage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCopyTexImage2D((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                     (GLenum)argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                     argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5),
                     argi(ctx, argc, argv, 6), argi(ctx, argc, argv, 7));
    return JS_UNDEFINED;
}

static JSValue
wgl_copyTexSubImage2D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCopyTexSubImage2D((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                        argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                        argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5),
                        argi(ctx, argc, argv, 6), argi(ctx, argc, argv, 7));
    return JS_UNDEFINED;
}

static JSValue
wgl_copyTexSubImage3D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glCopyTexSubImage3D((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                        argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                        argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5),
                        argi(ctx, argc, argv, 6), argi(ctx, argc, argv, 7),
                        argi(ctx, argc, argv, 8));
    return JS_UNDEFINED;
}

static JSValue
wgl_drawRangeElements(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLsizei count = argi(ctx, argc, argv, 3);
    GLenum type = (GLenum)argi(ctx, argc, argv, 4);
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 5);
    if (!wgl_elements_in_range(g, count, type, offset)) return JS_UNDEFINED;
    glDrawRangeElements((GLenum)argi(ctx, argc, argv, 0), (GLuint)argi(ctx, argc, argv, 1),
                        (GLuint)argi(ctx, argc, argv, 2), count, type,
                        (const void *)offset);
    wgl_mark_dirty(g);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribI4i(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glVertexAttribI4i((GLuint)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                      argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                      argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribI4ui(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glVertexAttribI4ui((GLuint)argi(ctx, argc, argv, 0), (GLuint)argi(ctx, argc, argv, 1),
                       (GLuint)argi(ctx, argc, argv, 2), (GLuint)argi(ctx, argc, argv, 3),
                       (GLuint)argi(ctx, argc, argv, 4));
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribI4iv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint v[4] = { 0, 0, 0, 0 };
    if (argc >= 2) wgl_ints(ctx, argv[1], v, 4);
    glVertexAttribI4iv((GLuint)argi(ctx, argc, argv, 0), v);
    return JS_UNDEFINED;
}

static JSValue
wgl_vertexAttribI4uiv(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint v[4] = { 0, 0, 0, 0 };
    if (argc >= 2) wgl_uints(ctx, argv[1], v, 4);
    glVertexAttribI4uiv((GLuint)argi(ctx, argc, argv, 0), v);
    return JS_UNDEFINED;
}

static JSValue
wgl_getFragDataLocation(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    const char *name = argc >= 2 ? JS_ToCString(ctx, argv[1]) : NULL;
    GLint loc = -1;
    if (name) {
        loc = glGetFragDataLocation((GLuint)wgl_name(ctx, argv[0]), name);
        JS_FreeCString(ctx, name);
    }
    return JS_NewInt32(ctx, loc);
}

static JSValue
wgl_getInternalformatParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLenum iformat = (GLenum)argi(ctx, argc, argv, 1);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 2);
    GLint count = 0;
    glGetInternalformativ(target, iformat, GL_NUM_SAMPLE_COUNTS, 1, &count);
    if (count <= 0 || count > 64) {
        JSValue ab = JS_NewArrayBufferCopy(ctx, NULL, 0);
        JSValue ta = wgl_typed_array(ctx, ab, JS_TYPED_ARRAY_INT32);
        JS_FreeValue(ctx, ab);
        return ta;
    }
    GLint *vals = g_new0(GLint, count);
    glGetInternalformativ(target, iformat, pname, count, vals);
    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)vals,
                                       (size_t)count * sizeof(GLint));
    JSValue ta = wgl_typed_array(ctx, ab, JS_TYPED_ARRAY_INT32);
    JS_FreeValue(ctx, ab);
    g_free(vals);
    return ta;
}

static JSValue
wgl_texImage3D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLint level = argi(ctx, argc, argv, 1);
    GLint internalformat = argi(ctx, argc, argv, 2);
    GLsizei w = argi(ctx, argc, argv, 3);
    GLsizei h = argi(ctx, argc, argv, 4);
    GLsizei d = argi(ctx, argc, argv, 5);
    GLint border = argi(ctx, argc, argv, 6);
    GLenum format = (GLenum)argi(ctx, argc, argv, 7);
    GLenum type = (GLenum)argi(ctx, argc, argv, 8);
    JSValue hold = JS_UNDEFINED;
    size_t len = 0;
    const uint8_t *px = NULL;
    if (argc >= 10 && JS_IsObject(argv[9]))
        px = view_bytes(ctx, argv[9], &len, &hold);
    size_t need = wgl_transfer_bytes(g, w, h, d, format, type, FALSE);
    if (need > NS_WEBGL_MAX_ALLOC || (px && len < need)) {
        if (px && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
        return JS_UNDEFINED;
    }
    uint8_t *zero = NULL;
    if (!px) { zero = need ? g_try_malloc0(need) : NULL; px = zero; }
    glTexImage3D(target, level, internalformat, w, h, d, border, format, type, px);
    g_free(zero);
    if (!JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static JSValue
wgl_texSubImage3D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum target = (GLenum)argi(ctx, argc, argv, 0);
    GLint level = argi(ctx, argc, argv, 1);
    GLint xoff = argi(ctx, argc, argv, 2);
    GLint yoff = argi(ctx, argc, argv, 3);
    GLint zoff = argi(ctx, argc, argv, 4);
    GLsizei w = argi(ctx, argc, argv, 5);
    GLsizei h = argi(ctx, argc, argv, 6);
    GLsizei d = argi(ctx, argc, argv, 7);
    GLenum format = (GLenum)argi(ctx, argc, argv, 8);
    GLenum type = (GLenum)argi(ctx, argc, argv, 9);
    JSValue hold = JS_UNDEFINED;
    size_t len = 0;
    const uint8_t *px = NULL;
    if (argc >= 11 && JS_IsObject(argv[10]))
        px = view_bytes(ctx, argv[10], &len, &hold);
    size_t need = wgl_transfer_bytes(g, w, h, d, format, type, FALSE);
    if (px && need > 0 && need <= NS_WEBGL_MAX_ALLOC && len >= need)
        glTexSubImage3D(target, level, xoff, yoff, zoff, w, h, d, format, type, px);
    if (px && !JS_IsUndefined(hold)) JS_FreeValue(ctx, hold);
    return JS_UNDEFINED;
}

static JSValue
wgl_texStorage3D(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glTexStorage3D((GLenum)argi(ctx, argc, argv, 0), argi(ctx, argc, argv, 1),
                   (GLenum)argi(ctx, argc, argv, 2), argi(ctx, argc, argv, 3),
                   argi(ctx, argc, argv, 4), argi(ctx, argc, argv, 5));
    return JS_UNDEFINED;
}

static JSValue wgl_createQuery(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "query", glGenQueries); }
static JSValue wgl_deleteQuery(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteQueries); }

static JSValue
wgl_isQuery(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsQuery); }

static JSValue
wgl_beginQuery(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBeginQuery((GLenum)argi(ctx, argc, argv, 0), (GLuint)wgl_name(ctx, argv[1]));
    return JS_UNDEFINED;
}

static JSValue
wgl_endQuery(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glEndQuery((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_getQueryParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 1);
    GLuint v = 0;
    glGetQueryObjectuiv((GLuint)wgl_name(ctx, argv[0]), pname, &v);
    if (pname == GL_QUERY_RESULT_AVAILABLE)
        return JS_NewBool(ctx, v);
    return JS_NewUint32(ctx, v);
}

static JSValue
wgl_getQuery(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLint v = 0;
    glGetQueryiv((GLenum)argi(ctx, argc, argv, 0), (GLenum)argi(ctx, argc, argv, 1), &v);
    return v ? wgl_wrap(ctx, (GLuint)v, "query") : JS_NULL;
}

static JSValue wgl_createTransformFeedback(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_gen_obj(c, t, a, v, "transformfeedback", glGenTransformFeedbacks); }
static JSValue wgl_deleteTransformFeedback(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_del_obj(c, t, a, v, glDeleteTransformFeedbacks); }

static JSValue
wgl_isTransformFeedback(JSContext *c, JSValueConst t, int a, JSValueConst *v)
{ return wgl_is_obj(c, t, a, v, glIsTransformFeedback); }

static JSValue
wgl_bindTransformFeedback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBindTransformFeedback((GLenum)argi(ctx, argc, argv, 0),
                            argc >= 2 ? (GLuint)wgl_name(ctx, argv[1]) : 0);
    return JS_UNDEFINED;
}

static JSValue
wgl_beginTransformFeedback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    glBeginTransformFeedback((GLenum)argi(ctx, argc, argv, 0));
    return JS_UNDEFINED;
}

static JSValue
wgl_endTransformFeedback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    glEndTransformFeedback();
    return JS_UNDEFINED;
}

static JSValue
wgl_pauseTransformFeedback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    glPauseTransformFeedback();
    return JS_UNDEFINED;
}

static JSValue
wgl_resumeTransformFeedback(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    WGL_GET(0);
    glResumeTransformFeedback();
    return JS_UNDEFINED;
}

static JSValue
wgl_transformFeedbackVaryings(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    if (argc < 3 || !JS_IsObject(argv[1])) return JS_UNDEFINED;
    JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    if (n == 0 || n > 256) return JS_UNDEFINED;
    char **names = g_new0(char *, n);
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], i);
        const char *s = JS_ToCString(ctx, e);
        names[i] = g_strdup(s ? s : "");
        if (s) JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, e);
    }
    glTransformFeedbackVaryings((GLuint)wgl_name(ctx, argv[0]), (GLsizei)n,
                                (const GLchar *const *)names,
                                (GLenum)argi(ctx, argc, argv, 2));
    for (uint32_t i = 0; i < n; i++) g_free(names[i]);
    g_free(names);
    return JS_UNDEFINED;
}

static JSValue
wgl_getActiveUniforms(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    if (argc < 3 || !JS_IsObject(argv[1])) return JS_NULL;
    JSValue lv = JS_GetPropertyStr(ctx, argv[1], "length");
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    if (n == 0 || n > 4096) return JS_NewArray(ctx);
    GLuint *indices = g_new0(GLuint, n);
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, argv[1], i);
        int32_t v = 0;
        JS_ToInt32(ctx, &v, e);
        JS_FreeValue(ctx, e);
        indices[i] = (GLuint)v;
    }
    GLint *out = g_new0(GLint, n);
    glGetActiveUniformsiv((GLuint)wgl_name(ctx, argv[0]), (GLsizei)n, indices,
                          (GLenum)argi(ctx, argc, argv, 2), out);
    JSValue arr = JS_NewArray(ctx);
    for (uint32_t i = 0; i < n; i++)
        JS_SetPropertyUint32(ctx, arr, i, JS_NewInt32(ctx, out[i]));
    g_free(indices);
    g_free(out);
    return arr;
}

static JSValue
wgl_getActiveUniformBlockParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLuint pr = (GLuint)wgl_name(ctx, argv[0]);
    GLuint idx = (GLuint)argi(ctx, argc, argv, 1);
    GLenum pname = (GLenum)argi(ctx, argc, argv, 2);
    if (pname == GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES) {
        GLint count = 0;
        glGetActiveUniformBlockiv(pr, idx, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, &count);
        if (count <= 0 || count > 4096) count = 0;
        GLint *vals = g_new0(GLint, count > 0 ? count : 1);
        if (count > 0)
            glGetActiveUniformBlockiv(pr, idx, pname, vals);
        JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)vals,
                                           (size_t)count * sizeof(GLint));
        JSValue ta = wgl_typed_array(ctx, ab, JS_TYPED_ARRAY_UINT32);
        JS_FreeValue(ctx, ab);
        g_free(vals);
        return ta;
    }
    GLint v = 0;
    glGetActiveUniformBlockiv(pr, idx, pname, &v);
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_getActiveUniformBlockName(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    char name[256] = { 0 };
    glGetActiveUniformBlockName((GLuint)wgl_name(ctx, argv[0]),
                                (GLuint)argi(ctx, argc, argv, 1),
                                sizeof(name) - 1, NULL, name);
    return JS_NewString(ctx, name);
}

static JSValue
wgl_fenceSync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLsync s = glFenceSync((GLenum)argi(ctx, argc, argv, 0),
                           (GLbitfield)argi(ctx, argc, argv, 1));
    if (!s) return JS_NULL;
    if (!g->syncs)
        g->syncs = g_hash_table_new(g_direct_hash, g_direct_equal);
    int id = ++g->next_sync;
    g_hash_table_insert(g->syncs, GINT_TO_POINTER(id), (gpointer)s);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "_sync", JS_NewInt32(ctx, id));
    return o;
}

static GLsync
wgl_sync_lookup(JSContext *ctx, ns_webgl *g, JSValueConst v)
{
    if (!g->syncs || !JS_IsObject(v)) return NULL;
    JSValue p = JS_GetPropertyStr(ctx, v, "_sync");
    int32_t id = 0;
    JS_ToInt32(ctx, &id, p);
    JS_FreeValue(ctx, p);
    return (GLsync)g_hash_table_lookup(g->syncs, GINT_TO_POINTER(id));
}

static JSValue
wgl_isSync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLsync s = argc >= 1 ? wgl_sync_lookup(ctx, g, argv[0]) : NULL;
    return JS_NewBool(ctx, s && glIsSync(s));
}

static JSValue
wgl_deleteSync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    if (argc < 1 || !g->syncs || !JS_IsObject(argv[0])) return JS_UNDEFINED;
    JSValue p = JS_GetPropertyStr(ctx, argv[0], "_sync");
    int32_t id = 0;
    JS_ToInt32(ctx, &id, p);
    JS_FreeValue(ctx, p);
    GLsync s = (GLsync)g_hash_table_lookup(g->syncs, GINT_TO_POINTER(id));
    if (s) {
        glDeleteSync(s);
        g_hash_table_remove(g->syncs, GINT_TO_POINTER(id));
    }
    return JS_UNDEFINED;
}

static JSValue
wgl_clientWaitSync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLsync s = wgl_sync_lookup(ctx, g, argv[0]);
    if (!s) return JS_NewInt32(ctx, (int)GL_WAIT_FAILED);
    double timeout = argd(ctx, argc, argv, 2);
    GLenum r = glClientWaitSync(s, (GLbitfield)argi(ctx, argc, argv, 1),
                                (GLuint64)timeout);
    return JS_NewInt32(ctx, (int)r);
}

static JSValue
wgl_waitSync(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLsync s = wgl_sync_lookup(ctx, g, argv[0]);
    if (s)
        glWaitSync(s, (GLbitfield)argi(ctx, argc, argv, 1), GL_TIMEOUT_IGNORED);
    return JS_UNDEFINED;
}

static JSValue
wgl_getSyncParameter(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    WGL_GET(0);
    GLsync s = wgl_sync_lookup(ctx, g, argv[0]);
    if (!s) return JS_NULL;
    GLint v = 0;
    GLsizei len = 0;
    glGetSynciv(s, (GLenum)argi(ctx, argc, argv, 1), 1, &len, &v);
    return JS_NewInt32(ctx, v);
}

static JSValue
wgl_noop(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

static void
bindf(JSContext *ctx, JSValueConst obj, const char *name, JSCFunction *fn, int argc)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewCFunction(ctx, fn, name, argc));
}

static void
set_const(JSContext *ctx, JSValueConst obj, const char *name, int value)
{
    JS_SetPropertyStr(ctx, obj, name, JS_NewInt32(ctx, value));
}

#define K(n) set_const(ctx, obj, #n, GL_##n)

static void wgl_set_constants(JSContext *ctx, JSValueConst obj);
static void wgl_set_constants2(JSContext *ctx, JSValueConst obj);

void
ns_webgl_install_constants(JSContext *ctx, JSValueConst obj, int version)
{
    wgl_set_constants(ctx, obj);
    if (version >= 2) wgl_set_constants2(ctx, obj);
}

static void
wgl_set_constants(JSContext *ctx, JSValueConst obj)
{
    K(DEPTH_BUFFER_BIT); K(STENCIL_BUFFER_BIT); K(COLOR_BUFFER_BIT);
    K(POINTS); K(LINES); K(LINE_LOOP); K(LINE_STRIP);
    K(TRIANGLES); K(TRIANGLE_STRIP); K(TRIANGLE_FAN);
    K(ZERO); K(ONE); K(SRC_COLOR); K(ONE_MINUS_SRC_COLOR);
    K(SRC_ALPHA); K(ONE_MINUS_SRC_ALPHA); K(DST_ALPHA); K(ONE_MINUS_DST_ALPHA);
    K(DST_COLOR); K(ONE_MINUS_DST_COLOR); K(SRC_ALPHA_SATURATE);
    K(CONSTANT_COLOR); K(ONE_MINUS_CONSTANT_COLOR);
    K(CONSTANT_ALPHA); K(ONE_MINUS_CONSTANT_ALPHA);
    K(FUNC_ADD); K(FUNC_SUBTRACT); K(FUNC_REVERSE_SUBTRACT);
    K(BLEND_EQUATION); K(BLEND_EQUATION_RGB); K(BLEND_EQUATION_ALPHA);
    K(BLEND_DST_RGB); K(BLEND_SRC_RGB); K(BLEND_DST_ALPHA); K(BLEND_SRC_ALPHA);
    K(BLEND_COLOR); K(BLEND);
    K(DEPTH_FUNC); K(DEPTH_RANGE); K(DEPTH_WRITEMASK);
    K(STENCIL_FUNC); K(STENCIL_REF); K(STENCIL_VALUE_MASK);
    K(STENCIL_FAIL); K(STENCIL_PASS_DEPTH_FAIL); K(STENCIL_PASS_DEPTH_PASS);
    K(STENCIL_BACK_FUNC); K(STENCIL_BACK_REF); K(STENCIL_BACK_VALUE_MASK);
    K(STENCIL_BACK_FAIL); K(STENCIL_BACK_PASS_DEPTH_FAIL);
    K(STENCIL_BACK_PASS_DEPTH_PASS); K(STENCIL_WRITEMASK);
    K(STENCIL_BACK_WRITEMASK); K(COLOR_WRITEMASK);
    K(DEPTH_TEST); K(STENCIL_TEST); K(DITHER); K(CULL_FACE);
    K(POLYGON_OFFSET_FILL); K(SAMPLE_ALPHA_TO_COVERAGE); K(SAMPLE_COVERAGE);
    K(LINE_WIDTH); K(POLYGON_OFFSET_FACTOR); K(POLYGON_OFFSET_UNITS);
    K(SAMPLE_COVERAGE_VALUE); K(SAMPLE_COVERAGE_INVERT);
    K(SCISSOR_TEST);
    K(NEVER); K(LESS); K(EQUAL); K(LEQUAL); K(GREATER); K(NOTEQUAL);
    K(GEQUAL); K(ALWAYS);
    K(FRONT); K(BACK); K(FRONT_AND_BACK); K(CW); K(CCW);
    K(KEEP); K(REPLACE); K(INCR); K(DECR); K(INVERT);
    K(INCR_WRAP); K(DECR_WRAP);
    K(BYTE); K(UNSIGNED_BYTE); K(SHORT); K(UNSIGNED_SHORT);
    K(INT); K(UNSIGNED_INT); K(FLOAT);
    K(UNSIGNED_SHORT_4_4_4_4); K(UNSIGNED_SHORT_5_5_5_1); K(UNSIGNED_SHORT_5_6_5);
    K(DEPTH_COMPONENT); K(DEPTH_COMPONENT16); K(DEPTH_STENCIL);
    K(ALPHA); K(RGB); K(RGBA); K(LUMINANCE); K(LUMINANCE_ALPHA);
    K(RGBA4); K(RGB5_A1); K(RGB565); K(STENCIL_INDEX8);
    K(FRAGMENT_SHADER); K(VERTEX_SHADER);
    K(COMPILE_STATUS); K(LINK_STATUS); K(VALIDATE_STATUS); K(DELETE_STATUS);
    K(SHADER_TYPE); K(ATTACHED_SHADERS); K(ACTIVE_UNIFORMS); K(ACTIVE_ATTRIBUTES);
    K(SHADER_SOURCE_LENGTH); K(INFO_LOG_LENGTH);
    K(CURRENT_PROGRAM);
    K(ARRAY_BUFFER); K(ELEMENT_ARRAY_BUFFER);
    K(ARRAY_BUFFER_BINDING); K(ELEMENT_ARRAY_BUFFER_BINDING);
    K(STREAM_DRAW); K(STATIC_DRAW); K(DYNAMIC_DRAW);
    K(BUFFER_SIZE); K(BUFFER_USAGE);
    K(TEXTURE_2D); K(TEXTURE); K(TEXTURE_CUBE_MAP);
    K(TEXTURE_CUBE_MAP_POSITIVE_X); K(TEXTURE_CUBE_MAP_NEGATIVE_X);
    K(TEXTURE_CUBE_MAP_POSITIVE_Y); K(TEXTURE_CUBE_MAP_NEGATIVE_Y);
    K(TEXTURE_CUBE_MAP_POSITIVE_Z); K(TEXTURE_CUBE_MAP_NEGATIVE_Z);
    K(ACTIVE_TEXTURE);
    K(TEXTURE_BINDING_2D); K(TEXTURE_BINDING_CUBE_MAP);
    for (int i = 0; i < 32; i++) {
        char name[16];
        g_snprintf(name, sizeof(name), "TEXTURE%d", i);
        set_const(ctx, obj, name, (int)(GL_TEXTURE0 + i));
    }
    K(TEXTURE_MAG_FILTER); K(TEXTURE_MIN_FILTER);
    K(TEXTURE_WRAP_S); K(TEXTURE_WRAP_T);
    K(NEAREST); K(LINEAR);
    K(NEAREST_MIPMAP_NEAREST); K(LINEAR_MIPMAP_NEAREST);
    K(NEAREST_MIPMAP_LINEAR); K(LINEAR_MIPMAP_LINEAR);
    K(REPEAT); K(CLAMP_TO_EDGE); K(MIRRORED_REPEAT);
    K(FLOAT_VEC2); K(FLOAT_VEC3); K(FLOAT_VEC4);
    K(INT_VEC2); K(INT_VEC3); K(INT_VEC4);
    K(BOOL); K(BOOL_VEC2); K(BOOL_VEC3); K(BOOL_VEC4);
    K(FLOAT_MAT2); K(FLOAT_MAT3); K(FLOAT_MAT4);
    K(SAMPLER_2D); K(SAMPLER_CUBE);
    K(VENDOR); K(RENDERER); K(VERSION); K(SHADING_LANGUAGE_VERSION);
    K(NO_ERROR); K(INVALID_ENUM); K(INVALID_VALUE); K(INVALID_OPERATION);
    K(OUT_OF_MEMORY); K(INVALID_FRAMEBUFFER_OPERATION);
    K(FRAMEBUFFER); K(RENDERBUFFER);
    K(COLOR_ATTACHMENT0); K(DEPTH_ATTACHMENT); K(STENCIL_ATTACHMENT);
    K(FRAMEBUFFER_COMPLETE); K(FRAMEBUFFER_BINDING); K(RENDERBUFFER_BINDING);
    K(MAX_RENDERBUFFER_SIZE);
    K(GENERATE_MIPMAP_HINT); K(DONT_CARE); K(FASTEST); K(NICEST);
    K(MAX_TEXTURE_SIZE); K(MAX_VERTEX_ATTRIBS); K(MAX_TEXTURE_IMAGE_UNITS);
    K(MAX_COMBINED_TEXTURE_IMAGE_UNITS); K(MAX_CUBE_MAP_TEXTURE_SIZE);
    K(MAX_VERTEX_TEXTURE_IMAGE_UNITS); K(MAX_VARYING_VECTORS);
    K(MAX_VERTEX_UNIFORM_VECTORS); K(MAX_FRAGMENT_UNIFORM_VECTORS);
    K(MAX_VIEWPORT_DIMS);
    K(VIEWPORT); K(SCISSOR_BOX); K(COLOR_CLEAR_VALUE);
    K(DEPTH_CLEAR_VALUE); K(BLEND_COLOR);
    K(UNPACK_ALIGNMENT); K(PACK_ALIGNMENT);
    K(CURRENT_VERTEX_ATTRIB); K(VERTEX_ATTRIB_ARRAY_ENABLED);
    K(VERTEX_ATTRIB_ARRAY_SIZE); K(VERTEX_ATTRIB_ARRAY_STRIDE);
    K(VERTEX_ATTRIB_ARRAY_TYPE); K(VERTEX_ATTRIB_ARRAY_NORMALIZED);
    K(VERTEX_ATTRIB_ARRAY_POINTER); K(VERTEX_ATTRIB_ARRAY_BUFFER_BINDING);
    K(FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE); K(FRAMEBUFFER_ATTACHMENT_OBJECT_NAME);
    K(FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL);
    K(FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE);

    set_const(ctx, obj, "DEPTH_STENCIL_ATTACHMENT", 0x821A);
    set_const(ctx, obj, "UNPACK_FLIP_Y_WEBGL", NS_UNPACK_FLIP_Y_WEBGL);
    set_const(ctx, obj, "UNPACK_PREMULTIPLY_ALPHA_WEBGL", NS_UNPACK_PREMULTIPLY_ALPHA_WEBGL);
    set_const(ctx, obj, "CONTEXT_LOST_WEBGL", NS_CONTEXT_LOST_WEBGL);
    set_const(ctx, obj, "UNPACK_COLORSPACE_CONVERSION_WEBGL", NS_UNPACK_COLORSPACE_CONVERSION_WEBGL);
    set_const(ctx, obj, "BROWSER_DEFAULT_WEBGL", NS_BROWSER_DEFAULT_WEBGL);
}

#undef K

#define K(n) set_const(ctx, obj, #n, (int)GL_##n)

static void
wgl_set_constants2(JSContext *ctx, JSValueConst obj)
{
    K(RED); K(RG); K(RGB8); K(RGBA8); K(SRGB8); K(SRGB8_ALPHA8);
    K(RGB10_A2); K(R8); K(RG8);
    K(R16F); K(RG16F); K(RGBA16F); K(R32F); K(RG32F); K(RGBA32F);
    K(R11F_G11F_B10F); K(RGB16F); K(RGB32F);
    K(R8UI); K(RG8UI); K(RGBA8UI); K(R32UI); K(RGBA32UI);
    K(R32I); K(RGBA32I);
    K(RED_INTEGER); K(RG_INTEGER); K(RGB_INTEGER); K(RGBA_INTEGER);
    K(HALF_FLOAT); K(UNSIGNED_INT_24_8); K(UNSIGNED_INT_2_10_10_10_REV);
    K(FLOAT_32_UNSIGNED_INT_24_8_REV);
    K(DEPTH_COMPONENT24); K(DEPTH_COMPONENT32F);
    K(DEPTH24_STENCIL8); K(DEPTH32F_STENCIL8);
    K(PIXEL_PACK_BUFFER); K(PIXEL_UNPACK_BUFFER); K(UNIFORM_BUFFER);
    K(TRANSFORM_FEEDBACK_BUFFER); K(COPY_READ_BUFFER); K(COPY_WRITE_BUFFER);
    K(STATIC_READ); K(DYNAMIC_READ); K(STREAM_READ);
    K(STATIC_COPY); K(DYNAMIC_COPY); K(STREAM_COPY);
    K(READ_FRAMEBUFFER); K(DRAW_FRAMEBUFFER);
    K(MAX_SAMPLES); K(MAX_COLOR_ATTACHMENTS); K(MAX_DRAW_BUFFERS);
    K(TEXTURE_3D); K(TEXTURE_2D_ARRAY); K(TEXTURE_WRAP_R);
    K(TEXTURE_MIN_LOD); K(TEXTURE_MAX_LOD); K(TEXTURE_BASE_LEVEL);
    K(TEXTURE_MAX_LEVEL); K(TEXTURE_COMPARE_MODE); K(TEXTURE_COMPARE_FUNC);
    K(SAMPLER_3D); K(SAMPLER_2D_ARRAY); K(SAMPLER_2D_SHADOW);
    K(UNSIGNED_INT_VEC2); K(UNSIGNED_INT_VEC3); K(UNSIGNED_INT_VEC4);
    K(FLOAT_MAT2x3); K(FLOAT_MAT2x4); K(FLOAT_MAT3x2);
    K(FLOAT_MAT3x4); K(FLOAT_MAT4x2); K(FLOAT_MAT4x3);
    K(MIN); K(MAX);
    K(VERTEX_ARRAY_BINDING);
    K(MAX_3D_TEXTURE_SIZE); K(MAX_ARRAY_TEXTURE_LAYERS);
    K(MAX_ELEMENTS_VERTICES); K(MAX_ELEMENTS_INDICES);
    K(MAX_VERTEX_UNIFORM_BLOCKS); K(MAX_FRAGMENT_UNIFORM_BLOCKS);
    K(MAX_UNIFORM_BUFFER_BINDINGS); K(UNIFORM_BUFFER_OFFSET_ALIGNMENT);
    K(MAX_VERTEX_OUTPUT_COMPONENTS); K(MAX_FRAGMENT_INPUT_COMPONENTS);
    K(COLOR); K(DEPTH); K(STENCIL);

    for (int i = 1; i <= 15; i++) {
        char name[32];
        g_snprintf(name, sizeof(name), "COLOR_ATTACHMENT%d", i);
        set_const(ctx, obj, name, (int)(GL_COLOR_ATTACHMENT0 + i));
        g_snprintf(name, sizeof(name), "DRAW_BUFFER%d", i);
        set_const(ctx, obj, name, (int)(GL_DRAW_BUFFER0 + i));
    }
    set_const(ctx, obj, "DRAW_BUFFER0", (int)GL_DRAW_BUFFER0);
    JS_SetPropertyStr(ctx, obj, "INVALID_INDEX",
                      JS_NewUint32(ctx, GL_INVALID_INDEX));

    K(ANY_SAMPLES_PASSED); K(ANY_SAMPLES_PASSED_CONSERVATIVE);
    K(TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN);
    K(QUERY_RESULT); K(QUERY_RESULT_AVAILABLE); K(CURRENT_QUERY);
    K(TRANSFORM_FEEDBACK); K(INTERLEAVED_ATTRIBS); K(SEPARATE_ATTRIBS);
    K(RASTERIZER_DISCARD); K(TRANSFORM_FEEDBACK_VARYINGS);
    K(TRANSFORM_FEEDBACK_BUFFER_MODE); K(TRANSFORM_FEEDBACK_ACTIVE);
    K(TRANSFORM_FEEDBACK_PAUSED);
    K(SYNC_GPU_COMMANDS_COMPLETE); K(SYNC_FLUSH_COMMANDS_BIT);
    K(ALREADY_SIGNALED); K(TIMEOUT_EXPIRED); K(CONDITION_SATISFIED);
    K(WAIT_FAILED); K(SYNC_STATUS); K(SIGNALED); K(UNSIGNALED);
    K(OBJECT_TYPE); K(SYNC_CONDITION); K(SYNC_FENCE); K(SYNC_FLAGS);
    K(UNIFORM_BLOCK_ACTIVE_UNIFORMS); K(UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES);
    K(UNIFORM_BLOCK_BINDING); K(UNIFORM_BLOCK_DATA_SIZE); K(UNIFORM_BLOCK_INDEX);
    K(UNIFORM_TYPE); K(UNIFORM_SIZE); K(UNIFORM_OFFSET);
    K(UNIFORM_ARRAY_STRIDE); K(UNIFORM_MATRIX_STRIDE); K(UNIFORM_IS_ROW_MAJOR);
    K(SAMPLES); K(NUM_SAMPLE_COUNTS);
    K(COMPARE_REF_TO_TEXTURE); K(MAX_SAMPLES);
}

#undef K

static void
wgl_bind_methods(JSContext *ctx, JSValueConst obj)
{
    bindf(ctx, obj, "getContextAttributes", wgl_getContextAttributes, 0);
    bindf(ctx, obj, "isContextLost", wgl_isContextLost, 0);
    bindf(ctx, obj, "getSupportedExtensions", wgl_getSupportedExtensions, 0);
    bindf(ctx, obj, "getExtension", wgl_getExtension, 1);
    bindf(ctx, obj, "getParameter", wgl_getParameter, 1);
    bindf(ctx, obj, "getError", wgl_getError, 0);

    bindf(ctx, obj, "clearColor", wgl_clearColor, 4);
    bindf(ctx, obj, "clearDepth", wgl_clearDepth, 1);
    bindf(ctx, obj, "clearStencil", wgl_clearStencil, 1);
    bindf(ctx, obj, "clear", wgl_clear, 1);
    bindf(ctx, obj, "viewport", wgl_viewport, 4);
    bindf(ctx, obj, "scissor", wgl_scissor, 4);
    bindf(ctx, obj, "enable", wgl_enable, 1);
    bindf(ctx, obj, "disable", wgl_disable, 1);
    bindf(ctx, obj, "isEnabled", wgl_isEnabled, 1);
    bindf(ctx, obj, "depthFunc", wgl_depthFunc, 1);
    bindf(ctx, obj, "depthMask", wgl_depthMask, 1);
    bindf(ctx, obj, "depthRange", wgl_depthRange, 2);
    bindf(ctx, obj, "colorMask", wgl_colorMask, 4);
    bindf(ctx, obj, "stencilMask", wgl_stencilMask, 1);
    bindf(ctx, obj, "stencilFunc", wgl_stencilFunc, 3);
    bindf(ctx, obj, "stencilOp", wgl_stencilOp, 3);
    bindf(ctx, obj, "blendFunc", wgl_blendFunc, 2);
    bindf(ctx, obj, "blendFuncSeparate", wgl_blendFuncSeparate, 4);
    bindf(ctx, obj, "blendEquation", wgl_blendEquation, 1);
    bindf(ctx, obj, "blendEquationSeparate", wgl_blendEquationSeparate, 2);
    bindf(ctx, obj, "blendColor", wgl_blendColor, 4);
    bindf(ctx, obj, "cullFace", wgl_cullFace, 1);
    bindf(ctx, obj, "frontFace", wgl_frontFace, 1);
    bindf(ctx, obj, "lineWidth", wgl_lineWidth, 1);
    bindf(ctx, obj, "polygonOffset", wgl_polygonOffset, 2);
    bindf(ctx, obj, "hint", wgl_hint, 2);
    bindf(ctx, obj, "finish", wgl_finish, 0);
    bindf(ctx, obj, "flush", wgl_flush, 0);
    bindf(ctx, obj, "pixelStorei", wgl_pixelStorei, 2);
    bindf(ctx, obj, "sampleCoverage", wgl_noop, 2);
    bindf(ctx, obj, "stencilFuncSeparate", wgl_noop, 4);
    bindf(ctx, obj, "stencilOpSeparate", wgl_noop, 4);
    bindf(ctx, obj, "stencilMaskSeparate", wgl_noop, 2);

    bindf(ctx, obj, "activeTexture", wgl_activeTexture, 1);
    bindf(ctx, obj, "createShader", wgl_createShader, 1);
    bindf(ctx, obj, "deleteShader", wgl_deleteShader, 1);
    bindf(ctx, obj, "shaderSource", wgl_shaderSource, 2);
    bindf(ctx, obj, "compileShader", wgl_compileShader, 1);
    bindf(ctx, obj, "getShaderParameter", wgl_getShaderParameter, 2);
    bindf(ctx, obj, "getShaderInfoLog", wgl_getShaderInfoLog, 1);
    bindf(ctx, obj, "getShaderSource", wgl_getShaderSource, 1);
    bindf(ctx, obj, "createProgram", wgl_createProgram, 0);
    bindf(ctx, obj, "deleteProgram", wgl_deleteProgram, 1);
    bindf(ctx, obj, "attachShader", wgl_attachShader, 2);
    bindf(ctx, obj, "detachShader", wgl_detachShader, 2);
    bindf(ctx, obj, "linkProgram", wgl_linkProgram, 1);
    bindf(ctx, obj, "validateProgram", wgl_validateProgram, 1);
    bindf(ctx, obj, "useProgram", wgl_useProgram, 1);
    bindf(ctx, obj, "getProgramParameter", wgl_getProgramParameter, 2);
    bindf(ctx, obj, "getProgramInfoLog", wgl_getProgramInfoLog, 1);
    bindf(ctx, obj, "bindAttribLocation", wgl_bindAttribLocation, 3);
    bindf(ctx, obj, "getAttribLocation", wgl_getAttribLocation, 2);
    bindf(ctx, obj, "getUniformLocation", wgl_getUniformLocation, 2);
    bindf(ctx, obj, "getActiveAttrib", wgl_getActiveAttrib, 2);
    bindf(ctx, obj, "getShaderPrecisionFormat", wgl_getShaderPrecisionFormat, 2);
    bindf(ctx, obj, "getActiveUniform", wgl_getActiveUniform, 2);

    bindf(ctx, obj, "createBuffer", wgl_createBuffer, 0);
    bindf(ctx, obj, "deleteBuffer", wgl_deleteBuffer, 1);
    bindf(ctx, obj, "bindBuffer", wgl_bindBuffer, 2);
    bindf(ctx, obj, "bufferData", wgl_bufferData, 3);
    bindf(ctx, obj, "bufferSubData", wgl_bufferSubData, 3);

    bindf(ctx, obj, "enableVertexAttribArray", wgl_enableVertexAttribArray, 1);
    bindf(ctx, obj, "disableVertexAttribArray", wgl_disableVertexAttribArray, 1);
    bindf(ctx, obj, "vertexAttribPointer", wgl_vertexAttribPointer, 6);
    bindf(ctx, obj, "vertexAttrib1f", wgl_vertexAttrib1f, 2);
    bindf(ctx, obj, "vertexAttrib2f", wgl_vertexAttrib2f, 3);
    bindf(ctx, obj, "vertexAttrib3f", wgl_vertexAttrib3f, 4);
    bindf(ctx, obj, "vertexAttrib4f", wgl_vertexAttrib4f, 5);

    bindf(ctx, obj, "uniform1f", wgl_uniform1f, 2);
    bindf(ctx, obj, "uniform2f", wgl_uniform2f, 3);
    bindf(ctx, obj, "uniform3f", wgl_uniform3f, 4);
    bindf(ctx, obj, "uniform4f", wgl_uniform4f, 5);
    bindf(ctx, obj, "uniform1i", wgl_uniform1i, 2);
    bindf(ctx, obj, "uniform2i", wgl_uniform2i, 3);
    bindf(ctx, obj, "uniform3i", wgl_uniform3i, 4);
    bindf(ctx, obj, "uniform4i", wgl_uniform4i, 5);
    bindf(ctx, obj, "uniform1fv", wgl_uniform1fv, 2);
    bindf(ctx, obj, "uniform2fv", wgl_uniform2fv, 2);
    bindf(ctx, obj, "uniform3fv", wgl_uniform3fv, 2);
    bindf(ctx, obj, "uniform4fv", wgl_uniform4fv, 2);
    bindf(ctx, obj, "uniform1iv", wgl_uniform1iv, 2);
    bindf(ctx, obj, "uniform2iv", wgl_uniform2iv, 2);
    bindf(ctx, obj, "uniform3iv", wgl_uniform3iv, 2);
    bindf(ctx, obj, "uniform4iv", wgl_uniform4iv, 2);
    bindf(ctx, obj, "uniformMatrix2fv", wgl_uniformMatrix2fv, 3);
    bindf(ctx, obj, "uniformMatrix3fv", wgl_uniformMatrix3fv, 3);
    bindf(ctx, obj, "uniformMatrix4fv", wgl_uniformMatrix4fv, 3);

    bindf(ctx, obj, "drawArrays", wgl_drawArrays, 3);
    bindf(ctx, obj, "drawElements", wgl_drawElements, 4);

    bindf(ctx, obj, "createTexture", wgl_createTexture, 0);
    bindf(ctx, obj, "deleteTexture", wgl_deleteTexture, 1);
    bindf(ctx, obj, "bindTexture", wgl_bindTexture, 2);
    bindf(ctx, obj, "texParameteri", wgl_texParameteri, 3);
    bindf(ctx, obj, "texParameterf", wgl_texParameterf, 3);
    bindf(ctx, obj, "generateMipmap", wgl_generateMipmap, 1);
    bindf(ctx, obj, "texImage2D", wgl_texImage2D, 9);
    bindf(ctx, obj, "texSubImage2D", wgl_texSubImage2D, 9);

    bindf(ctx, obj, "createFramebuffer", wgl_createFramebuffer, 0);
    bindf(ctx, obj, "deleteFramebuffer", wgl_deleteFramebuffer, 1);
    bindf(ctx, obj, "bindFramebuffer", wgl_bindFramebuffer, 2);
    bindf(ctx, obj, "framebufferTexture2D", wgl_framebufferTexture2D, 5);
    bindf(ctx, obj, "framebufferRenderbuffer", wgl_framebufferRenderbuffer, 4);
    bindf(ctx, obj, "checkFramebufferStatus", wgl_checkFramebufferStatus, 1);
    bindf(ctx, obj, "createRenderbuffer", wgl_createRenderbuffer, 0);
    bindf(ctx, obj, "deleteRenderbuffer", wgl_deleteRenderbuffer, 1);
    bindf(ctx, obj, "bindRenderbuffer", wgl_bindRenderbuffer, 2);
    bindf(ctx, obj, "renderbufferStorage", wgl_renderbufferStorage, 4);
    bindf(ctx, obj, "readPixels", wgl_readPixels, 7);

    bindf(ctx, obj, "isBuffer", wgl_isBuffer, 1);
    bindf(ctx, obj, "isProgram", wgl_isProgram, 1);
    bindf(ctx, obj, "isShader", wgl_isShader, 1);
    bindf(ctx, obj, "isTexture", wgl_isTexture, 1);
    bindf(ctx, obj, "isFramebuffer", wgl_isFramebuffer, 1);
    bindf(ctx, obj, "isRenderbuffer", wgl_isRenderbuffer, 1);
    bindf(ctx, obj, "getBufferParameter", wgl_getBufferParameter, 2);
    bindf(ctx, obj, "getTexParameter", wgl_getTexParameter, 2);
    bindf(ctx, obj, "getRenderbufferParameter", wgl_getRenderbufferParameter, 2);
    bindf(ctx, obj, "getFramebufferAttachmentParameter",
          wgl_getFramebufferAttachmentParameter, 3);
    bindf(ctx, obj, "getVertexAttrib", wgl_getVertexAttrib, 2);
    bindf(ctx, obj, "getVertexAttribOffset", wgl_getVertexAttribOffset, 2);
    bindf(ctx, obj, "getUniform", wgl_getUniform, 2);
    bindf(ctx, obj, "vertexAttrib1fv", wgl_vertexAttrib1fv, 2);
    bindf(ctx, obj, "vertexAttrib2fv", wgl_vertexAttrib2fv, 2);
    bindf(ctx, obj, "vertexAttrib3fv", wgl_vertexAttrib3fv, 2);
    bindf(ctx, obj, "vertexAttrib4fv", wgl_vertexAttrib4fv, 2);
}

static void
wgl_bind_methods2(JSContext *ctx, JSValueConst obj)
{
    bindf(ctx, obj, "createVertexArray", wgl_createVertexArray, 0);
    bindf(ctx, obj, "deleteVertexArray", wgl_deleteVertexArray, 1);
    bindf(ctx, obj, "bindVertexArray", wgl_bindVertexArray, 1);
    bindf(ctx, obj, "isVertexArray", wgl_isVertexArray, 1);
    bindf(ctx, obj, "drawArraysInstanced", wgl_drawArraysInstanced, 4);
    bindf(ctx, obj, "drawElementsInstanced", wgl_drawElementsInstanced, 5);
    bindf(ctx, obj, "vertexAttribDivisor", wgl_vertexAttribDivisor, 2);
    bindf(ctx, obj, "drawBuffers", wgl_drawBuffers, 1);
    bindf(ctx, obj, "vertexAttribIPointer", wgl_vertexAttribIPointer, 5);
    bindf(ctx, obj, "uniform1ui", wgl_uniform1ui, 2);
    bindf(ctx, obj, "uniform2ui", wgl_uniform2ui, 3);
    bindf(ctx, obj, "uniform3ui", wgl_uniform3ui, 4);
    bindf(ctx, obj, "uniform4ui", wgl_uniform4ui, 5);
    bindf(ctx, obj, "uniform1uiv", wgl_uniform1uiv, 2);
    bindf(ctx, obj, "uniform2uiv", wgl_uniform2uiv, 2);
    bindf(ctx, obj, "uniform3uiv", wgl_uniform3uiv, 2);
    bindf(ctx, obj, "uniform4uiv", wgl_uniform4uiv, 2);
    bindf(ctx, obj, "uniformMatrix2x3fv", wgl_uniformMatrix2x3fv, 3);
    bindf(ctx, obj, "uniformMatrix3x2fv", wgl_uniformMatrix3x2fv, 3);
    bindf(ctx, obj, "uniformMatrix2x4fv", wgl_uniformMatrix2x4fv, 3);
    bindf(ctx, obj, "uniformMatrix4x2fv", wgl_uniformMatrix4x2fv, 3);
    bindf(ctx, obj, "uniformMatrix3x4fv", wgl_uniformMatrix3x4fv, 3);
    bindf(ctx, obj, "uniformMatrix4x3fv", wgl_uniformMatrix4x3fv, 3);
    bindf(ctx, obj, "texStorage2D", wgl_texStorage2D, 5);
    bindf(ctx, obj, "renderbufferStorageMultisample",
          wgl_renderbufferStorageMultisample, 5);
    bindf(ctx, obj, "blitFramebuffer", wgl_blitFramebuffer, 10);
    bindf(ctx, obj, "framebufferTextureLayer", wgl_framebufferTextureLayer, 5);
    bindf(ctx, obj, "invalidateFramebuffer", wgl_invalidateFramebuffer, 2);
    bindf(ctx, obj, "readBuffer", wgl_readBuffer, 1);
    bindf(ctx, obj, "copyBufferSubData", wgl_copyBufferSubData, 5);
    bindf(ctx, obj, "getBufferSubData", wgl_getBufferSubData, 5);
    bindf(ctx, obj, "clearBufferfv", wgl_clearBuffer_fv, 3);
    bindf(ctx, obj, "clearBufferiv", wgl_clearBuffer_iv, 3);
    bindf(ctx, obj, "clearBufferuiv", wgl_clearBuffer_uiv, 3);
    bindf(ctx, obj, "clearBufferfi", wgl_clearBufferfi, 4);
    bindf(ctx, obj, "createSampler", wgl_createSampler, 0);
    bindf(ctx, obj, "deleteSampler", wgl_deleteSampler, 1);
    bindf(ctx, obj, "bindSampler", wgl_bindSampler, 2);
    bindf(ctx, obj, "samplerParameteri", wgl_samplerParameteri, 3);
    bindf(ctx, obj, "samplerParameterf", wgl_samplerParameterf, 3);
    bindf(ctx, obj, "isSampler", wgl_isSampler, 1);
    bindf(ctx, obj, "getUniformBlockIndex", wgl_getUniformBlockIndex, 2);
    bindf(ctx, obj, "uniformBlockBinding", wgl_uniformBlockBinding, 3);
    bindf(ctx, obj, "bindBufferBase", wgl_bindBufferBase, 3);
    bindf(ctx, obj, "bindBufferRange", wgl_bindBufferRange, 5);

    bindf(ctx, obj, "copyTexImage2D", wgl_copyTexImage2D, 8);
    bindf(ctx, obj, "copyTexSubImage2D", wgl_copyTexSubImage2D, 8);
    bindf(ctx, obj, "copyTexSubImage3D", wgl_copyTexSubImage3D, 9);
    bindf(ctx, obj, "drawRangeElements", wgl_drawRangeElements, 6);
    bindf(ctx, obj, "vertexAttribI4i", wgl_vertexAttribI4i, 5);
    bindf(ctx, obj, "vertexAttribI4ui", wgl_vertexAttribI4ui, 5);
    bindf(ctx, obj, "vertexAttribI4iv", wgl_vertexAttribI4iv, 2);
    bindf(ctx, obj, "vertexAttribI4uiv", wgl_vertexAttribI4uiv, 2);
    bindf(ctx, obj, "getFragDataLocation", wgl_getFragDataLocation, 2);
    bindf(ctx, obj, "getInternalformatParameter", wgl_getInternalformatParameter, 3);
    bindf(ctx, obj, "texImage3D", wgl_texImage3D, 10);
    bindf(ctx, obj, "texSubImage3D", wgl_texSubImage3D, 11);
    bindf(ctx, obj, "texStorage3D", wgl_texStorage3D, 6);
    bindf(ctx, obj, "createQuery", wgl_createQuery, 0);
    bindf(ctx, obj, "deleteQuery", wgl_deleteQuery, 1);
    bindf(ctx, obj, "isQuery", wgl_isQuery, 1);
    bindf(ctx, obj, "beginQuery", wgl_beginQuery, 2);
    bindf(ctx, obj, "endQuery", wgl_endQuery, 1);
    bindf(ctx, obj, "getQuery", wgl_getQuery, 2);
    bindf(ctx, obj, "getQueryParameter", wgl_getQueryParameter, 2);
    bindf(ctx, obj, "createTransformFeedback", wgl_createTransformFeedback, 0);
    bindf(ctx, obj, "deleteTransformFeedback", wgl_deleteTransformFeedback, 1);
    bindf(ctx, obj, "isTransformFeedback", wgl_isTransformFeedback, 1);
    bindf(ctx, obj, "bindTransformFeedback", wgl_bindTransformFeedback, 2);
    bindf(ctx, obj, "beginTransformFeedback", wgl_beginTransformFeedback, 1);
    bindf(ctx, obj, "endTransformFeedback", wgl_endTransformFeedback, 0);
    bindf(ctx, obj, "pauseTransformFeedback", wgl_pauseTransformFeedback, 0);
    bindf(ctx, obj, "resumeTransformFeedback", wgl_resumeTransformFeedback, 0);
    bindf(ctx, obj, "transformFeedbackVaryings", wgl_transformFeedbackVaryings, 3);
    bindf(ctx, obj, "getActiveUniforms", wgl_getActiveUniforms, 3);
    bindf(ctx, obj, "getActiveUniformBlockParameter",
          wgl_getActiveUniformBlockParameter, 3);
    bindf(ctx, obj, "getActiveUniformBlockName", wgl_getActiveUniformBlockName, 2);
    bindf(ctx, obj, "fenceSync", wgl_fenceSync, 2);
    bindf(ctx, obj, "isSync", wgl_isSync, 1);
    bindf(ctx, obj, "deleteSync", wgl_deleteSync, 1);
    bindf(ctx, obj, "clientWaitSync", wgl_clientWaitSync, 3);
    bindf(ctx, obj, "waitSync", wgl_waitSync, 3);
    bindf(ctx, obj, "getSyncParameter", wgl_getSyncParameter, 2);
}

JSValue
ns_webgl_get_context(JSContext *ctx, ns_js *js, JSValueConst canvas_obj,
                     const ns_node *canvas, int version, JSValueConst attrs)
{
    if (!g_webgl_by_node)
        g_webgl_by_node = g_hash_table_new(g_direct_hash, g_direct_equal);

    ns_webgl *existing = g_hash_table_lookup(g_webgl_by_node, canvas);
    if (existing)
        return JS_DupValue(ctx, existing->js_obj);

    if (g_hash_table_size(g_webgl_by_node) >= NS_WEBGL_MAX_CONTEXTS)
        return JS_NULL;

    if (!ns_webgl_permission(js))
        return JS_NULL;

    if (!ns_webgl_class_id) {
        JS_NewClassID(JS_GetRuntime(ctx), &ns_webgl_class_id);
        JS_NewClass(JS_GetRuntime(ctx), ns_webgl_class_id, &ns_webgl_class);
    }

    ns_webgl *g = ns_webgl_make(ctx, js, canvas, version, attrs);
    if (!g) return JS_NULL;

    JSValue obj = JS_NewObjectClass(ctx, ns_webgl_class_id);
    if (JS_IsException(obj)) {
        ns_webgl_free(g);
        return JS_NULL;
    }
    JS_SetOpaque(obj, g);
    g->ctx = ctx;
    g->js_obj = obj;

    JS_SetPropertyStr(ctx, obj, "canvas", JS_DupValue(ctx, canvas_obj));
    JS_SetPropertyStr(ctx, obj, "drawingBufferWidth", JS_NewInt32(ctx, g->w));
    JS_SetPropertyStr(ctx, obj, "drawingBufferHeight", JS_NewInt32(ctx, g->h));
    wgl_set_constants(ctx, obj);
    wgl_bind_methods(ctx, obj);
    if (version >= 2) {
        wgl_set_constants2(ctx, obj);
        wgl_bind_methods2(ctx, obj);
    }

    g_hash_table_insert(g_webgl_by_node, (gpointer)canvas, g);
    return obj;
}

cairo_surface_t *
ns_webgl_canvas_surface(const ns_node *canvas)
{
    if (!g_webgl_by_node) return NULL;
    ns_webgl *g = g_hash_table_lookup(g_webgl_by_node, canvas);
    if (!g || !g->gl) return NULL;

    ns_gl_context_make_current(g->gl);
    ns_webgl_sync_size(g);

    int w = g->w, h = g->h;
    if (w <= 0 || h <= 0) return NULL;

    if (!g->dirty && g->surf)
        return g->surf;

    if (g->samples > 1) {
        wgl_bind_framebuffer(g, GL_READ_FRAMEBUFFER, g->draw_fbo);
        wgl_bind_framebuffer(g, GL_DRAW_FRAMEBUFFER, g->fbo);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    wgl_bind_framebuffer(g, GL_FRAMEBUFFER, g->fbo);

    if (!g->surf) {
        g->surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (cairo_surface_status(g->surf) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(g->surf);
            g->surf = NULL;
            return NULL;
        }
    }

    cairo_surface_flush(g->surf);
    int stride = cairo_image_surface_get_stride(g->surf);
    unsigned char *dst = cairo_image_surface_get_data(g->surf);

    uint8_t *rgba = wgl_readback_buffer(g, (size_t)w * (size_t)h * 4);
    if (!rgba) return g->surf;
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    for (int y = 0; y < h; y++) {
        const uint8_t *src = rgba + (size_t)(h - 1 - y) * (size_t)w * 4;
        uint8_t *row = dst + (size_t)y * stride;
        if (!g->alpha) {
            wgl_copy_opaque_rgba_row(row, src, w, TRUE);
            continue;
        }
        {
            gboolean opaque = TRUE;
            for (int x = 0; x < w; x++) {
                if (src[x * 4 + 3] != 255u) {
                    opaque = FALSE;
                    break;
                }
            }
            if (opaque) {
                wgl_copy_opaque_rgba_row(row, src, w, FALSE);
                continue;
            }
        }
        for (int x = 0; x < w; x++) {
            unsigned r = src[x * 4 + 0];
            unsigned gg = src[x * 4 + 1];
            unsigned b = src[x * 4 + 2];
            unsigned a = src[x * 4 + 3];
            if (a == 255u) {
                row[x * 4 + 0] = (uint8_t)b;
                row[x * 4 + 1] = (uint8_t)gg;
                row[x * 4 + 2] = (uint8_t)r;
            } else {
                row[x * 4 + 0] = (uint8_t)((b * a + 127) / 255);
                row[x * 4 + 1] = (uint8_t)((gg * a + 127) / 255);
                row[x * 4 + 2] = (uint8_t)((r * a + 127) / 255);
            }
            row[x * 4 + 3] = (uint8_t)a;
        }
    }
    cairo_surface_mark_dirty(g->surf);
    g->dirty = FALSE;
    g->repaint_queued = FALSE;
    return g->surf;
}

#else /* !NS_ENABLE_WEBGL */

JSValue
ns_webgl_get_context(JSContext *ctx, ns_js *js, JSValueConst canvas_obj,
                     const ns_node *canvas, int version, JSValueConst attrs)
{
    (void)ctx; (void)js; (void)canvas_obj; (void)canvas; (void)version;
    (void)attrs;
    return JS_NULL;
}

void
ns_webgl_install_constants(JSContext *ctx, JSValueConst obj, int version)
{
    (void)ctx; (void)obj; (void)version;
}

cairo_surface_t *
ns_webgl_canvas_surface(const ns_node *canvas)
{
    (void)canvas;
    return NULL;
}

char *
ns_webgl_take_pending_origin(void)
{
    return NULL;
}

void
ns_webgl_set_decision(const char *origin, int allow)
{
    (void)origin; (void)allow;
}

#endif
