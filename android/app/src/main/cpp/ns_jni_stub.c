/* Nordstjernen — JNI stub used when the native engine deps are not yet
 * cross-compiled. Builds an APK whose UI runs and reports the engine as
 * unavailable, so the host app and CI are exercised without the full
 * GNOME/cairo dependency stack.
 * Copyright 2026 Andreas Røsdal
 * SPDX-License-Identifier: LicenseRef-NSL-1.0
 */

#include <jni.h>

JNIEXPORT jboolean JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeEngineAvailable(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeInit(JNIEnv *env, jclass clazz,
                                                       jstring data_dir, jstring ca_bundle)
{
    (void)env; (void)clazz; (void)data_dir; (void)ca_bundle;
    return -1;
}

JNIEXPORT void JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeSetDesktopMode(JNIEnv *env,
                                                                 jclass clazz,
                                                                 jboolean enabled)
{
    (void)env; (void)clazz; (void)enabled;
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeDefaultSettleMs(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return 400;
}

JNIEXPORT jlong JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeOpen(JNIEnv *env, jclass clazz,
                                                       jstring url, jint viewport_width,
                                                       jint viewport_height,
                                                       jint settle_ms)
{
    (void)env; (void)clazz; (void)url; (void)viewport_width;
    (void)viewport_height; (void)settle_ms;
    return 0;
}

JNIEXPORT jboolean JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeNavigate(JNIEnv *env, jclass clazz,
                                                           jlong handle, jstring url,
                                                           jint viewport_width,
                                                           jint viewport_height,
                                                           jint settle_ms)
{
    (void)env; (void)clazz; (void)handle; (void)url; (void)viewport_width;
    (void)viewport_height; (void)settle_ms;
    return JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativePageSize(JNIEnv *env, jclass clazz,
                                                           jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeFocusedEditable(JNIEnv *env,
                                                                  jclass clazz,
                                                                  jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeFocusedEditableState(JNIEnv *env,
                                                                       jclass clazz,
                                                                       jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeSetFocusedEditableSelection(JNIEnv *env,
                                                                              jclass clazz,
                                                                              jlong handle,
                                                                              jint caret,
                                                                              jint anchor)
{
    (void)env; (void)clazz; (void)handle; (void)caret; (void)anchor;
    return JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeTakeNavigation(JNIEnv *env,
                                                                 jclass clazz,
                                                                 jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeTakeDownload(JNIEnv *env,
                                                               jclass clazz,
                                                               jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return NULL;
}

JNIEXPORT jint JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeRender(JNIEnv *env, jclass clazz,
                                                         jlong handle, jint scroll_x,
                                                         jint scroll_y, jdouble scale,
                                                         jobject bitmap)
{
    (void)env; (void)clazz; (void)handle; (void)scroll_x; (void)scroll_y;
    (void)scale; (void)bitmap;
    return 0;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeTitle(JNIEnv *env, jclass clazz,
                                                        jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeRenderText(JNIEnv *env, jclass clazz,
                                                             jlong handle)
{
    (void)clazz; (void)handle;
    return (*env)->NewStringUTF(env,
        "Nordstjernen native engine is not bundled in this build. "
        "Cross-compile the dependency stack (see android/scripts/build-deps.sh) "
        "and rebuild with -DNORDSTJERNEN_DEPS=<prefix>.");
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeLinkAt(JNIEnv *env, jclass clazz,
                                                         jlong handle, jint x, jint y)
{
    (void)env; (void)clazz; (void)handle; (void)x; (void)y;
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeClick(JNIEnv *env, jclass clazz,
                                                        jlong handle, jint x, jint y,
                                                        jint mods)
{
    (void)env; (void)clazz; (void)handle; (void)x; (void)y; (void)mods;
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeRelease(JNIEnv *env, jclass clazz,
                                                          jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeKey(JNIEnv *env, jclass clazz,
                                                      jlong handle, jint kind,
                                                      jstring key, jstring code,
                                                      jint keycode, jint mods)
{
    (void)env; (void)clazz; (void)handle; (void)kind; (void)key; (void)code;
    (void)keycode; (void)mods;
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeKeyText(JNIEnv *env, jclass clazz,
                                                          jlong handle, jstring text)
{
    (void)env; (void)clazz; (void)handle; (void)text;
    return NULL;
}

JNIEXPORT void JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeClose(JNIEnv *env, jclass clazz,
                                                        jlong handle)
{
    (void)env; (void)clazz; (void)handle;
}

JNIEXPORT void JNICALL
Java_org_nordstjernen_WebBrowser_NativeBrowser_nativeShutdown(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
}
