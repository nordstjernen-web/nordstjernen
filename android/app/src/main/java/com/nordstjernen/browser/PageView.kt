/* Nordstjernen — render surface: paints the engine's RGBA viewport, scrolls
 * (with fling) in 2D, pinch/double-tap zooms, follows tapped links and offers a
 * long-press menu. */

package com.nordstjernen.browser

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.os.SystemClock
import android.util.AttributeSet
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.VelocityTracker
import android.view.View
import android.view.ViewConfiguration
import android.widget.OverScroller
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.roundToInt

class PageView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {

    private val renderExecutor = Executors.newSingleThreadExecutor()
    private val scroller = OverScroller(context)
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private val minFlingVelocity = ViewConfiguration.get(context).scaledMinimumFlingVelocity.toFloat()
    private val maxFlingVelocity = ViewConfiguration.get(context).scaledMaximumFlingVelocity.toFloat()
    private val longPressTimeout = ViewConfiguration.getLongPressTimeout().toLong()
    private val doubleTapTimeout = ViewConfiguration.getDoubleTapTimeout().toLong()
    private val scaleDetector = ScaleGestureDetector(context, ScaleListener())

    private val minZoom = 1.0
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
    private var viewport: Bitmap? = null
    @Volatile private var renderPending = false

    private var lastContentTapTime = 0L
    private var lastContentTapX = 0f
    private var lastContentTapY = 0f

    // Scroll fraction to restore on the next setDocument (rotation relayout);
    // -1 means "fresh navigation, reset to top".
    private var pendingScrollFraction = -1f

    var onNavigate: ((url: String) -> Unit)? = null
    var onLinkLongPress: ((url: String) -> Unit)? = null
    var onViewportWidthChanged: ((cssWidth: Int) -> Unit)? = null

    // CSS-pixel -> device-pixel base scale (display density). Effective scale is
    // this times the user's zoom.
    var renderScale: Double = 1.0

    init {
        // Double-tap is our zoom toggle; don't let the detector hijack it for
        // quick-scale (double-tap-drag).
        scaleDetector.isQuickScaleEnabled = false
    }

    private fun effScale(): Double = renderScale * userZoom
    private fun contentW(): Int = (pageWidthCss * effScale()).roundToInt()
    private fun contentH(): Int = (pageHeightCss * effScale()).roundToInt()
    private fun maxScrollX(): Int = maxOf(0, contentW() - width)
    private fun maxScrollY(): Int = maxOf(0, contentH() - height)

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
        userZoom = 1.0
        scrollXpx = 0
        scrollYpx = if (pendingScrollFraction >= 0f)
            (pendingScrollFraction * contentH()).roundToInt().coerceIn(0, maxScrollY())
        else 0
        pendingScrollFraction = -1f
        scroller.forceFinished(true)
        scheduleRender()
    }

    fun recycleDocument() {
        val h = handle
        handle = 0
        if (h != 0L) renderExecutor.execute { NativeBrowser.nativeClose(h) }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        viewport?.recycle()
        viewport = if (w > 0 && h > 0) Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888) else null
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

    private fun scheduleRender() {
        val bmp = viewport ?: return
        if (handle == 0L || renderPending) return
        renderPending = true
        val eff = effScale()
        val sxc = (scrollXpx / eff).roundToInt()
        val syc = (scrollYpx / eff).roundToInt()
        renderExecutor.execute {
            val ok = NativeBrowser.nativeRender(handle, sxc, syc, eff, bmp)
            renderPending = false
            if (ok) postInvalidate()
        }
    }

    override fun onDraw(canvas: Canvas) {
        val bmp = viewport
        if (bmp != null && handle != 0L) {
            canvas.drawBitmap(bmp, 0f, 0f, null)
        } else {
            canvas.drawColor(Color.WHITE)
        }
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
        userZoom = if (userZoom > minZoom + 0.01) minZoom else doubleTapZoom
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
                    setScroll(scrollXpx + dx, scrollYpx + dy)
                }
                return true
            }
            MotionEvent.ACTION_UP -> {
                removeCallbacks(longPressRunnable)
                when {
                    gestureWasScale -> { /* end of a pinch — not a tap */ }
                    dragging -> {
                        tracker.computeCurrentVelocity(1000, maxFlingVelocity)
                        val vx = -tracker.xVelocity
                        val vy = -tracker.yVelocity
                        if ((abs(vx) > minFlingVelocity || abs(vy) > minFlingVelocity) &&
                            (maxScrollX() > 0 || maxScrollY() > 0)) {
                            scroller.fling(scrollXpx, scrollYpx, vx.toInt(), vy.toInt(),
                                0, maxScrollX(), 0, maxScrollY())
                            postInvalidateOnAnimation()
                        }
                    }
                    !longPressFired && handle != 0L -> {
                        val tapX = downX
                        val tapY = downY
                        hitLink(tapX.toInt(), tapY.toInt()) { url ->
                            if (url != null) onNavigate?.invoke(url)
                            else handleContentTap(tapX, tapY)
                        }
                    }
                }
                releaseTracker()
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                removeCallbacks(longPressRunnable)
                releaseTracker()
                return true
            }
        }
        return super.onTouchEvent(event)
    }

    private fun releaseTracker() {
        velocityTracker?.recycle()
        velocityTracker = null
    }

    private inner class ScaleListener : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScaleBegin(detector: ScaleGestureDetector): Boolean {
            gestureWasScale = true
            dragging = false
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
