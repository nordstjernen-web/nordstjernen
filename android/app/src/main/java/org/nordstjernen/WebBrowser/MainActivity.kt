/* Nordstjernen — Android host activity: URL bar over the engine render surface,
 * with history, find-in-page, sharing, printing, permission prompts and
 * rotation relayout. */

package org.nordstjernen.WebBrowser

import android.app.role.RoleManager
import android.app.DownloadManager
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.content.pm.ShortcutInfo
import android.content.pm.ShortcutManager
import android.content.res.Configuration
import android.graphics.drawable.Icon
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.CancellationSignal
import android.os.Environment
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.print.PageRange
import android.print.PrintAttributes
import android.print.PrintDocumentAdapter
import android.print.PrintDocumentInfo
import android.print.PrintManager
import android.provider.Settings
import android.text.Editable
import android.text.TextWatcher
import android.util.Log
import android.view.ActionMode
import android.view.Menu
import android.view.MenuItem
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.webkit.URLUtil
import android.widget.EditText
import android.widget.ImageButton
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

class MainActivity : AppCompatActivity() {

    companion object {
        private const val TAG = "nordstjernen"
        private const val STATE_PAGE_FOR_COMPUTER = "page_for_computer"
        private const val STATE_HISTORY = "history"
        private const val STATE_HISTORY_INDEX = "history_index"
    }

    private val ioExecutor = Executors.newSingleThreadExecutor()
    private val loadGen = AtomicInteger(0)

    // One list plus a cursor, so Forward works the way it does everywhere else;
    // it survives process death through onSaveInstanceState.
    private val history = ArrayList<String>()
    private var historyIndex = -1

    private lateinit var urlBar: EditText
    private lateinit var pageView: PageView
    private lateinit var progress: ProgressBar
    private lateinit var banner: TextView
    private lateinit var backButton: ImageButton
    private lateinit var forwardButton: ImageButton
    private lateinit var goButton: ImageButton
    private lateinit var swipeRefresh: SwipeRefreshLayout
    private lateinit var findBar: LinearLayout
    private lateinit var findField: EditText
    private lateinit var findCount: TextView

    private var initialized = false
    private var currentUrl: String? = null
    private var pageForComputer = false
    private var selectionMode: ActionMode? = null
    private val permissionAsked = HashSet<String>()

    private val browserRoleLauncher =
        registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {}

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContentView(R.layout.activity_main)

        // targetSdk 36 enforces edge-to-edge with no opt-out on Android 16+;
        // pad the root view by the system bar / cutout / IME insets so the
        // toolbar and page stay clear of them on every API level.
        val root = findViewById<View>(R.id.root)
        ViewCompat.setOnApplyWindowInsetsListener(root) { v, insets ->
            val bars = insets.getInsets(
                WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout()
            )
            val ime = insets.getInsets(WindowInsetsCompat.Type.ime())
            v.setPadding(bars.left, bars.top, bars.right, maxOf(bars.bottom, ime.bottom))
            WindowInsetsCompat.CONSUMED
        }

        urlBar = findViewById(R.id.urlBar)
        pageView = findViewById(R.id.pageView)
        progress = findViewById(R.id.progress)
        banner = findViewById(R.id.banner)
        backButton = findViewById(R.id.backButton)
        forwardButton = findViewById(R.id.forwardButton)
        goButton = findViewById(R.id.goButton)
        swipeRefresh = findViewById(R.id.swipeRefresh)
        findBar = findViewById(R.id.findBar)
        findField = findViewById(R.id.findField)
        findCount = findViewById(R.id.findCount)
        pageForComputer = savedInstanceState?.getBoolean(STATE_PAGE_FOR_COMPUTER) ?: false
        savedInstanceState?.getStringArrayList(STATE_HISTORY)?.let {
            history.addAll(it)
            historyIndex = savedInstanceState.getInt(STATE_HISTORY_INDEX, history.size - 1)
        }

