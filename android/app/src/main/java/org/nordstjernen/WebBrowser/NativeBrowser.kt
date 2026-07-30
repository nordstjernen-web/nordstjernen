/* Nordstjernen — Kotlin facade over the native renderer IPC bridge. */

package org.nordstjernen.WebBrowser

import android.graphics.Bitmap
import android.util.Log

object NativeBrowser {
    private const val TAG = "nordstjernen"
    @Volatile private var libraryLoaded = false

    const val SECURITY_NONE = 0
    const val SECURITY_SECURE = 1
    const val SECURITY_UNTRUSTED = 2
    const val SECURITY_INSECURE = 3

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

    /** The page favicon as a bitmap, decoded from the {w, h, pixels…} the bridge returns. */
    fun favicon(handle: Long): Bitmap? {
        val data = runCatching { nativeFavicon(handle) }.getOrNull() ?: return null
        if (data.size < 3) return null
        val w = data[0]
        val h = data[1]
        if (w <= 0 || h <= 0 || data.size < 2 + w * h) return null
        return Bitmap.createBitmap(data, 2, w, w, h, Bitmap.Config.ARGB_8888)
    }

    external fun nativeEngineAvailable(): Boolean
    external fun nativeInit(dataDir: String, caBundle: String): Int
    external fun nativeSetDesktopMode(enabled: Boolean)
    external fun nativeSetDisplayPrefs(dark: Boolean, reduceMotion: Boolean)
    external fun nativeDefaultSettleMs(): Int
    external fun nativeOpen(url: String, viewportWidth: Int, viewportHeight: Int, settleMs: Int): Long
    external fun nativeNavigate(handle: Long, url: String, viewportWidth: Int, viewportHeight: Int, settleMs: Int, history: Boolean): Boolean
    external fun nativePageSize(handle: Long): IntArray?
    external fun nativeSetViewport(handle: Long, width: Int, height: Int): IntArray?
    external fun nativeUrl(handle: Long): String?
    external fun nativeSecurity(handle: Long): Int
    external fun nativeRemoteIp(handle: Long): String?
    external fun nativeFocusedEditable(handle: Long): Boolean
    external fun nativeFocusedEditableState(handle: Long): Array<String?>?
    external fun nativeSetFocusedEditableSelection(handle: Long, caret: Int, anchor: Int): Boolean
    external fun nativeTakeNavigation(handle: Long): String?
    external fun nativeTakeDownload(handle: Long): String?
    external fun nativeTakeWebgl(handle: Long): String?
    external fun nativeTakeCamera(handle: Long): String?
    external fun nativeTakeScrollY(handle: Long): Int
    external fun nativeResolveWebgl(handle: Long, origin: String, allow: Boolean)
    external fun nativeResolveCamera(handle: Long, origin: String, allow: Boolean)
    external fun nativeRender(handle: Long, scrollX: Int, scrollY: Int, scale: Double, bitmap: Bitmap): Int
    external fun nativeScrollAt(handle: Long, x: Int, y: Int, dx: Int, dy: Int): Boolean
    external fun nativeRenderText(handle: Long): String?
    external fun nativeTitle(handle: Long): String?
    external fun nativeLinkAt(handle: Long, x: Int, y: Int): String?
    external fun nativeClick(handle: Long, x: Int, y: Int, mods: Int): String?
    external fun nativeRelease(handle: Long): String?
    external fun nativeContextMenu(handle: Long, x: Int, y: Int): Boolean
    external fun nativeSelect(handle: Long, kind: Int, x: Int, y: Int): String?
    external fun nativeFind(handle: Long, query: String, caseSensitive: Boolean, direction: Int, fromY: Int): IntArray?
    external fun nativeMediaAt(handle: Long, x: Int, y: Int): Array<String?>?
    external fun nativeExport(handle: Long, path: String): Int
    external fun nativeEval(handle: Long, src: String): String?
    external fun nativeKey(handle: Long, kind: Int, key: String, code: String, keyCode: Int, mods: Int): String?
    external fun nativeKeyText(handle: Long, text: String): String?
    external fun nativeClose(handle: Long)
    external fun nativeShutdown()
    external fun nativeFavicon(handle: Long): IntArray?
}
