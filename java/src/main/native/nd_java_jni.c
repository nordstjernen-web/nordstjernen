/* Nordstjernen — JNI bridge from the Java API to the C embedding API.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include <jni.h>
#include <stdint.h>
#include <stdlib.h>

#include "libnordstjernen.h"

static nd_browser *
as_browser(jlong handle)
{
    return (nd_browser *)(intptr_t)handle;
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_Nordstjernen_nativeInit(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return nd_browser_init();
}

JNIEXPORT void JNICALL
Java_org_nordstjernen_Nordstjernen_nativeShutdown(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    nd_browser_shutdown();
}

JNIEXPORT jlong JNICALL
Java_org_nordstjernen_Nordstjernen_nativeOpen(JNIEnv *env, jclass clazz,
                                              jstring url, jint viewport_width,
                                              jint settle_ms)
{
    (void)clazz;
    if (!url) return 0;
    const char *u = (*env)->GetStringUTFChars(env, url, NULL);
    nd_browser *b = u ? nd_browser_open(u, viewport_width, settle_ms) : NULL;
    if (u) (*env)->ReleaseStringUTFChars(env, url, u);
    return (jlong)(intptr_t)b;
}

JNIEXPORT jintArray JNICALL
Java_org_nordstjernen_Nordstjernen_nativePageSize(JNIEnv *env, jclass clazz,
                                                  jlong handle)
{
    (void)clazz;
    int w = 0, h = 0;
    if (nd_browser_page_size(as_browser(handle), &w, &h) != 0) return NULL;
    jintArray arr = (*env)->NewIntArray(env, 2);
    if (!arr) return NULL;
    jint vals[2] = { w, h };
    (*env)->SetIntArrayRegion(env, arr, 0, 2, vals);
    return arr;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_Nordstjernen_nativeRenderText(JNIEnv *env, jclass clazz,
                                                    jlong handle)
{
    (void)clazz;
    char *text = nd_browser_render_text(as_browser(handle));
    if (!text) return NULL;
    jstring s = (*env)->NewStringUTF(env, text);
    free(text);
    return s;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_Nordstjernen_nativeTitle(JNIEnv *env, jclass clazz,
                                               jlong handle)
{
    (void)clazz;
    char *title = nd_browser_title(as_browser(handle));
    if (!title) return NULL;
    jstring s = (*env)->NewStringUTF(env, title);
    free(title);
    return s;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_Nordstjernen_nativeLinkAt(JNIEnv *env, jclass clazz,
                                                jlong handle, jint x, jint y)
{
    (void)clazz;
    char *url = nd_browser_link_at(as_browser(handle), x, y);
    if (!url) return NULL;
    jstring s = (*env)->NewStringUTF(env, url);
    free(url);
    return s;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_Nordstjernen_nativeUrl(JNIEnv *env, jclass clazz,
                                             jlong handle)
{
    (void)clazz;
    char *url = nd_browser_url(as_browser(handle));
    if (!url) return NULL;
    jstring s = (*env)->NewStringUTF(env, url);
    free(url);
    return s;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_Nordstjernen_nativeLinks(JNIEnv *env, jclass clazz,
                                               jlong handle)
{
    (void)clazz;
    char *links = nd_browser_links(as_browser(handle));
    if (!links) return NULL;
    jstring s = (*env)->NewStringUTF(env, links);
    free(links);
    return s;
}

JNIEXPORT jbyteArray JNICALL
Java_org_nordstjernen_Nordstjernen_nativeRenderRgba(JNIEnv *env, jclass clazz,
                                                    jlong handle, jint scroll_x,
                                                    jint scroll_y, jint width,
                                                    jint height, jdouble scale)
{
    (void)clazz;
    if (width <= 0 || height <= 0) return NULL;
    int stride = width * 4;
    size_t len = (size_t)stride * (size_t)height;
    unsigned char *buf = malloc(len);
    if (!buf) return NULL;

    int rc = nd_browser_render_rgba(as_browser(handle), scroll_x, scroll_y,
                                    width, height, scale, buf, stride);
    if (rc != 0) {
        free(buf);
        return NULL;
    }
    jbyteArray arr = (*env)->NewByteArray(env, (jsize)len);
    if (arr) (*env)->SetByteArrayRegion(env, arr, 0, (jsize)len, (const jbyte *)buf);
    free(buf);
    return arr;
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_Nordstjernen_nativeRenderImage(JNIEnv *env, jclass clazz,
                                                     jlong handle, jstring path)
{
    (void)clazz;
    if (!path) return -1;
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    int rc = p ? nd_browser_render_image(as_browser(handle), p) : -1;
    if (p) (*env)->ReleaseStringUTFChars(env, path, p);
    return rc;
}

JNIEXPORT void JNICALL
Java_org_nordstjernen_Nordstjernen_nativeClose(JNIEnv *env, jclass clazz,
                                               jlong handle)
{
    (void)env; (void)clazz;
    nd_browser_close(as_browser(handle));
}
