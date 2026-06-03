/* Nordstjernen — Android host activity: URL bar over the engine render surface,
 * with history, reload, link following and rotation relayout. */

package com.nordstjernen.browser

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Intent
import android.os.Bundle
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import java.io.File
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

class MainActivity : AppCompatActivity() {

    private val ioExecutor = Executors.newSingleThreadExecutor()
    private val loadGen = AtomicInteger(0)
    private val backStack = ArrayDeque<String>()

    private lateinit var urlBar: EditText
    private lateinit var pageView: PageView
    private lateinit var progress: ProgressBar
    private lateinit var banner: TextView
    private lateinit var backButton: Button

    private var initialized = false
    private var currentUrl: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        urlBar = findViewById(R.id.urlBar)
        pageView = findViewById(R.id.pageView)
        progress = findViewById(R.id.progress)
        banner = findViewById(R.id.banner)
        backButton = findViewById(R.id.backButton)

        findViewById<Button>(R.id.goButton).setOnClickListener { navigate(urlBar.text.toString()) }
        findViewById<Button>(R.id.reloadButton).setOnClickListener { reload() }
        backButton.setOnClickListener { goBack() }
        urlBar.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_GO) { navigate(urlBar.text.toString()); true } else false
        }

        pageView.renderScale = resources.displayMetrics.density.toDouble()
        pageView.onNavigate = { url -> navigate(url) }
        pageView.onLinkLongPress = { url -> showLinkMenu(url) }
        pageView.onViewportWidthChanged = { currentUrl?.let { load(it) } }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                if (!goBack()) { isEnabled = false; onBackPressedDispatcher.onBackPressed() }
            }
        })
        updateBackButton()

        if (!NativeBrowser.available) {
            banner.visibility = View.VISIBLE
            banner.text = getString(R.string.engine_unavailable)
            return
        }

        ioExecutor.execute {
            val caBundle = extractCaBundle()
            val rc = NativeBrowser.nativeInit(filesDir.absolutePath, caBundle)
            initialized = rc == 0
            runOnUiThread {
                if (initialized) {
                    navigate(initialUrl())
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

    private fun initialUrl(): String {
        val data = intent?.data?.toString()
        return if (!data.isNullOrEmpty()) data else getString(R.string.home_url)
    }

    private fun navigate(input: String) {
        if (!initialized) return
        val raw = input.trim()
        if (raw.isEmpty()) return
        val url = normalizeUrl(raw)
        currentUrl?.let { if (it != url) backStack.addLast(it) }
        load(url)
    }

    private fun reload() {
        currentUrl?.let { load(it) }
    }

    private fun goBack(): Boolean {
        if (backStack.isEmpty()) return false
        load(backStack.removeLast())
        return true
    }

    private fun load(url: String) {
        if (!initialized) return
        currentUrl = url
        urlBar.setText(url)
        hideKeyboard()
        updateBackButton()
        progress.visibility = View.VISIBLE

        val gen = loadGen.incrementAndGet()
        // The engine lays out in CSS px; convert the device-px surface width to
        // CSS px (≈ dp) so pages get a phone-width mobile layout, then render
        // scaled by the density for crisp text.
        val density = resources.displayMetrics.density.toDouble()
        val widthPx = if (pageView.width > 0) pageView.width else resources.displayMetrics.widthPixels
        val viewportCss = Math.max(320, (widthPx / density).toInt())
        ioExecutor.execute {
            val handle = NativeBrowser.nativeOpen(url, viewportCss, 600)
            val size = if (handle != 0L) NativeBrowser.nativePageSize(handle) else null
            val title = if (handle != 0L) NativeBrowser.nativeTitle(handle) else null
            runOnUiThread {
                if (gen != loadGen.get()) {
                    if (handle != 0L) NativeBrowser.nativeClose(handle)
                    return@runOnUiThread
                }
                progress.visibility = View.GONE
                if (handle == 0L || size == null) {
                    if (handle != 0L) NativeBrowser.nativeClose(handle)
                    Toast.makeText(this, getString(R.string.load_failed, url), Toast.LENGTH_SHORT).show()
                    return@runOnUiThread
                }
                setTitle(if (!title.isNullOrEmpty()) title else getString(R.string.app_name))
                pageView.setDocument(handle, size[0], size[1])
            }
        }
    }

    private fun showLinkMenu(url: String) {
        val items = arrayOf(getString(R.string.open), getString(R.string.copy_link))
        AlertDialog.Builder(this)
            .setTitle(url)
            .setItems(items) { _, which ->
                when (which) {
                    0 -> navigate(url)
                    1 -> {
                        val cm = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
                        cm.setPrimaryClip(ClipData.newPlainText("url", url))
                        Toast.makeText(this, getString(R.string.link_copied), Toast.LENGTH_SHORT).show()
                    }
                }
            }
            .show()
    }

    private fun normalizeUrl(input: String): String {
        if (input.startsWith("about:") || input.contains("://")) return input
        if (!input.contains('.') || input.contains(' ')) {
            return "https://duckduckgo.com/html/?q=" + android.net.Uri.encode(input)
        }
        return "https://$input"
    }

    private fun updateBackButton() {
        backButton.isEnabled = backStack.isNotEmpty()
    }

    private fun extractCaBundle(): String {
        val systemBundle = File("/system/etc/security/cacerts.pem")
        if (systemBundle.exists()) return systemBundle.absolutePath
        val out = File(filesDir, "cacert.pem")
        if (!out.exists()) {
            runCatching {
                resources.openRawResource(R.raw.cacert).use { input ->
                    out.outputStream().use { input.copyTo(it) }
                }
            }
        }
        return if (out.exists()) out.absolutePath else ""
    }

    private fun hideKeyboard() {
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(urlBar.windowToken, 0)
    }

    override fun onDestroy() {
        pageView.recycleDocument()
        super.onDestroy()
    }
}
