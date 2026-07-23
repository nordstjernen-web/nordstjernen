/* Nordstjernen — Kotlin facade over the native renderer IPC bridge. */

package org.nordstjernen.WebBrowser

import android.graphics.Bitmap
import android.util.Log

object NativeBrowser {
    private const val TAG = "nordstjernen"
    @Volatile private var libraryLoaded = false

    init {
        libraryLoaded = try {
            System.loadLibrary("nordstjernen_jni")
            true
        } catch (t: Throwable) {
            Log.e(TAG, "failed to load native bridge", t)
            false
        }
    }

    val available: Boolean
        get() = libraryLoaded && runCatching { nativeEngineAvailable() }.getOrDefault(false)

    external fun nativeEngineAvailable(): Boolean
    external fun nativeInit(dataDir: String, caBundle: String): Int
    external fun nativeDefaultSettleMs(): Int
    external fun nativeOpen(url: String, viewportWidth: Int, viewportHeight: Int, settleMs: Int): Long
    external fun nativeNavigate(handle: Long, url: String, viewportWidth: Int, viewportHeight: Int, settleMs: Int): Boolean
    external fun nativePageSize(handle: Long): IntArray?
    external fun nativeUrl(handle: Long): String?
    external fun nativeFocusedEditable(handle: Long): Boolean
    external fun nativeFocusedEditableState(handle: Long): Array<String?>?
    external fun nativeSetFocusedEditableSelection(handle: Long, caret: Int, anchor: Int): Boolean
    external fun nativeTakeNavigation(handle: Long): String?
    external fun nativeTakeDownload(handle: Long): String?
    external fun nativeRender(handle: Long, scrollX: Int, scrollY: Int, scale: Double, bitmap: Bitmap): Int
    external fun nativeScrollAt(handle: Long, x: Int, y: Int, dx: Int, dy: Int): Boolean
    external fun nativeRenderText(handle: Long): String?
    external fun nativeTitle(handle: Long): String?
    external fun nativeLinkAt(handle: Long, x: Int, y: Int): String?
    external fun nativeClick(handle: Long, x: Int, y: Int, mods: Int): String?
    external fun nativeRelease(handle: Long): String?
    external fun nativeKey(handle: Long, kind: Int, key: String, code: String, keyCode: Int, mods: Int): String?
    external fun nativeKeyText(handle: Long, text: String): String?
    external fun nativeClose(handle: Long)
    external fun nativeShutdown()
}
