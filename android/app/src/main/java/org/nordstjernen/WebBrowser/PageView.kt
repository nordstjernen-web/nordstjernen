/* Nordstjernen — render surface: paints the engine's RGBA viewport, scrolls
 * (with fling) in 2D, pinch/double-tap zooms, follows tapped links and offers a
 * long-press menu. */

package org.nordstjernen.WebBrowser

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.os.SystemClock
import android.text.InputType
import android.util.AttributeSet
import android.util.Log
import android.view.HapticFeedbackConstants
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.VelocityTracker
import android.view.View
import android.view.ViewConfiguration
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.ExtractedText
import android.view.inputmethod.ExtractedTextRequest
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import android.widget.OverScroller
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.roundToInt

class PageView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    companion object {
        private const val TAG = "nordstjernen"
        private const val RENDER_OK = 1
        private const val RENDER_ANIMATING = 2
    }

    private val renderExecutor = Executors.newSingleThreadExecutor()
    private val scroller = OverScroller(context)
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private val minFlingVelocity = ViewConfiguration.get(context).scaledMinimumFlingVelocity.toFloat()
    private val maxFlingVelocity = ViewConfiguration.get(context).scaledMaximumFlingVelocity.toFloat()
    private val longPressTimeout = ViewConfiguration.getLongPressTimeout().toLong()
    private val doubleTapTimeout = ViewConfiguration.getDoubleTapTimeout().toLong()
    private val scaleDetector = ScaleGestureDetector(context, ScaleListener())

    private val minZoom = 0.5
    private val normalZoom = 1.0
    private val maxZoom = 5.0
    private val doubleTapZoom = 2.5

    @Volatile private var handle: Long = 0
    private var pageWidthCss = 0
    private var pageHeightCss = 0
    private var scrollXpx = 0
    private var scrollYpx = 0
    private var userZoom = 1.0
    private var lastTouchX = 0f
    private var lastTouchY = 0f
    private var downX = 0f
    private var downY = 0f
    private var dragging = false
    private var longPressFired = false
    private var gestureWasScale = false
    private var velocityTracker: VelocityTracker? = null
    private var dragScrollGeneration = 0
    private var dragScrollPending = 0
    private var dragOverflowConsumed = false
    private var dragEnded = false
    private var dragFlingX = 0
    private var dragFlingY = 0
    private var viewport: Bitmap? = null
    @Volatile private var renderPending = false
    @Volatile private var renderDirty = false
    @Volatile private var lastRenderStartedAt = 0L
    @Volatile private var pageInputActive = false
    private var pageInputText = ""
    private var pageInputCaret = 0
    private var pageInputAnchor = 0

    private var lastContentTapTime = 0L
    private var lastContentTapX = 0f
    private var lastContentTapY = 0f

    // Scroll fraction to restore on the next setDocument (rotation relayout);
    // -1 means "fresh navigation, reset to top".
    private var pendingScrollFraction = -1f

    var onNavigate: ((url: String) -> Unit)? = null
    var onDownload: ((download: String) -> Unit)? = null
    var onLinkLongPress: ((url: String) -> Unit)? = null
    var onViewportWidthChanged: ((cssWidth: Int) -> Unit)? = null

    // CSS-pixel -> device-pixel base scale (display density). Effective scale is
    // this times the user's zoom.
    var renderScale: Double = 1.0

    init {
        // Double-tap is our zoom toggle; don't let the detector hijack it for
        // quick-scale (double-tap-drag).
        isFocusable = true
        isFocusableInTouchMode = true
        scaleDetector.isQuickScaleEnabled = false
    }

    private fun effScale(): Double = renderScale * userZoom
    private fun contentW(): Int = (pageWidthCss * effScale()).roundToInt()
    private fun contentH(): Int = (pageHeightCss * effScale()).roundToInt()
    private fun maxScrollX(): Int = maxOf(0, contentW() - width)
    private fun maxScrollY(): Int = maxOf(0, contentH() - height)

    private fun utf8Size(codePoint: Int): Int = when {
        codePoint <= 0x7f -> 1
        codePoint <= 0x7ff -> 2
        codePoint <= 0xffff -> 3
        else -> 4
    }

    private fun utf8ByteOffsetToCharIndex(text: String, byteOffset: Int): Int {
        val target = byteOffset.coerceAtLeast(0)
        var bytes = 0
        var i = 0
        while (i < text.length) {
            val codePoint = text.codePointAt(i)
            val nextBytes = bytes + utf8Size(codePoint)
            if (nextBytes > target) break
            bytes = nextBytes
            i += Character.charCount(codePoint)
        }
        return i
    }

    private fun charIndexToUtf8ByteOffset(text: String, index: Int): Int {
        val end = index.coerceIn(0, text.length)
        var bytes = 0
        var i = 0
        while (i < end) {
            val codePoint = text.codePointAt(i)
            bytes += utf8Size(codePoint)
            i += Character.charCount(codePoint)
        }
        return bytes
    }

    private fun clearPageInputState() {
        pageInputText = ""
        pageInputCaret = 0
        pageInputAnchor = 0
    }

    private fun updateCachedPageInputState(state: Array<String?>?): Boolean {
        if (state == null || state.size < 3) {
            clearPageInputState()
            return false
        }
        val text = state.getOrNull(0) ?: ""
        val caretByte = state.getOrNull(1)?.toIntOrNull() ?: 0
        val anchorByte = state.getOrNull(2)?.toIntOrNull() ?: caretByte
        pageInputText = text
        pageInputCaret = utf8ByteOffsetToCharIndex(text, caretByte)
        pageInputAnchor = utf8ByteOffsetToCharIndex(text, anchorByte)
        return true
    }

    private fun selectedInputBounds(): Pair<Int, Int>? {
        val lo = minOf(pageInputCaret, pageInputAnchor).coerceIn(0, pageInputText.length)
        val hi = maxOf(pageInputCaret, pageInputAnchor).coerceIn(0, pageInputText.length)
        return if (lo < hi) Pair(lo, hi) else null
    }

    private fun replaceCachedInputSelection(text: String) {
        val lo = minOf(pageInputCaret, pageInputAnchor).coerceIn(0, pageInputText.length)
        val hi = maxOf(pageInputCaret, pageInputAnchor).coerceIn(0, pageInputText.length)
        pageInputText = pageInputText.substring(0, lo) + text + pageInputText.substring(hi)
        pageInputCaret = lo + text.length
        pageInputAnchor = pageInputCaret
        updateImeSelection()
    }

    private fun removeCachedInputRange(start: Int, end: Int) {
        val lo = start.coerceIn(0, pageInputText.length)
        val hi = end.coerceIn(lo, pageInputText.length)
        pageInputText = pageInputText.substring(0, lo) + pageInputText.substring(hi)
        pageInputCaret = lo
        pageInputAnchor = lo
        updateImeSelection()
    }

    private fun updateImeSelection() {
        if (!pageInputActive) return
        val start = minOf(pageInputCaret, pageInputAnchor).coerceIn(0, pageInputText.length)
        val end = maxOf(pageInputCaret, pageInputAnchor).coerceIn(0, pageInputText.length)
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.updateSelection(this, start, end, -1, -1)
    }

    private val longPressRunnable = Runnable {
        if (!dragging && !scaleDetector.isInProgress && handle != 0L) {
            hitLink(downX.toInt(), downY.toInt()) { url ->
                if (url != null) {
                    longPressFired = true
                    performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
                    onLinkLongPress?.invoke(url)
                }
            }
        }
    }

    // Page dimensions are given in CSS px; scroll is kept in device px.
    fun setDocument(newHandle: Long, pageWidthCssArg: Int, pageHeightCssArg: Int) {
        recycleDocument()
        handle = newHandle
        pageWidthCss = pageWidthCssArg
        pageHeightCss = pageHeightCssArg
        userZoom = normalZoom
        scrollXpx = 0
        scrollYpx = if (pendingScrollFraction >= 0f)
            (pendingScrollFraction * contentH()).roundToInt().coerceIn(0, maxScrollY())
        else 0
        pendingScrollFraction = -1f
        clearPageInputState()
        scroller.forceFinished(true)
        Log.i(TAG, "PageView setDocument handle=$newHandle page=${pageWidthCssArg}x$pageHeightCssArg view=${width}x${height} scale=$renderScale")
        scheduleRender()
    }

    fun updateDocument(pageWidthCssArg: Int, pageHeightCssArg: Int) {
        pageWidthCss = pageWidthCssArg
        pageHeightCss = pageHeightCssArg
        userZoom = normalZoom
        scrollXpx = 0
        scrollYpx = 0
        pendingScrollFraction = -1f
        pageInputActive = false
        clearPageInputState()
        scroller.forceFinished(true)
        Log.i(TAG, "PageView updateDocument handle=$handle page=${pageWidthCssArg}x$pageHeightCssArg view=${width}x${height} scale=$renderScale")
        scheduleRender()
    }

    fun navigateCurrent(
        url: String,
        viewportWidthCss: Int,
        viewportHeightCss: Int,
        settleMs: Int,
        callback: (Boolean, IntArray?, String?, String?) -> Unit,
    ) {
        val h = handle
        if (h == 0L) {
            callback(false, null, null, null)
            return
        }
        renderExecutor.execute {
            val ok = NativeBrowser.nativeNavigate(h, url, viewportWidthCss, viewportHeightCss, settleMs)
            val size = if (ok) NativeBrowser.nativePageSize(h) else null
            val finalUrl = if (ok) NativeBrowser.nativeUrl(h) else null
            val title = if (ok) NativeBrowser.nativeTitle(h) else null
            post {
                if (handle != h) {
                    callback(false, null, null, null)
                } else {
                    callback(ok, size, finalUrl, title)
                }
            }
        }
    }

    fun recycleDocument() {
        val h = handle
        handle = 0
        pageInputActive = false
        clearPageInputState()
        if (h != 0L) renderExecutor.execute { NativeBrowser.nativeClose(h) }
    }

    fun releaseTextInput() {
        pageInputActive = false
        clearPageInputState()
        if (hasFocus()) clearFocus()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        viewport = if (w > 0 && h > 0) {
            Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888).also { it.eraseColor(Color.WHITE) }
        } else null
        Log.i(TAG, "PageView viewport=${w}x$h old=${oldw}x$oldh")
        scheduleRender()
        if (oldw > 0 && w != oldw) {
            pendingScrollFraction = if (contentH() > 0) scrollYpx.toFloat() / contentH() else 0f
            onViewportWidthChanged?.invoke((w / renderScale).roundToInt())
        }
    }

    // viewX/viewY are surface pixels; convert (plus the current scroll) to CSS
    // px at the effective scale for the engine's hit test. cb runs on the UI
    // thread.
    private fun hitLink(viewX: Int, viewY: Int, cb: (String?) -> Unit) {
        val eff = effScale()
        val cssX = ((scrollXpx + viewX) / eff).roundToInt()
        val cssY = ((scrollYpx + viewY) / eff).roundToInt()
        val h = handle
        renderExecutor.execute {
            val href = if (h != 0L) NativeBrowser.nativeLinkAt(h, cssX, cssY) else null
            post { cb(href) }
        }
    }

    private fun pagePoint(viewX: Float, viewY: Float): Pair<Int, Int> {
        val eff = effScale()
        val cssX = ((scrollXpx + viewX) / eff).roundToInt()
        val cssY = ((scrollYpx + viewY) / eff).roundToInt()
        return Pair(cssX, cssY)
    }

    private fun refreshPageInputState(after: (() -> Unit)? = null) {
        val h = handle
        if (h == 0L) {
            pageInputActive = false
            clearPageInputState()
            after?.invoke()
            return
        }
        renderExecutor.execute {
            val state = NativeBrowser.nativeFocusedEditableState(h)
            post {
                if (handle != h) return@post
                pageInputActive = updateCachedPageInputState(state)
                updateImeSelection()
                after?.invoke()
            }
        }
    }

    private fun syncPageSelection(start: Int, end: Int): Boolean {
        val h = handle
        if (h == 0L) return false
        pageInputAnchor = start.coerceIn(0, pageInputText.length)
        pageInputCaret = end.coerceIn(0, pageInputText.length)
        updateImeSelection()
        val caretByte = charIndexToUtf8ByteOffset(pageInputText, pageInputCaret)
        val anchorByte = charIndexToUtf8ByteOffset(pageInputText, pageInputAnchor)
        renderExecutor.execute {
            val ok = NativeBrowser.nativeSetFocusedEditableSelection(h, caretByte, anchorByte)
            post {
                if (handle != h) return@post
                if (!ok) refreshPageInputState()
                scheduleRender()
                invalidate()
            }
        }
        return true
    }

    private fun clipboardText(): String {
        val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        val clip = cm.primaryClip ?: return ""
        if (clip.itemCount <= 0) return ""
        return clip.getItemAt(0).coerceToText(context)?.toString() ?: ""
    }

    private fun copySelectedPageInput(): Boolean {
        val bounds = selectedInputBounds() ?: return false
        val text = pageInputText.substring(bounds.first, bounds.second)
        val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("text", text))
        return true
    }

    private fun cutSelectedPageInput(): Boolean {
        val bounds = selectedInputBounds() ?: return false
        copySelectedPageInput()
        sendKeyToPage(0, "Backspace", "Backspace", 8, 0)
        removeCachedInputRange(bounds.first, bounds.second)
        return true
    }

    private fun pasteIntoPageInput(): Boolean {
        val text = clipboardText()
        if (text.isEmpty()) return true
        replaceCachedInputSelection(text)
        sendTextToPage(text)
        return true
    }

    private fun showPageKeyboard() {
        pageInputActive = true
        if (!hasFocus()) requestFocus()
        refreshPageInputState {
            val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
            imm.restartInput(this)
            imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT)
        }
    }

    private fun hidePageKeyboard() {
        pageInputActive = false
        clearPageInputState()
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(windowToken, 0)
    }

    private fun activatePage(viewX: Float, viewY: Float) {
        val h = handle
        if (h == 0L) return
        val point = pagePoint(viewX, viewY)
        requestFocus()
        renderExecutor.execute {
            val pressNav = NativeBrowser.nativeClick(h, point.first, point.second, 0)
            val releaseNav = NativeBrowser.nativeRelease(h)
            val nav = if (!releaseNav.isNullOrEmpty()) releaseNav else pressNav
            val inputActive = NativeBrowser.nativeFocusedEditable(h)
            post {
                if (handle != h) return@post
                if (!nav.isNullOrEmpty()) {
                    onNavigate?.invoke(nav)
                } else if (inputActive) {
                    showPageKeyboard()
                    scheduleRender()
                    invalidate()
                } else {
                    hidePageKeyboard()
                    scheduleRender()
                    invalidate()
                }
            }
        }
    }

    private fun sendTextToPage(text: String) {
        if (text.isEmpty()) return
        val h = handle
        if (h == 0L) return
        renderExecutor.execute {
            val nav = NativeBrowser.nativeKeyText(h, text)
            post {
                if (handle != h) return@post
                if (!nav.isNullOrEmpty()) onNavigate?.invoke(nav) else {
                    scheduleRender()
                    if (pageInputActive) refreshPageInputState()
                }
            }
        }
    }

    private fun sendKeyToPage(kind: Int, key: String, code: String, keyCode: Int, mods: Int) {
        if (key.isEmpty()) return
        val h = handle
        if (h == 0L) return
        renderExecutor.execute {
            val nav = NativeBrowser.nativeKey(h, kind, key, code, keyCode, mods)
            post {
                if (handle != h) return@post
                if (!nav.isNullOrEmpty()) onNavigate?.invoke(nav) else {
                    scheduleRender()
                    if (pageInputActive) refreshPageInputState()
                }
            }
        }
    }

    private fun eventMods(event: KeyEvent): Int {
        var mods = 0
        if (event.isShiftPressed) mods = mods or 1
        if (event.isCtrlPressed) mods = mods or 2
        if (event.isAltPressed) mods = mods or 4
        if (event.isMetaPressed) mods = mods or 8
        return mods
    }

    private fun keyName(event: KeyEvent): String? = when (event.keyCode) {
        KeyEvent.KEYCODE_DEL -> "Backspace"
        KeyEvent.KEYCODE_FORWARD_DEL -> "Delete"
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> "Enter"
        KeyEvent.KEYCODE_TAB -> "Tab"
        KeyEvent.KEYCODE_ESCAPE -> "Escape"
        KeyEvent.KEYCODE_DPAD_LEFT -> "ArrowLeft"
        KeyEvent.KEYCODE_DPAD_RIGHT -> "ArrowRight"
        KeyEvent.KEYCODE_DPAD_UP -> "ArrowUp"
        KeyEvent.KEYCODE_DPAD_DOWN -> "ArrowDown"
        KeyEvent.KEYCODE_MOVE_HOME -> "Home"
        KeyEvent.KEYCODE_MOVE_END -> "End"
        KeyEvent.KEYCODE_SPACE -> " "
        else -> null
    }

    private fun keyCodeName(keyCode: Int): String = when (keyCode) {
        KeyEvent.KEYCODE_DEL -> "Backspace"
        KeyEvent.KEYCODE_FORWARD_DEL -> "Delete"
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> "Enter"
        KeyEvent.KEYCODE_TAB -> "Tab"
        KeyEvent.KEYCODE_ESCAPE -> "Escape"
        KeyEvent.KEYCODE_DPAD_LEFT -> "ArrowLeft"
        KeyEvent.KEYCODE_DPAD_RIGHT -> "ArrowRight"
        KeyEvent.KEYCODE_DPAD_UP -> "ArrowUp"
        KeyEvent.KEYCODE_DPAD_DOWN -> "ArrowDown"
        KeyEvent.KEYCODE_MOVE_HOME -> "Home"
        KeyEvent.KEYCODE_MOVE_END -> "End"
        KeyEvent.KEYCODE_SPACE -> "Space"
        in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z ->
            "Key" + ('A'.code + keyCode - KeyEvent.KEYCODE_A).toChar()
        in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9 ->
            "Digit" + (keyCode - KeyEvent.KEYCODE_0).toString()
        else -> ""
    }

    private fun domKeyCode(keyCode: Int, key: String): Int = when (keyCode) {
        KeyEvent.KEYCODE_DEL -> 8
        KeyEvent.KEYCODE_TAB -> 9
        KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_NUMPAD_ENTER -> 13
        KeyEvent.KEYCODE_ESCAPE -> 27
        KeyEvent.KEYCODE_SPACE -> 32
        KeyEvent.KEYCODE_DPAD_LEFT -> 37
        KeyEvent.KEYCODE_DPAD_UP -> 38
        KeyEvent.KEYCODE_DPAD_RIGHT -> 39
        KeyEvent.KEYCODE_DPAD_DOWN -> 40
        KeyEvent.KEYCODE_FORWARD_DEL -> 46
        KeyEvent.KEYCODE_MOVE_HOME -> 36
        KeyEvent.KEYCODE_MOVE_END -> 35
        in KeyEvent.KEYCODE_A..KeyEvent.KEYCODE_Z -> 65 + keyCode - KeyEvent.KEYCODE_A
        in KeyEvent.KEYCODE_0..KeyEvent.KEYCODE_9 -> 48 + keyCode - KeyEvent.KEYCODE_0
        else -> if (key.length == 1) key[0].uppercaseChar().code else keyCode
    }

    private fun unicodeKey(event: KeyEvent): String? {
        val value = event.getUnicodeChar(event.metaState)
        return if (value > 0) String(Character.toChars(value)) else null
    }

    private fun dispatchKeyEventToPage(event: KeyEvent, kind: Int): Boolean {
        val key = keyName(event) ?: unicodeKey(event) ?: return false
        sendKeyToPage(kind, key, keyCodeName(event.keyCode), domKeyCode(event.keyCode, key), eventMods(event))
        return true
    }

    private fun setScroll(x: Int, y: Int) {
        val cx = x.coerceIn(0, maxScrollX())
        val cy = y.coerceIn(0, maxScrollY())
        if (cx != scrollXpx || cy != scrollYpx) {
            scrollXpx = cx
            scrollYpx = cy
            scheduleRender()
            invalidate()
        }
    }

    private fun finishDragScroll() {
        if (!dragEnded || dragScrollPending != 0) return
        dragEnded = false
        if (!dragOverflowConsumed &&
            (abs(dragFlingX) > minFlingVelocity || abs(dragFlingY) > minFlingVelocity) &&
            (maxScrollX() > 0 || maxScrollY() > 0)) {
            scroller.fling(scrollXpx, scrollYpx, dragFlingX, dragFlingY,
                0, maxScrollX(), 0, maxScrollY())
            postInvalidateOnAnimation()
        }
    }

    private fun scrollContentAt(viewX: Float, viewY: Float, dxPx: Int, dyPx: Int) {
        if (dxPx == 0 && dyPx == 0) return
        val h = handle
        if (h == 0L) return
        val eff = effScale()
        val cssX = ((scrollXpx + viewX) / eff).roundToInt()
        val cssY = ((scrollYpx + viewY) / eff).roundToInt()
        val cssDx = (dxPx / eff).roundToInt()
        val cssDy = (dyPx / eff).roundToInt()
        val generation = dragScrollGeneration
        dragScrollPending++
        renderExecutor.execute {
            val consumed = NativeBrowser.nativeScrollAt(h, cssX, cssY, cssDx, cssDy)
            post {
                if (handle != h || generation != dragScrollGeneration) return@post
                dragScrollPending--
                if (consumed) {
                    dragOverflowConsumed = true
                    scheduleRender()
                    invalidate()
                } else {
                    setScroll(scrollXpx + dxPx, scrollYpx + dyPx)
                }
                finishDragScroll()
            }
        }
    }

    private fun scheduleRender() {
        val bmp = viewport ?: return
        val h = handle
        if (h == 0L) return
        if (renderPending) {
            val age = SystemClock.uptimeMillis() - lastRenderStartedAt
            if (lastRenderStartedAt > 0L && age > 1500L) {
                Log.w(TAG, "PageView stale render recovered handle=$h age=${age}ms")
                renderPending = false
                renderDirty = false
            } else {
                renderDirty = true
                return
            }
        }
        renderPending = true
        renderDirty = false
        lastRenderStartedAt = SystemClock.uptimeMillis()
        val eff = effScale()
        val sxc = (scrollXpx / eff).roundToInt()
        val syc = (scrollYpx / eff).roundToInt()
        renderExecutor.execute {
            val renderState = NativeBrowser.nativeRender(h, sxc, syc, eff, bmp)
            val ok = (renderState and RENDER_OK) != 0
            val animating = (renderState and RENDER_ANIMATING) != 0
            val nav = if (ok) NativeBrowser.nativeTakeNavigation(h) else null
            val download = if (ok) NativeBrowser.nativeTakeDownload(h) else null
            post {
                if (handle != h) {
                    renderPending = false
                    lastRenderStartedAt = 0L
                    return@post
                }
                renderPending = false
                lastRenderStartedAt = 0L
                if (!nav.isNullOrEmpty()) {
                    onNavigate?.invoke(nav)
                    return@post
                } else if (ok) {
                    if (!download.isNullOrEmpty()) onDownload?.invoke(download)
                    invalidate()
                } else {
                    Log.e(TAG, "PageView render failed handle=$h view=${bmp.width}x${bmp.height} scroll=${sxc},${syc} scale=$eff")
                }
                if (renderDirty) scheduleRender()
                else if (animating) postDelayed({ if (handle == h) scheduleRender() }, 32L)
            }
        }
    }

    fun redrawCurrentPage() {
        if (handle == 0L) return
        if (viewport == null && width > 0 && height > 0)
            viewport = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888).also { it.eraseColor(Color.WHITE) }
        if (renderPending && lastRenderStartedAt > 0L &&
            SystemClock.uptimeMillis() - lastRenderStartedAt > 1500L) {
            renderPending = false
            renderDirty = true
        }
        scheduleRender()
        invalidate()
        postDelayed({ if (handle != 0L) scheduleRender() }, 120)
    }

    override fun onDraw(canvas: Canvas) {
        val bmp = viewport
        canvas.drawColor(Color.WHITE)
        if (bmp != null && handle != 0L) {
            canvas.drawBitmap(bmp, 0f, 0f, null)
        }
    }

    override fun onWindowFocusChanged(hasWindowFocus: Boolean) {
        super.onWindowFocusChanged(hasWindowFocus)
        if (hasWindowFocus) redrawCurrentPage()
    }

    override fun computeScroll() {
        if (scroller.computeScrollOffset()) {
            setScroll(scroller.currX, scroller.currY)
            postInvalidateOnAnimation()
        }
    }

    private fun toggleZoomAt(viewX: Float, viewY: Float) {
        val old = effScale()
        val cssX = (scrollXpx + viewX) / old
        val cssY = (scrollYpx + viewY) / old
        userZoom =
            if (userZoom > normalZoom + 0.01 || userZoom < normalZoom - 0.01)
                normalZoom
            else
                doubleTapZoom
        val neo = effScale()
        scrollXpx = (cssX * neo - viewX).roundToInt().coerceIn(0, maxScrollX())
        scrollYpx = (cssY * neo - viewY).roundToInt().coerceIn(0, maxScrollY())
        scheduleRender()
        invalidate()
    }

    private fun handleContentTap(x: Float, y: Float) {
        val now = SystemClock.uptimeMillis()
        if (now - lastContentTapTime < doubleTapTimeout &&
            hypot(x - lastContentTapX, y - lastContentTapY) < touchSlop * 3) {
            lastContentTapTime = 0L
            toggleZoomAt(x, y)
        } else {
            lastContentTapTime = now
            lastContentTapX = x
            lastContentTapY = y
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val tracker = velocityTracker ?: VelocityTracker.obtain().also { velocityTracker = it }
        tracker.addMovement(event)
        scaleDetector.onTouchEvent(event)

        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                scroller.forceFinished(true)
                dragScrollGeneration++
                dragScrollPending = 0
                dragOverflowConsumed = false
                dragEnded = false
                dragFlingX = 0
                dragFlingY = 0
                downX = event.x
                downY = event.y
                lastTouchX = event.x
                lastTouchY = event.y
                dragging = false
                longPressFired = false
                gestureWasScale = false
                postDelayed(longPressRunnable, longPressTimeout)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                if (scaleDetector.isInProgress) {
                    removeCallbacks(longPressRunnable)
                    return true
                }
                if (!dragging && hypot(event.x - downX, event.y - downY) > touchSlop) {
                    dragging = true
                    removeCallbacks(longPressRunnable)
                }
                if (dragging) {
                    val dx = (lastTouchX - event.x).toInt()
                    val dy = (lastTouchY - event.y).toInt()
                    lastTouchX = event.x
                    lastTouchY = event.y
                    scrollContentAt(downX, downY, dx, dy)
                }
                return true
            }
            MotionEvent.ACTION_UP -> {
                removeCallbacks(longPressRunnable)
                when {
                    gestureWasScale -> { /* end of a pinch — not a tap */ }
                    dragging -> {
                        tracker.computeCurrentVelocity(1000, maxFlingVelocity)
                        dragFlingX = -tracker.xVelocity.toInt()
                        dragFlingY = -tracker.yVelocity.toInt()
                        dragEnded = true
                        finishDragScroll()
                    }
                    !longPressFired && handle != 0L -> {
                        activatePage(downX, downY)
                    }
                }
                releaseTracker()
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                removeCallbacks(longPressRunnable)
                dragScrollGeneration++
                dragScrollPending = 0
                dragEnded = false
                releaseTracker()
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    override fun onCheckIsTextEditor(): Boolean = pageInputActive && handle != 0L

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection? {
        if (!pageInputActive || handle == 0L) return null
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT or
            InputType.TYPE_TEXT_FLAG_MULTI_LINE or
            InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
        outAttrs.imeOptions = EditorInfo.IME_ACTION_NONE or EditorInfo.IME_FLAG_NO_EXTRACT_UI
        outAttrs.initialSelStart = minOf(pageInputCaret, pageInputAnchor).coerceIn(0, pageInputText.length)
        outAttrs.initialSelEnd = maxOf(pageInputCaret, pageInputAnchor).coerceIn(0, pageInputText.length)
        return PageInputConnection()
    }

    private fun handlePageEditShortcut(event: KeyEvent): Boolean {
        if (!pageInputActive || !(event.isCtrlPressed || event.isMetaPressed)) return false
        return when (event.keyCode) {
            KeyEvent.KEYCODE_A -> {
                syncPageSelection(0, pageInputText.length)
                true
            }
            KeyEvent.KEYCODE_C -> {
                copySelectedPageInput()
                true
            }
            KeyEvent.KEYCODE_X -> {
                cutSelectedPageInput()
                true
            }
            KeyEvent.KEYCODE_V -> {
                pasteIntoPageInput()
                true
            }
            else -> false
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        return if (handlePageEditShortcut(event) || (pageInputActive && dispatchKeyEventToPage(event, 0)))
            true
        else
            super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        return if (pageInputActive && dispatchKeyEventToPage(event, 1)) true else super.onKeyUp(keyCode, event)
    }

    private inner class PageInputConnection : BaseInputConnection(this@PageView, true) {
        override fun commitText(text: CharSequence?, newCursorPosition: Int): Boolean {
            if (!text.isNullOrEmpty()) {
                val value = text.toString()
                replaceCachedInputSelection(value)
                sendTextToPage(value)
            }
            return true
        }

        override fun setComposingText(text: CharSequence?, newCursorPosition: Int): Boolean {
            return commitText(text, newCursorPosition)
        }

        override fun finishComposingText(): Boolean = true

        override fun setComposingRegion(start: Int, end: Int): Boolean {
            return syncPageSelection(start, end)
        }

        override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
            val selected = selectedInputBounds()
            if (selected != null) {
                sendKeyToPage(0, "Backspace", "Backspace", 8, 0)
                removeCachedInputRange(selected.first, selected.second)
                return true
            }
            val caret = pageInputCaret.coerceIn(0, pageInputText.length)
            val start = (caret - beforeLength.coerceAtLeast(0)).coerceAtLeast(0)
            val end = (caret + afterLength.coerceAtLeast(0)).coerceAtMost(pageInputText.length)
            if (start < end) {
                syncPageSelection(start, end)
                sendKeyToPage(0, "Backspace", "Backspace", 8, 0)
                removeCachedInputRange(start, end)
            }
            return true
        }

        override fun deleteSurroundingTextInCodePoints(beforeLength: Int, afterLength: Int): Boolean {
            return deleteSurroundingText(beforeLength, afterLength)
        }

        override fun getTextBeforeCursor(n: Int, flags: Int): CharSequence {
            val end = pageInputCaret.coerceIn(0, pageInputText.length)
            val start = (end - n.coerceAtLeast(0)).coerceAtLeast(0)
            return pageInputText.substring(start, end)
        }

        override fun getTextAfterCursor(n: Int, flags: Int): CharSequence {
            val start = pageInputCaret.coerceIn(0, pageInputText.length)
            val end = (start + n.coerceAtLeast(0)).coerceAtMost(pageInputText.length)
            return pageInputText.substring(start, end)
        }

        override fun getSelectedText(flags: Int): CharSequence? {
            val bounds = selectedInputBounds() ?: return null
            return pageInputText.substring(bounds.first, bounds.second)
        }

        override fun getExtractedText(request: ExtractedTextRequest?, flags: Int): ExtractedText {
            val start = minOf(pageInputCaret, pageInputAnchor).coerceIn(0, pageInputText.length)
            val end = maxOf(pageInputCaret, pageInputAnchor).coerceIn(0, pageInputText.length)
            return ExtractedText().also {
                it.text = pageInputText
                it.startOffset = 0
                it.partialStartOffset = -1
                it.partialEndOffset = -1
                it.selectionStart = start
                it.selectionEnd = end
            }
        }

        override fun setSelection(start: Int, end: Int): Boolean {
            return syncPageSelection(start, end)
        }

        override fun sendKeyEvent(event: KeyEvent): Boolean {
            return when (event.action) {
                KeyEvent.ACTION_DOWN -> handlePageEditShortcut(event) || dispatchKeyEventToPage(event, 0)
                KeyEvent.ACTION_UP -> dispatchKeyEventToPage(event, 1)
                else -> true
            }
        }

        override fun performEditorAction(actionCode: Int): Boolean {
            sendKeyToPage(0, "Enter", "Enter", 13, 0)
            sendKeyToPage(1, "Enter", "Enter", 13, 0)
            return true
        }

        override fun performContextMenuAction(id: Int): Boolean {
            return when (id) {
                android.R.id.selectAll -> {
                    syncPageSelection(0, pageInputText.length)
                    true
                }
                android.R.id.copy -> {
                    copySelectedPageInput()
                    true
                }
                android.R.id.cut -> {
                    cutSelectedPageInput()
                    true
                }
                android.R.id.paste, android.R.id.pasteAsPlainText -> {
                    pasteIntoPageInput()
                    true
                }
                else -> super.performContextMenuAction(id)
            }
        }
    }

    private fun releaseTracker() {
        velocityTracker?.recycle()
        velocityTracker = null
    }

    private inner class ScaleListener : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScaleBegin(detector: ScaleGestureDetector): Boolean {
            gestureWasScale = true
            dragging = false
            dragScrollGeneration++
            dragScrollPending = 0
            dragEnded = false
            removeCallbacks(longPressRunnable)
            return true
        }

        override fun onScale(detector: ScaleGestureDetector): Boolean {
            val old = effScale()
            val fx = detector.focusX
            val fy = detector.focusY
            val cssX = (scrollXpx + fx) / old
            val cssY = (scrollYpx + fy) / old
            userZoom = (userZoom * detector.scaleFactor).coerceIn(minZoom, maxZoom)
            val neo = effScale()
            scrollXpx = (cssX * neo - fx).roundToInt().coerceIn(0, maxScrollX())
            scrollYpx = (cssY * neo - fy).roundToInt().coerceIn(0, maxScrollY())
            scheduleRender()
            invalidate()
            return true
        }
    }
}
