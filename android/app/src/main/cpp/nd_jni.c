/* Nordstjernen — JNI bridge from the Android host app to the C engine. */

#include <jni.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libnordstjernen.h"

#define LOG_TAG "nordstjernen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static char *
jstr_dup(JNIEnv *env, jstring s)
{
    if (!s) return NULL;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    char *out = c ? strdup(c) : NULL;
    if (c) (*env)->ReleaseStringUTFChars(env, s, c);
    return out;
}

JNIEXPORT jboolean JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeEngineAvailable(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return JNI_TRUE;
}

JNIEXPORT jint JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeInit(JNIEnv *env, jclass clazz,
                                                       jstring data_dir,
                                                       jstring ca_bundle)
{
    (void)clazz;
    char *dir = jstr_dup(env, data_dir);
    char *ca  = jstr_dup(env, ca_bundle);
    if (dir && *dir) {
        setenv("HOME", dir, 1);
        setenv("XDG_CONFIG_HOME", dir, 1);
        setenv("XDG_CACHE_HOME", dir, 1);
        setenv("XDG_DATA_HOME", dir, 1);
    }
    if (ca && *ca) setenv("CURL_CA_BUNDLE", ca, 1);
    free(dir);
    free(ca);
    int rc = nd_browser_init();
    LOGI("nd_browser_init rc=%d", rc);
    return rc;
}

JNIEXPORT jlong JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeOpen(JNIEnv *env, jclass clazz,
                                                       jstring url, jint viewport_width,
                                                       jint settle_ms)
{
    (void)clazz;
    char *u = jstr_dup(env, url);
    nd_browser *b = u ? nd_browser_open(u, viewport_width, settle_ms) : NULL;
    free(u);
    return (jlong)(intptr_t)b;
}

JNIEXPORT jintArray JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativePageSize(JNIEnv *env, jclass clazz,
                                                           jlong handle)
{
    (void)clazz;
    nd_browser *b = (nd_browser *)(intptr_t)handle;
    int w = 0, h = 0;
    if (nd_browser_page_size(b, &w, &h) != 0) return NULL;
    jintArray arr = (*env)->NewIntArray(env, 2);
    if (!arr) return NULL;
    jint vals[2] = { w, h };
    (*env)->SetIntArrayRegion(env, arr, 0, 2, vals);
    return arr;
}

JNIEXPORT jboolean JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeRender(JNIEnv *env, jclass clazz,
                                                         jlong handle, jint scroll_x,
                                                         jint scroll_y, jdouble scale,
                                                         jobject bitmap)
{
    (void)clazz;
    nd_browser *b = (nd_browser *)(intptr_t)handle;
    if (!b || !bitmap) return JNI_FALSE;

    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        LOGE("nativeRender: bad bitmap format");
        return JNI_FALSE;
    }

    void *pixels = NULL;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS)
        return JNI_FALSE;

    int rc = nd_browser_render_rgba(b, scroll_x, scroll_y,
                                    (int)info.width, (int)info.height, scale,
                                    (unsigned char *)pixels, (int)info.stride);
    AndroidBitmap_unlockPixels(env, bitmap);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeTitle(JNIEnv *env, jclass clazz,
                                                        jlong handle)
{
    (void)clazz;
    char *title = nd_browser_title((nd_browser *)(intptr_t)handle);
    if (!title) return NULL;
    jstring s = (*env)->NewStringUTF(env, title);
    free(title);
    return s;
}

JNIEXPORT jstring JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeRenderText(JNIEnv *env, jclass clazz,
                                                             jlong handle)
{
    (void)clazz;
    nd_browser *b = (nd_browser *)(intptr_t)handle;
    char *text = nd_browser_render_text(b);
    if (!text) return NULL;
    jstring s = (*env)->NewStringUTF(env, text);
    free(text);
    return s;
}

JNIEXPORT jstring JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeLinkAt(JNIEnv *env, jclass clazz,
                                                         jlong handle, jint x, jint y)
{
    (void)clazz;
    nd_browser *b = (nd_browser *)(intptr_t)handle;
    char *url = nd_browser_link_at(b, x, y);
    if (!url) return NULL;
    jstring s = (*env)->NewStringUTF(env, url);
    free(url);
    return s;
}

JNIEXPORT void JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeClose(JNIEnv *env, jclass clazz,
                                                        jlong handle)
{
    (void)env; (void)clazz;
    nd_browser_close((nd_browser *)(intptr_t)handle);
}

JNIEXPORT void JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeShutdown(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    nd_browser_shutdown();
}
