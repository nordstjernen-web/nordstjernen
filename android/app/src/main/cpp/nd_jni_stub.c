/* Nordstjernen — JNI stub used when the native engine deps are not yet
 * cross-compiled. Builds an APK whose UI runs and reports the engine as
 * unavailable, so the host app and CI are exercised without the full
 * GNOME/cairo dependency stack. */

#include <jni.h>

JNIEXPORT jboolean JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeEngineAvailable(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
    return JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeInit(JNIEnv *env, jclass clazz,
                                                       jstring data_dir, jstring ca_bundle)
{
    (void)env; (void)clazz; (void)data_dir; (void)ca_bundle;
    return -1;
}

JNIEXPORT jlong JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeOpen(JNIEnv *env, jclass clazz,
                                                       jstring url, jint viewport_width,
                                                       jint settle_ms)
{
    (void)env; (void)clazz; (void)url; (void)viewport_width; (void)settle_ms;
    return 0;
}

JNIEXPORT jintArray JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativePageSize(JNIEnv *env, jclass clazz,
                                                           jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return NULL;
}

JNIEXPORT jboolean JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeRender(JNIEnv *env, jclass clazz,
                                                         jlong handle, jint scroll_x,
                                                         jint scroll_y, jdouble scale,
                                                         jobject bitmap)
{
    (void)env; (void)clazz; (void)handle; (void)scroll_x; (void)scroll_y;
    (void)scale; (void)bitmap;
    return JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeTitle(JNIEnv *env, jclass clazz,
                                                        jlong handle)
{
    (void)env; (void)clazz; (void)handle;
    return NULL;
}

JNIEXPORT jstring JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeRenderText(JNIEnv *env, jclass clazz,
                                                             jlong handle)
{
    (void)clazz; (void)handle;
    return (*env)->NewStringUTF(env,
        "Nordstjernen native engine is not bundled in this build. "
        "Cross-compile the dependency stack (see android/scripts/build-deps.sh) "
        "and rebuild with -DNORDSTJERNEN_DEPS=<prefix>.");
}

JNIEXPORT jstring JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeLinkAt(JNIEnv *env, jclass clazz,
                                                         jlong handle, jint x, jint y)
{
    (void)env; (void)clazz; (void)handle; (void)x; (void)y;
    return NULL;
}

JNIEXPORT void JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeClose(JNIEnv *env, jclass clazz,
                                                        jlong handle)
{
    (void)env; (void)clazz; (void)handle;
}

JNIEXPORT void JNICALL
Java_com_nordstjernen_browser_NativeBrowser_nativeShutdown(JNIEnv *env, jclass clazz)
{
    (void)env; (void)clazz;
}