        urlBar.isFocusable = true
        urlBar.isFocusableInTouchMode = true
        goButton.visibility = View.GONE
        goButton.setOnClickListener { navigate(urlBar.text.toString()) }
        findViewById<ImageButton>(R.id.reloadButton).setOnClickListener { reload() }
        findViewById<ImageButton>(R.id.homeButton).setOnClickListener { navigate(getString(R.string.home_url)) }
        findViewById<ImageButton>(R.id.menuButton).setOnClickListener { showAppMenu() }
        backButton.setOnClickListener { goBack() }
        forwardButton.setOnClickListener { goForward() }
        urlBar.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_GO) { navigate(urlBar.text.toString()); true } else false
        }
        urlBar.setOnTouchListener { _, event ->
            if (event.actionMasked == MotionEvent.ACTION_DOWN) {
                pageView.releaseTextInput()
                urlBar.requestFocusFromTouch()
                showUrlKeyboard(true)
            }
            false
        }
        urlBar.setOnClickListener { showUrlKeyboard() }
        urlBar.setOnFocusChangeListener { _, hasFocus ->
            updateUrlGoButton()
            if (hasFocus) {
                pageView.releaseTextInput()
                showUrlKeyboard(true)
            }
        }
        urlBar.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                updateUrlGoButton()
            }
            override fun afterTextChanged(s: Editable?) {}
        })

        swipeRefresh.setOnRefreshListener { reload() }
        buildFindBar()

        pageView.renderScale = resources.displayMetrics.density.toDouble()
        pageView.onNavigate = { url -> navigateFromPage(url) }
        pageView.onDownload = { download -> handleDownload(download) }
        pageView.onLinkLongPress = { url -> showLinkMenu(url) }
        pageView.onViewportWidthChanged = { relayoutForViewport() }
        pageView.onWebglPrompt = { origin -> promptWebgl(origin) }
        pageView.onCameraPrompt = { origin -> promptCamera(origin) }
        pageView.onSelectionStarted = { startSelectionActionMode() }
        pageView.onMediaTapped = { url, isVideo -> openMedia(url, isVideo) }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (findBar.visibility == View.VISIBLE) { closeFind(); return }
                if (!goBack()) { isEnabled = false; onBackPressedDispatcher.onBackPressed() }
            }
        })
        updateNavButtons()

        if (!NativeBrowser.available) {
            banner.visibility = View.VISIBLE
            banner.text = getString(R.string.engine_unavailable)
            return
        }

        maybeRequestBrowserRole()

        ioExecutor.execute {
            val caBundle = extractCaBundle()
            extractBundledDocs()
            val rc = NativeBrowser.nativeInit(filesDir.absolutePath, caBundle)
            initialized = rc == 0
            runOnUiThread {
                if (initialized) {
                    applyDisplayPrefs()
                    // A recreated activity (night mode, locale, low memory)
                    // resumes where it was rather than starting over.
                    val resumed = if (intent?.data == null && historyIndex >= 0)
                        history[historyIndex] else null
                    if (resumed != null) load(resumed) else navigate(initialUrl())
                } else {
                    banner.visibility = View.VISIBLE
                    banner.text = getString(R.string.engine_init_failed)
                }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        if (initialized) navigate(initialUrl())
    }

    override fun onResume() {
        super.onResume()
        if (::pageView.isInitialized) pageView.redrawCurrentPage()
    }

    /**
     * Mirror the system's dark theme and its "remove animations" accessibility
     * switch into the engine, so a page's
     * {@code @media (prefers-color-scheme: dark)} and
     * {@code prefers-reduced-motion} rules match the rest of the device.
     */
    private fun applyDisplayPrefs() {
        val night = (resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
        val animScale = runCatching {
            Settings.Global.getFloat(contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f)
        }.getOrDefault(1f)
        Log.i(TAG, "display prefs dark=$night animatorScale=$animScale")
        runCatching { NativeBrowser.nativeSetDisplayPrefs(night, animScale == 0f) }
    }

    /**
     * The URL this launch is about: an http(s) VIEW intent, a link or snippet
     * shared to the app, a system web search, or text sent through the
     * "process text" menu of another app.
     */
    private fun initialUrl(): String {
        val i = intent ?: return getString(R.string.home_url)
        val data = i.data?.toString()
        if (!data.isNullOrEmpty()) return data
        val shared = when (i.action) {
            Intent.ACTION_SEND -> i.getStringExtra(Intent.EXTRA_TEXT)
            Intent.ACTION_WEB_SEARCH -> i.getStringExtra(android.app.SearchManager.QUERY)
            Intent.ACTION_PROCESS_TEXT -> i.getCharSequenceExtra(Intent.EXTRA_PROCESS_TEXT)?.toString()
            else -> null
        }
        if (!shared.isNullOrBlank()) return normalizeUrl(shared.trim())
        return getString(R.string.home_url)
    }

    private fun navigate(input: String) {
        if (!initialized) return
        val raw = input.trim()
        if (raw.isEmpty()) return
        pushHistory(normalizeUrl(raw))
        load(history[historyIndex], fromHistory = false)
    }

    private fun navigateFromPage(url: String) {
        if (!initialized || url.isEmpty()) return
        pushHistory(url)
        load(history[historyIndex], reuseCurrent = true, fromHistory = false)
    }

    private fun pushHistory(url: String) {
        if (historyIndex >= 0 && history[historyIndex] == url) return
        while (history.size > historyIndex + 1) {
            history.removeAt(history.size - 1)
        }
        history.add(url)
        historyIndex = history.size - 1
    }

    private fun reload() {
        currentUrl?.let { load(it, reuseCurrent = true, fromHistory = false) }
            ?: run { swipeRefresh.isRefreshing = false }
    }

    private fun goBack(): Boolean {
        if (historyIndex <= 0) return false
        historyIndex--
        load(history[historyIndex], reuseCurrent = true, fromHistory = true)
        return true
    }

    private fun goForward(): Boolean {
        if (historyIndex >= history.size - 1) return false
        historyIndex++
        load(history[historyIndex], reuseCurrent = true, fromHistory = true)
        return true
    }

    private fun load(url: String, reuseCurrent: Boolean = false, fromHistory: Boolean = false) {
        if (!initialized) return
        currentUrl = url
        urlBar.setText(url)
        urlBar.clearFocus()
        updateUrlGoButton()
        pageView.requestFocus()
        hideKeyboard()
        closeFind()
        finishSelectionActionMode()
        updateNavButtons()
        progress.visibility = View.VISIBLE

        val gen = loadGen.incrementAndGet()
        // The engine lays out in CSS px; convert the device-px surface width to
        // CSS px (≈ dp) so pages get a phone-width mobile layout, then render
        // scaled by the density for crisp text.
        val density = resources.displayMetrics.density.toDouble()
        val widthPx = if (pageView.width > 0) pageView.width else resources.displayMetrics.widthPixels
        val heightPx = if (pageView.height > 0) pageView.height else resources.displayMetrics.heightPixels
        val scale = if (pageForComputer) widthPx.toDouble() / 1000.0 else density
        val viewportCss = if (pageForComputer) 1000 else Math.max(320, (widthPx / scale).toInt())
        val viewportCssHeight = Math.max(240, (heightPx / scale).toInt())
        pageView.renderScale = scale
        NativeBrowser.nativeSetDesktopMode(pageForComputer)
        val settleMs = NativeBrowser.nativeDefaultSettleMs()
        val started = SystemClock.uptimeMillis()
        Log.i(TAG, "load start url=$url viewport=${viewportCss}x$viewportCssHeight view=${widthPx}x$heightPx density=$density settle=$settleMs history=$fromHistory gen=$gen")
        // Reusing the renderer needs a document in it; the first load, and any
        // load after one failed, has to open a fresh one.
        if (reuseCurrent && pageView.hasDocument) {
            pageView.navigateCurrent(url, viewportCss, viewportCssHeight, settleMs, fromHistory) nav@{ ok, size, finalUrl, title ->
                val elapsed = SystemClock.uptimeMillis() - started
                Log.i(TAG, "load nativeNavigate url=$url final=${finalUrl ?: ""} ok=$ok size=${size?.getOrNull(0)}x${size?.getOrNull(1)} title=${title ?: ""} elapsed=${elapsed}ms")
                if (gen != loadGen.get()) {
                    Log.i(TAG, "load stale url=$url gen=$gen current=${loadGen.get()}")
                    return@nav
                }
                finishLoad(ok, url, size, finalUrl, title, fresh = false)
            }
            return
        }
        ioExecutor.execute {
            val handle = NativeBrowser.nativeOpen(url, viewportCss, viewportCssHeight, settleMs)
            val size = if (handle != 0L) NativeBrowser.nativePageSize(handle) else null
            val finalUrl = if (handle != 0L) NativeBrowser.nativeUrl(handle) else null
            val title = if (handle != 0L) NativeBrowser.nativeTitle(handle) else null
            val elapsed = SystemClock.uptimeMillis() - started
            Log.i(TAG, "load nativeOpen url=$url final=${finalUrl ?: ""} handle=$handle size=${size?.getOrNull(0)}x${size?.getOrNull(1)} title=${title ?: ""} elapsed=${elapsed}ms")
            runOnUiThread {
                if (gen != loadGen.get()) {
                    Log.i(TAG, "load stale url=$url gen=$gen current=${loadGen.get()}")
                    if (handle != 0L) NativeBrowser.nativeClose(handle)
                    return@runOnUiThread
                }
                if (handle == 0L || size == null) {
                    if (handle != 0L) NativeBrowser.nativeClose(handle)
                    finishLoad(false, url, null, null, null, fresh = true)
                    return@runOnUiThread
                }
                pageView.setDocument(handle, size[0], size[1])
                finishLoad(true, url, size, finalUrl, title, fresh = true)
            }
        }
    }

    private fun finishLoad(ok: Boolean, url: String, size: IntArray?, finalUrl: String?,
                           title: String?, fresh: Boolean) {
        progress.visibility = View.GONE
        swipeRefresh.isRefreshing = false
        if (!ok || size == null) {
            Log.e(TAG, "load failed url=$url fresh=$fresh")
            Toast.makeText(this, getString(R.string.load_failed, url), Toast.LENGTH_SHORT).show()
            return
        }
        val displayUrl = if (!finalUrl.isNullOrEmpty()) finalUrl else url
        if (displayUrl != currentUrl) {
            Log.i(TAG, "load displayUrl requested=$url final=$displayUrl")
            currentUrl = displayUrl
            urlBar.setText(displayUrl)
            if (historyIndex >= 0) history[historyIndex] = displayUrl
        }
        setTitle(if (!title.isNullOrEmpty()) title else getString(R.string.app_name))
        if (!fresh) pageView.updateDocument(size[0], size[1])
        updateSecurityBadge()
        updateNavButtons()
    }

    /**
     * Rotation: re-lay out the open page at the new viewport rather than
     * refetching it, so scripts, form state and the position in the document
     * all survive turning the device.
     */
    private fun relayoutForViewport() {
        val url = currentUrl ?: return
        if (!initialized) return
        val density = resources.displayMetrics.density.toDouble()
        val widthPx = if (pageView.width > 0) pageView.width else resources.displayMetrics.widthPixels
        val heightPx = if (pageView.height > 0) pageView.height else resources.displayMetrics.heightPixels
        val scale = if (pageForComputer) widthPx.toDouble() / 1000.0 else density
        val viewportCss = if (pageForComputer) 1000 else Math.max(320, (widthPx / scale).toInt())
        val viewportCssHeight = Math.max(240, (heightPx / scale).toInt())
        pageView.renderScale = scale
        Log.i(TAG, "relayout url=$url viewport=${viewportCss}x$viewportCssHeight")
        pageView.relayoutViewport(viewportCss, viewportCssHeight)
    }

    // --- Find in page --------------------------------------------------------

    private fun buildFindBar() {
        findField.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {
                runFind(0)
            }
            override fun afterTextChanged(s: Editable?) {}
        })
        findField.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_SEARCH) { runFind(1); true } else false
        }
        findViewById<ImageButton>(R.id.findPrev).setOnClickListener { runFind(2) }
        findViewById<ImageButton>(R.id.findNext).setOnClickListener { runFind(1) }
        findViewById<ImageButton>(R.id.findClose).setOnClickListener { closeFind() }
    }

    private fun openFind() {
        if (currentUrl == null) return
        findBar.visibility = View.VISIBLE
        findField.requestFocus()
        findField.selectAll()
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(findField, InputMethodManager.SHOW_IMPLICIT)
        if (findField.text.isNotEmpty()) runFind(0)
    }

    private fun closeFind() {
        if (findBar.visibility != View.VISIBLE) return
        findBar.visibility = View.GONE
        findCount.text = ""
        pageView.find("", 0) { _, _ -> }
        hideKeyboard()
        pageView.requestFocus()
    }

    private fun runFind(direction: Int) {
        if (findBar.visibility != View.VISIBLE) return
        val query = findField.text.toString()
        pageView.find(query, direction) { total, current ->
            findCount.text = when {
                query.isEmpty() -> ""
                total <= 0 -> getString(R.string.find_no_results)
                else -> getString(R.string.find_count, current, total)
            }
        }
    }

    // --- Text selection ------------------------------------------------------

    private fun startSelectionActionMode() {
        if (selectionMode != null) return
        selectionMode = startActionMode(object : ActionMode.Callback {
            override fun onCreateActionMode(mode: ActionMode, menu: Menu): Boolean {
                menu.add(0, 1, 0, R.string.copy)
                menu.add(0, 2, 1, R.string.share)
                menu.add(0, 3, 2, R.string.select_all)
                return true
            }

            override fun onPrepareActionMode(mode: ActionMode, menu: Menu) = false

            override fun onActionItemClicked(mode: ActionMode, item: MenuItem): Boolean {
                when (item.itemId) {
                    1 -> pageView.selectionText { text ->
                        if (!text.isNullOrEmpty()) {
                            copyToClipboard(text)
                            Toast.makeText(this@MainActivity, R.string.text_copied, Toast.LENGTH_SHORT).show()
                        }
                        mode.finish()
                    }
                    2 -> pageView.selectionText { text ->
                        if (!text.isNullOrEmpty()) shareText(text)
                        mode.finish()
                    }
                    3 -> { pageView.selectAll(); return true }
                    else -> return false
                }
                return true
            }

            override fun onDestroyActionMode(mode: ActionMode) {
                selectionMode = null
                pageView.clearSelection()
            }
        })
    }

    private fun finishSelectionActionMode() {
        selectionMode?.finish()
        selectionMode = null
    }

    // --- Permission prompts --------------------------------------------------

    private fun promptWebgl(origin: String) {
        if (!permissionAsked.add("webgl:$origin")) return
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.webgl_title, origin))
            .setMessage(getString(R.string.webgl_message, origin))
            .setPositiveButton(R.string.webgl_allow) { _, _ ->
                pageView.resolveWebgl(origin, true)
                currentUrl?.let { load(it, reuseCurrent = true) }
            }
            .setNegativeButton(R.string.permission_block) { _, _ ->
                pageView.resolveWebgl(origin, false)
            }
            .setCancelable(false)
            .show()
    }

    private fun promptCamera(origin: String) {
        if (!permissionAsked.add("camera:$origin")) return
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.camera_title, origin))
            .setMessage(getString(R.string.camera_message, origin))
            .setPositiveButton(R.string.camera_allow) { _, _ -> pageView.resolveCamera(origin, true) }
            .setNegativeButton(R.string.permission_block) { _, _ -> pageView.resolveCamera(origin, false) }
            .setCancelable(false)
            .show()
    }

    // --- Menus ---------------------------------------------------------------

    private fun showLinkMenu(url: String) {
        val items = arrayOf(
            getString(R.string.open),
            getString(R.string.copy_link),
            getString(R.string.share_link)
        )
        AlertDialog.Builder(this)
            .setTitle(url)
            .setItems(items) { _, which ->
                when (which) {
                    0 -> navigate(url)
                    1 -> {
                        copyToClipboard(url)
                        Toast.makeText(this, getString(R.string.link_copied), Toast.LENGTH_SHORT).show()
                    }
                    2 -> shareText(url)
                }
            }
            .show()
    }

    private fun showAppMenu() {
        val items = arrayOf(
            getString(R.string.find_in_page),
            getString(R.string.share_page),
            getString(R.string.copy_page_address),
            getString(R.string.print_page),
            getString(R.string.add_to_home_screen),
            getString(if (pageForComputer) R.string.page_for_mobile else R.string.page_for_computer),
            getString(R.string.history),
            getString(R.string.settings),
            getString(R.string.about_nordstjernen),
            getString(R.string.licenses),
            getString(R.string.open_website),
            getString(R.string.privacy_policy)
        )
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.app_name))
            .setItems(items) { _, which ->
                when (which) {
                    0 -> openFind()
                    1 -> currentUrl?.let { shareText(it) }
                    2 -> currentUrl?.let {
                        copyToClipboard(it)
                        Toast.makeText(this, R.string.page_address_copied, Toast.LENGTH_SHORT).show()
                    }
                    3 -> printPage()
                    4 -> pinCurrentPage()
                    5 -> {
                        pageForComputer = !pageForComputer
                        currentUrl?.let { load(it, reuseCurrent = true) }
                    }
                    6 -> navigate("about:history")
                    7 -> navigate("about:settings")
                    8 -> navigate("about:nordstjernen")
                    9 -> navigate("about:license")
                    10 -> navigate("https://nordstjernen.org")
                    11 -> navigate("https://nordstjernen.org/privacy")
                }
            }
            .show()
    }

    // --- Sharing, shortcuts, printing, media ---------------------------------

    private fun shareText(text: String) {
        val send = Intent(Intent.ACTION_SEND).apply {
            type = "text/plain"
            putExtra(Intent.EXTRA_TEXT, text)
            title?.let { putExtra(Intent.EXTRA_SUBJECT, it.toString()) }
        }
        startActivity(Intent.createChooser(send, getString(R.string.share_chooser)))
    }

    private fun copyToClipboard(text: String) {
        val cm = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("text", text))
    }

    /** Put the current page on the launcher, the way a bookmark would be. */
    private fun pinCurrentPage() {
        val url = currentUrl ?: return
        val manager = getSystemService(ShortcutManager::class.java)
        if (manager == null || !manager.isRequestPinShortcutSupported) {
            Toast.makeText(this, R.string.shortcut_failed, Toast.LENGTH_SHORT).show()
            return
        }
        val label = title?.toString()?.takeIf { it.isNotBlank() } ?: url
        val shortcut = ShortcutInfo.Builder(this, "page:$url")
            .setShortLabel(label.take(24))
            .setLongLabel(label.take(64))
            .setIcon(Icon.createWithResource(this, R.mipmap.ic_launcher))
            .setIntent(Intent(Intent.ACTION_VIEW, Uri.parse(url), this, MainActivity::class.java))
            .build()
        val pinned = runCatching { manager.requestPinShortcut(shortcut, null) }.getOrDefault(false)
        if (!pinned) {
            Toast.makeText(this, R.string.shortcut_failed, Toast.LENGTH_SHORT).show()
        }
    }

    /**
     * Print through the system print service — which is also how Android's
     * "Save as PDF" works. The engine renders the whole page to a PDF in the
     * cache directory and the adapter hands that file to the spooler.
     */
    private fun printPage() {
        if (currentUrl == null) return
        val out = File(cacheDir, "print.pdf")
        pageView.exportPage(out.absolutePath) { ok ->
            if (!ok || !out.isFile || out.length() == 0L) {
                Toast.makeText(this, R.string.print_failed, Toast.LENGTH_SHORT).show()
                return@exportPage
            }
            val manager = getSystemService(PRINT_SERVICE) as PrintManager
            val jobName = title?.toString()?.takeIf { it.isNotBlank() }
                ?: getString(R.string.print_job)
            manager.print(jobName, PdfFileAdapter(out, jobName),
                PrintAttributes.Builder().build())
        }
    }

    private class PdfFileAdapter(
        private val file: File,
        private val jobName: String,
    ) : PrintDocumentAdapter() {

        override fun onLayout(
            oldAttributes: PrintAttributes?,
            newAttributes: PrintAttributes?,
            cancellationSignal: CancellationSignal?,
            callback: LayoutResultCallback,
            extras: Bundle?,
        ) {
            if (cancellationSignal?.isCanceled == true) {
                callback.onLayoutCancelled()
                return
            }
            val info = PrintDocumentInfo.Builder(jobName)
                .setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT)
                .setPageCount(PrintDocumentInfo.PAGE_COUNT_UNKNOWN)
                .build()
            callback.onLayoutFinished(info, true)
        }

        override fun onWrite(
            pages: Array<out PageRange>?,
            destination: ParcelFileDescriptor,
            cancellationSignal: CancellationSignal?,
            callback: WriteResultCallback,
        ) {
            try {
                FileInputStream(file).use { input ->
                    FileOutputStream(destination.fileDescriptor).use { output ->
                        input.copyTo(output)
                    }
                }
                if (cancellationSignal?.isCanceled == true) {
                    callback.onWriteCancelled()
                } else {
                    callback.onWriteFinished(arrayOf(PageRange.ALL_PAGES))
                }
            } catch (t: Throwable) {
                Log.e(TAG, "print write failed", t)
                callback.onWriteFailed(t.message)
            }
        }
    }

    /** Hand an audio/video URL the engine cannot play inline to another app. */
    private fun openMedia(url: String, isVideo: Boolean) {
        val uri = runCatching { Uri.parse(url) }.getOrNull() ?: return
        val view = Intent(Intent.ACTION_VIEW).apply {
            setDataAndType(uri, if (isVideo) "video/*" else "audio/*")
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        val ok = runCatching {
            startActivity(view)
            true
        }.getOrDefault(false)
        Toast.makeText(this,
            if (ok) R.string.media_opening else R.string.media_no_handler,
            Toast.LENGTH_SHORT).show()
    }

    private fun handleDownload(download: String) {
        val tab = download.indexOf('\t')
        val url = if (tab >= 0) download.substring(0, tab) else download
        val suggested = if (tab >= 0) download.substring(tab + 1) else ""
        val uri = runCatching { Uri.parse(url) }.getOrNull()
        val scheme = uri?.scheme ?: ""
        if (uri == null || !(scheme.equals("http", true) || scheme.equals("https", true))) {
            Toast.makeText(this, getString(R.string.download_failed), Toast.LENGTH_SHORT).show()
            return
        }
        val guessed = if (suggested.isNotBlank()) suggested else URLUtil.guessFileName(url, null, null)
        val filename = safeDownloadName(guessed)
        val request = DownloadManager.Request(uri)
            .setTitle(filename)
            .setDescription(url)
            .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
            .setAllowedOverMetered(true)
            .setAllowedOverRoaming(true)
            .setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, filename)
        runCatching {
            (getSystemService(DOWNLOAD_SERVICE) as DownloadManager).enqueue(request)
        }.onSuccess {
            Toast.makeText(this, getString(R.string.download_started, filename), Toast.LENGTH_SHORT).show()
        }.onFailure { t ->
            Log.e(TAG, "download failed url=$url filename=$filename", t)
            Toast.makeText(this, getString(R.string.download_failed), Toast.LENGTH_SHORT).show()
        }
    }

    private fun safeDownloadName(name: String): String {
        var out = name.substringAfterLast('/').substringAfterLast('\\').trim()
        if (out.isEmpty() || out == "." || out == "..") out = "download"
        out = out.replace(Regex("[\\r\\n\\t\\u0000/\\\\:*?\"<>|]"), "_")
        return out.take(96).ifEmpty { "download" }
    }

    private fun normalizeUrl(input: String): String {
        if (input.startsWith("about:") || input.contains("://")) return input
        if (!input.contains('.') || input.contains(' ')) {
            return "https://duckduckgo.com/html/?q=" + Uri.encode(input)
        }
        return "https://$input"
    }

    private fun updateNavButtons() {
        val canBack = historyIndex > 0
        val canForward = historyIndex in 0 until history.size - 1
        backButton.isEnabled = canBack
        backButton.alpha = if (canBack) 1f else 0.38f
        forwardButton.isEnabled = canForward
        forwardButton.alpha = if (canForward) 1f else 0.38f
    }

    /** A lock or a warning at the head of the URL bar, matching the GTK shell. */
    private fun updateSecurityBadge() {
        val icon = when (pageView.security()) {
            NativeBrowser.SECURITY_SECURE -> R.drawable.ic_secure
            NativeBrowser.SECURITY_INSECURE -> R.drawable.ic_insecure
            else -> 0
        }
        urlBar.setCompoundDrawablesRelativeWithIntrinsicBounds(icon, 0, 0, 0)
        urlBar.contentDescription = when (pageView.security()) {
            NativeBrowser.SECURITY_SECURE -> getString(R.string.secure_page)
            NativeBrowser.SECURITY_INSECURE -> getString(R.string.insecure_page)
            else -> null
        }
    }

    private fun updateUrlGoButton() {
        goButton.visibility =
            if (urlBar.hasFocus() && urlBar.text.toString().trim().isNotEmpty())
                View.VISIBLE
            else
                View.GONE
    }

    // Offer the system default-browser chooser once, on first launch
    // (RoleManager exists on Android 10+; the user can change it any time
    // in Settings, so never re-prompt).
    private fun maybeRequestBrowserRole() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val roleManager = getSystemService(RoleManager::class.java) ?: return
        if (!roleManager.isRoleAvailable(RoleManager.ROLE_BROWSER)) return
        if (roleManager.isRoleHeld(RoleManager.ROLE_BROWSER)) return
        val prefs = getPreferences(MODE_PRIVATE)
        if (prefs.getBoolean("browser_role_asked", false)) return
        prefs.edit().putBoolean("browser_role_asked", true).apply()
        browserRoleLauncher.launch(roleManager.createRequestRoleIntent(RoleManager.ROLE_BROWSER))
    }

    private fun extractCaBundle(): String {
        val systemBundle = File("/system/etc/security/cacerts.pem")
        if (systemBundle.exists()) return systemBundle.absolutePath
        val out = File(filesDir, "cacert.pem")
        runCatching {
            resources.openRawResource(R.raw.cacert).use { input ->
                out.outputStream().use { input.copyTo(it) }
            }
        }
        return if (out.exists()) out.absolutePath else ""
    }

    private fun extractBundledDocs() {
        val outDir = File(filesDir, "nordstjernen")
        extractAsset("License.md", File(outDir, "License.md"))
        extractAsset("THIRD-PARTY-LICENSES.md", File(outDir, "THIRD-PARTY-LICENSES.md"))
    }

    private fun extractAsset(name: String, out: File) {
        runCatching {
            out.parentFile?.mkdirs()
            assets.open(name).use { input ->
                out.outputStream().use { input.copyTo(it) }
            }
        }.onFailure {
            Log.w(TAG, "asset extract failed name=$name", it)
        }
    }

    private fun hideKeyboard() {
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(urlBar.windowToken, 0)
    }

    private fun showUrlKeyboard(selectAll: Boolean = false) {
        urlBar.post {
            urlBar.requestFocusFromTouch()
            if (selectAll) urlBar.selectAll()
            val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
            imm.restartInput(urlBar)
            imm.showSoftInput(urlBar, InputMethodManager.SHOW_IMPLICIT)
            Log.i(TAG, "urlBar focus=${urlBar.hasFocus()} textLen=${urlBar.length()}")
        }
    }

    override fun onDestroy() {
        pageView.recycleDocument()
        super.onDestroy()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putBoolean(STATE_PAGE_FOR_COMPUTER, pageForComputer)
        outState.putStringArrayList(STATE_HISTORY, ArrayList(history))
        outState.putInt(STATE_HISTORY_INDEX, historyIndex)
        super.onSaveInstanceState(outState)
    }
}
