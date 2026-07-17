/* Nordstjernen — JNI bridge from the Android host app to renderer IPC. */

#include <jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ipc_http.h"
#include "libnordstjernen.h"
#include "proc_limits.h"
#include "renderer_serve.h"
#include "rproc_http.h"

#define LOG_TAG "nordstjernen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define NS_ANDROID_MAX_W 2560
#define NS_ANDROID_MAX_H 4096
#define NS_ANDROID_MAX_REDIRECTS 8

typedef struct {
    int                  ctrl_r;
    int                  ctrl_w;
    unsigned char       *fb;
    ns_renderer_session *session;
} AndroidRenderer;

typedef struct {
    ns_rproc_http *renderer;
    int            page_width;
    int            page_height;
    int            render_count;
    int            render_fail_count;
    int            unchanged_count;
    char          *title;
    char          *url;
    char          *nav;
    char          *download;
} AndroidPage;

static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_engine_inited;

static int
android_default_settle_ms(void)
{
    const char *e = getenv(NS_PROC_SETTLE_ENV);
    if (e && *e) {
        int v = atoi(e);
        if (v >= 0 && v <= 10000)
            return v;
    }
    return NS_PROC_SETTLE_MS;
}

static char *
jstr_dup(JNIEnv *env, jstring s)
{
    if (!s) return NULL;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    char *out = c ? strdup(c) : NULL;
    if (c) (*env)->ReleaseStringUTFChars(env, s, c);
    return out;
}

static void
close_pair(int ctrl_r, int ctrl_w)
{
    if (ctrl_r >= 0)
        close(ctrl_r);
    if (ctrl_w >= 0 && ctrl_w != ctrl_r)
        close(ctrl_w);
}

static void *
renderer_thread_main(void *data)
{
    AndroidRenderer *r = data;
    http_conn c;
    http_conn_init(&c, r->ctrl_r);

    for (;;) {
        http_head head;
        if (http_read_head(&c, &head) != 0 ||
            head.content_length > NS_HTTP_MAX_BODY)
            break;

        char *body = NULL;
        if (head.content_length > 0) {
            body = malloc((size_t)head.content_length + 1);
            if (!body ||
                http_read_body(&c, head.content_length, body) != 0) {
                free(body);
                break;
            }
            body[head.content_length] = '\0';
        }

        int quit = ns_renderer_session_handle(r->session, &head, body);
        free(body);
        if (quit)
            break;
    }

    ns_renderer_session_free(r->session);
    close_pair(r->ctrl_r, r->ctrl_w);
    free(r->fb);
    free(r);
    LOGI("renderer thread exited");
    return NULL;
}

static int
android_inproc_attach(int ctrl_r, int ctrl_w, unsigned char *fb,
                      int max_w, int max_h)
{
    AndroidRenderer *r = calloc(1, sizeof *r);
    if (!r)
        return -1;
    r->ctrl_r = ctrl_r;
    r->ctrl_w = ctrl_w;
    r->fb = fb;
    r->session = ns_renderer_session_new(ctrl_w, fb, max_w, max_h, 1);
    if (!r->session) {
        free(r);
        return -1;
    }

    pthread_t thread;
    if (pthread_create(&thread, NULL, renderer_thread_main, r) != 0) {
        ns_renderer_session_free(r->session);
        free(r);
        return -1;
    }
    pthread_detach(thread);
    LOGI("renderer thread attached max=%dx%d", max_w, max_h);
    return 0;
}

static void
frame_clear(ns_rproc_http_frame *frame)
{
    free(frame->nav);
    free(frame->webgl);
    free(frame->download);
}

static AndroidPage *
page_from_handle(jlong handle)
{
    return (AndroidPage *)(intptr_t)handle;
}

static jstring
jstring_take(JNIEnv *env, char *s)
{
    if (!s || !*s) {
        free(s);
        return NULL;
    }
    jstring out = (*env)->NewStringUTF(env, s);
    free(s);
    return out;
}

static void
page_take_opened(AndroidPage *page, ns_rproc_http_page *opened)
{
    free(page->title);
    free(page->url);
    free(page->nav);
    free(page->download);
    page->page_width = opened->page_width;
    page->page_height = opened->page_height;
    page->render_count = 0;
    page->render_fail_count = 0;
    page->unchanged_count = 0;
    page->title = opened->title;
    page->url = opened->url;
    page->nav = NULL;
    page->download = NULL;
    opened->title = NULL;
    opened->url = NULL;
}

