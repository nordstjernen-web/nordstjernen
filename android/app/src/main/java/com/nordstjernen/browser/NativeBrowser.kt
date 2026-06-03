/* Nordstjernen — Kotlin facade over the native engine JNI bridge. */

package com.nordstjernen.browser

import android.graphics.Bitmap

object NativeBrowser {
    @Volatile private var libraryLoaded = false

    init {
        libraryLoaded = try {
            System.loadLibrary("nordstjernen_jni")
            true
        } catch (t: Throwable) {
            false
        }
    }

    val available: Boolean
        get() = libraryLoaded && runCatching { nativeEngineAvailable() }.getOrDefault(false)

    external fun nativeEngineAvailable(): Boolean
    external fun nativeInit(dataDir: String, caBundle: String): Int
    external fun nativeOpen(url: String, viewportWidth: Int, settleMs: Int): Long
    external fun nativePageSize(handle: Long): IntArray?
    external fun nativeRender(handle: Long, scrollX: Int, scrollY: Int, scale: Double, bitmap: Bitmap): Boolean
    external fun nativeRenderText(handle: Long): String?
    external fun nativeTitle(handle: Long): String?
    external fun nativeLinkAt(handle: Long, x: Int, y: Int): String?
    external fun nativeClose(handle: Long)
    external fun nativeShutdown()
}