static void
page_store_frame_events(AndroidPage *page, ns_rproc_http_frame *frame)
{
    if (frame->nav && *frame->nav) {
        free(page->nav);
        page->nav = strdup(frame->nav);
    }
    if (frame->download && *frame->download) {
        free(page->download);
        page->download = strdup(frame->download);
    }
}

static int
android_open_on_renderer(ns_rproc_http *renderer, const char *label,
                         const char *url, int vw, int vh, int settle_ms,
                         ns_rproc_http_page *opened)
{
    memset(opened, 0, sizeof *opened);
    int open_rc = ns_rproc_http_open(renderer, url, vw, vh, settle_ms, opened);
    if (open_rc != 0 || !opened->ok) {
        LOGE("%s failed rc=%d ok=%d url=%s page=%dx%d nav=%s",
             label, open_rc, opened->ok, url, opened->page_width,
             opened->page_height, opened->nav ? opened->nav : "");
        return -1;
    }

    int redirects = 0;
    while (opened->nav && *opened->nav &&
           redirects < NS_ANDROID_MAX_REDIRECTS) {
        char *next = strdup(opened->nav);
        if (!next)
            return -1;
        LOGI("%s redirect #%d from=%s to=%s", label, redirects + 1,
             opened->url ? opened->url : url, next);
        ns_rproc_http_page_clear(opened);
        open_rc = ns_rproc_http_open(renderer, next, vw, vh, settle_ms,
                                     opened);
        if (open_rc != 0 || !opened->ok) {
            LOGE("%s redirect failed rc=%d ok=%d url=%s page=%dx%d nav=%s",
                 label, open_rc, opened->ok, next, opened->page_width,
                 opened->page_height, opened->nav ? opened->nav : "");
            free(next);
            return -1;
        }
        free(next);
        redirects++;
    }

    if (opened->nav && *opened->nav)
        LOGE("%s stopped after %d redirects at nav=%s", label,
             NS_ANDROID_MAX_REDIRECTS, opened->nav);
    return 0;
}

JNIEXPORT jboolean JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeEngineAvailable(JNIEnv *env,
                                                                  jclass clazz)
{
    (void)env; (void)clazz;
    return JNI_TRUE;
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeInit(JNIEnv *env, jclass clazz,
                                                       jstring data_dir,
                                                       jstring ca_bundle)
{
    (void)clazz;
    pthread_mutex_lock(&g_init_lock);
    if (g_engine_inited) {
        pthread_mutex_unlock(&g_init_lock);
        return 0;
    }

    char *dir = jstr_dup(env, data_dir);
    char *ca = jstr_dup(env, ca_bundle);
    if (dir && *dir) {
        setenv("HOME", dir, 1);
        setenv("XDG_CONFIG_HOME", dir, 1);
        setenv("XDG_CACHE_HOME", dir, 1);
        setenv("XDG_DATA_HOME", dir, 1);
    }
    if (ca && *ca)
        setenv("CURL_CA_BUNDLE", ca, 1);
    free(dir);
    free(ca);

    int rc = ns_browser_init();
    if (rc == 0) {
        ns_rproc_http_set_inproc(android_inproc_attach);
        g_engine_inited = 1;
    }
    pthread_mutex_unlock(&g_init_lock);
    LOGI("ns_browser_init rc=%d", rc);
    return rc;
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeDefaultSettleMs(JNIEnv *env,
                                                                  jclass clazz)
{
    (void)env; (void)clazz;
    return android_default_settle_ms();
}

JNIEXPORT jlong JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeOpen(JNIEnv *env, jclass clazz,
                                                       jstring url,
                                                       jint viewport_width,
                                                       jint viewport_height,
                                                       jint settle_ms)
{
    (void)clazz;
    if (!g_engine_inited) {
        LOGE("nativeOpen before nativeInit");
        return 0;
    }

    char *u = jstr_dup(env, url);
    if (!u)
        return 0;

    int vw = viewport_width > 0 ? viewport_width : 360;
    int vh = viewport_height > 0 ? viewport_height : (vw * 3) / 4;
    LOGI("nativeOpen start url=%s viewport=%dx%d settle=%d",
         u, vw, vh, (int)settle_ms);

    ns_rproc_http *renderer =
        ns_rproc_http_spawn(NULL, NS_ANDROID_MAX_W, NS_ANDROID_MAX_H);
    if (!renderer) {
        LOGE("nativeOpen renderer spawn failed url=%s", u);
        free(u);
        return 0;
    }

    ns_rproc_http_page opened;
    if (android_open_on_renderer(renderer, "nativeOpen", u, vw, vh, settle_ms,
                                 &opened) != 0) {
        ns_rproc_http_page_clear(&opened);
        ns_rproc_http_close(renderer);
        free(u);
        return 0;
    }
    AndroidPage *page = calloc(1, sizeof *page);
    if (!page) {
        ns_rproc_http_page_clear(&opened);
        ns_rproc_http_close(renderer);
        free(u);
        return 0;
    }
    page->renderer = renderer;
    page_take_opened(page, &opened);
    ns_history_record(page->url, page->title);
    LOGI("nativeOpen ok url=%s final=%s page=%dx%d title=%s",
         u, page->url ? page->url : "", page->page_width, page->page_height,
         page->title ? page->title : "");
    ns_rproc_http_page_clear(&opened);
    free(u);
    return (jlong)(intptr_t)page;
}

JNIEXPORT jboolean JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeNavigate(JNIEnv *env,
                                                           jclass clazz,
                                                           jlong handle,
                                                           jstring url,
                                                           jint viewport_width,
                                                           jint viewport_height,
                                                           jint settle_ms)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    if (!g_engine_inited || !page || !page->renderer)
        return JNI_FALSE;

    char *u = jstr_dup(env, url);
    if (!u)
        return JNI_FALSE;

    int vw = viewport_width > 0 ? viewport_width : 360;
    int vh = viewport_height > 0 ? viewport_height : (vw * 3) / 4;
    LOGI("nativeNavigate start url=%s viewport=%dx%d settle=%d",
         u, vw, vh, (int)settle_ms);

    ns_rproc_http_page opened;
    if (android_open_on_renderer(page->renderer, "nativeNavigate", u, vw, vh,
                                 settle_ms, &opened) != 0) {
        ns_rproc_http_page_clear(&opened);
        free(u);
        return JNI_FALSE;
    }

    page_take_opened(page, &opened);
    ns_history_record(page->url, page->title);
    LOGI("nativeNavigate ok url=%s final=%s page=%dx%d title=%s",
         u, page->url ? page->url : "", page->page_width, page->page_height,
         page->title ? page->title : "");
    ns_rproc_http_page_clear(&opened);
    free(u);
    return JNI_TRUE;
}

JNIEXPORT jintArray JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativePageSize(JNIEnv *env,
                                                           jclass clazz,
                                                           jlong handle)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    if (!page)
        return NULL;
    jintArray arr = (*env)->NewIntArray(env, 2);
    if (!arr)
        return NULL;
    jint vals[2] = { page->page_width, page->page_height };
    (*env)->SetIntArrayRegion(env, arr, 0, 2, vals);
    return arr;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeUrl(JNIEnv *env,
                                                      jclass clazz,
                                                      jlong handle)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    if (!page || !page->url)
        return NULL;
    return (*env)->NewStringUTF(env, page->url);
}

JNIEXPORT jboolean JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeFocusedEditable(JNIEnv *env,
                                                                  jclass clazz,
                                                                  jlong handle)
{
    (void)env; (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    return page && page->renderer &&
           ns_rproc_http_focused_editable(page->renderer) ? JNI_TRUE
                                                         : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeFocusedEditableState(JNIEnv *env,
                                                                       jclass clazz,
                                                                       jlong handle)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    if (!page || !page->renderer)
        return NULL;

    size_t caret = 0, anchor = 0;
    char *value = ns_rproc_http_focused_editable_value(page->renderer,
                                                       &caret, &anchor);
    if (!value)
        return NULL;

    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    if (!string_class) {
        free(value);
        return NULL;
    }
    jobjectArray arr = (*env)->NewObjectArray(env, 3, string_class, NULL);
    if (!arr) {
        free(value);
        return NULL;
    }

    char caret_buf[32], anchor_buf[32];
    snprintf(caret_buf, sizeof caret_buf, "%zu", caret);
    snprintf(anchor_buf, sizeof anchor_buf, "%zu", anchor);
    jstring v = (*env)->NewStringUTF(env, value);
    jstring c = (*env)->NewStringUTF(env, caret_buf);
    jstring a = (*env)->NewStringUTF(env, anchor_buf);
    free(value);
    if (!v || !c || !a)
        return arr;
    (*env)->SetObjectArrayElement(env, arr, 0, v);
    (*env)->SetObjectArrayElement(env, arr, 1, c);
    (*env)->SetObjectArrayElement(env, arr, 2, a);
    (*env)->DeleteLocalRef(env, v);
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, a);
    return arr;
}

JNIEXPORT jboolean JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeSetFocusedEditableSelection(JNIEnv *env,
                                                                              jclass clazz,
                                                                              jlong handle,
                                                                              jint caret,
                                                                              jint anchor)
{
    (void)env; (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    if (!page || !page->renderer)
        return JNI_FALSE;
    return ns_rproc_http_set_focused_editable_selection(
               page->renderer,
               caret > 0 ? (size_t)caret : 0,
               anchor > 0 ? (size_t)anchor : 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeTakeNavigation(JNIEnv *env,
                                                                 jclass clazz,
                                                                 jlong handle)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    if (!page)
        return NULL;
    char *nav = page->nav;
    page->nav = NULL;
    return jstring_take(env, nav);
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeTakeDownload(JNIEnv *env,
                                                               jclass clazz,
                                                               jlong handle)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    if (!page)
        return NULL;
    char *download = page->download;
    page->download = NULL;
    return jstring_take(env, download);
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeRender(JNIEnv *env,
                                                         jclass clazz,
                                                         jlong handle,
                                                         jint scroll_x,
                                                         jint scroll_y,
                                                         jdouble scale,
                                                         jobject bitmap)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    if (!page || !page->renderer || !bitmap) {
        LOGE("nativeRender invalid handle=%lld", (long long)handle);
        return 0;
    }

    AndroidBitmapInfo info;
    memset(&info, 0, sizeof info);
    int info_rc = AndroidBitmap_getInfo(env, bitmap, &info);
    if (info_rc != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        LOGE("nativeRender bad bitmap handle=%lld info_rc=%d format=%u size=%ux%u",
             (long long)handle, info_rc, info.format, info.width,
             info.height);
        return 0;
    }

    ns_rproc_http_frame frame;
    int render_rc = ns_rproc_http_render(page->renderer, (int)info.width,
                                         (int)info.height, scroll_x, scroll_y,
                                         scale, &frame);
    if (render_rc != 0 || !frame.ok) {
        page->render_fail_count++;
        LOGE("nativeRender failed rc=%d ok=%d failures=%d view=%ux%u scroll=%d,%d scale=%.3f",
             render_rc, frame.ok, page->render_fail_count, info.width,
             info.height, (int)scroll_x, (int)scroll_y, (double)scale);
        frame_clear(&frame);
        return 0;
    }
    jint flags = 1 | (frame.animating ? 2 : 0) | (frame.unchanged ? 4 : 0);
    page_store_frame_events(page, &frame);
    if (frame.unchanged) {
        page->unchanged_count++;
        if (page->unchanged_count <= 4)
            LOGI("nativeRender unchanged #%d rc=%d view=%ux%u scroll=%d,%d scale=%.3f",
                 page->unchanged_count, frame.render_rc, info.width,
                 info.height, (int)scroll_x, (int)scroll_y, (double)scale);
        frame_clear(&frame);
        return flags;
    }
    if (frame.render_rc != 0) {
        page->render_fail_count++;
        LOGE("nativeRender renderer rc=%d failures=%d frame=%dx%d view=%ux%u scroll=%d,%d scale=%.3f",
             frame.render_rc, page->render_fail_count, frame.width,
             frame.height, info.width, info.height, (int)scroll_x,
             (int)scroll_y, (double)scale);
    }
    if (!frame.pixels) {
        page->render_fail_count++;
        LOGE("nativeRender missing pixels failures=%d frame=%dx%d stride=%d",
             page->render_fail_count, frame.width, frame.height, frame.stride);
        frame_clear(&frame);
        return 0;
    }

    void *pixels = NULL;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) {
        LOGE("nativeRender lockPixels failed view=%ux%u", info.width, info.height);
        frame_clear(&frame);
        return 0;
    }

    int rows = frame.height < (int)info.height ? frame.height : (int)info.height;
    int cols = frame.width < (int)info.width ? frame.width : (int)info.width;
    for (uint32_t y = 0; y < info.height; y++) {
        unsigned char *dst = (unsigned char *)pixels + (size_t)y * info.stride;
        memset(dst, 0xff, (size_t)info.width * 4u);
    }
    uint32_t first_pixel = 0;
    if (rows > 0 && cols > 0)
        memcpy(&first_pixel, frame.pixels, sizeof first_pixel);
    for (int y = 0; y < rows; y++) {
        const unsigned char *src = frame.pixels + (size_t)y * frame.stride;
        unsigned char *dst = (unsigned char *)pixels + (size_t)y * info.stride;
        for (int x = 0; x < cols; x++) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }

    AndroidBitmap_unlockPixels(env, bitmap);
    if (page->render_count < 6 || first_pixel == 0)
        LOGI("nativeRender frame #%d rc=%d frame=%dx%d stride=%d view=%ux%u scroll=%d,%d scale=%.3f first=%08x anim=%d",
             page->render_count + 1, frame.render_rc, frame.width,
             frame.height, frame.stride, info.width, info.height,
             (int)scroll_x, (int)scroll_y, (double)scale, first_pixel,
             frame.animating);
    page->render_count++;
    frame_clear(&frame);
    return flags;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeTitle(JNIEnv *env,
                                                        jclass clazz,
                                                        jlong handle)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    if (!page || !page->title)
        return NULL;
    return (*env)->NewStringUTF(env, page->title);
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeRenderText(JNIEnv *env,
                                                             jclass clazz,
                                                             jlong handle)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    char *text = page && page->renderer ? ns_rproc_http_dump(page->renderer,
                                                             "text")
                                        : NULL;
    if (!text)
        return NULL;
    jstring s = (*env)->NewStringUTF(env, text);
    free(text);
    return s;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeLinkAt(JNIEnv *env,
                                                         jclass clazz,
                                                         jlong handle,
                                                         jint x, jint y)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    char *url = page && page->renderer ? ns_rproc_http_link_at(page->renderer,
                                                               x, y)
                                      : NULL;
    return jstring_take(env, url);
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeClick(JNIEnv *env,
                                                        jclass clazz,
                                                        jlong handle,
                                                        jint x, jint y,
                                                        jint mods)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    char *url = page && page->renderer ? ns_rproc_http_click(page->renderer,
                                                             x, y, mods)
                                      : NULL;
    LOGI("nativeClick x=%d y=%d mods=%d nav=%s", (int)x, (int)y, (int)mods,
         url ? url : "");
    return jstring_take(env, url);
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeRelease(JNIEnv *env,
                                                          jclass clazz,
                                                          jlong handle)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    int changed = 0;
    char *url = page && page->renderer ?
        ns_rproc_http_release_full(page->renderer, &changed) : NULL;
    LOGI("nativeRelease changed=%d nav=%s", changed, url ? url : "");
    return jstring_take(env, url);
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeKey(JNIEnv *env,
                                                      jclass clazz,
                                                      jlong handle,
                                                      jint kind,
                                                      jstring key,
                                                      jstring code,
                                                      jint keycode,
                                                      jint mods)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    char *k = jstr_dup(env, key);
    char *c = jstr_dup(env, code);
    int prevented = 0;
    char *url = page && page->renderer ?
        ns_rproc_http_key_full(page->renderer, kind, k ? k : "",
                               c ? c : "", keycode, mods, &prevented) : NULL;
    LOGI("nativeKey kind=%d key_len=%zu code=%s keycode=%d mods=%d prevented=%d nav=%s",
         (int)kind, k ? strlen(k) : 0, c ? c : "", (int)keycode,
         (int)mods, prevented, url ? url : "");
    free(k);
    free(c);
    return jstring_take(env, url);
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeKeyText(JNIEnv *env,
                                                          jclass clazz,
                                                          jlong handle,
                                                          jstring text)
{
    (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    char *t = jstr_dup(env, text);
    int prevented = 0;
    char *url = page && page->renderer ?
        ns_rproc_http_key_full(page->renderer, 2, t ? t : "", "", 0, 0,
                               &prevented) : NULL;
    LOGI("nativeKeyText text_len=%zu prevented=%d nav=%s",
         t ? strlen(t) : 0, prevented, url ? url : "");
    free(t);
    return jstring_take(env, url);
}

JNIEXPORT void JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeClose(JNIEnv *env,
                                                        jclass clazz,
                                                        jlong handle)
{
    (void)env; (void)clazz;
    AndroidPage *page = page_from_handle(handle);
    if (!page)
        return;
    ns_rproc_http_close(page->renderer);
    LOGI("nativeClose page renders=%d unchanged=%d failures=%d url=%s",
         page->render_count, page->unchanged_count, page->render_fail_count,
         page->url ? page->url : "");
    free(page->title);
    free(page->url);
    free(page->nav);
    free(page->download);
    free(page);
}

JNIEXPORT void JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeShutdown(JNIEnv *env,
                                                          jclass clazz)
{
    (void)env; (void)clazz;
    pthread_mutex_lock(&g_init_lock);
    if (g_engine_inited) {
        ns_browser_shutdown();
        g_engine_inited = 0;
    }
    pthread_mutex_unlock(&g_init_lock);
}
