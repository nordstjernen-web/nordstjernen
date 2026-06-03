# Nordstjernen for Java

A Java library that embeds the Nordstjernen browser engine through its C
embedding API (`src/libnordstjernen.h`) via a thin JNI bridge. Fetch, parse,
lay out, script and render web pages from the JVM — to raw RGBA, a
`BufferedImage`, a PNG/PDF file, or extracted text — plus the page title and
final URL, all links, and pixel link hit-testing.

**Requires JDK 21.**

## Use

```java
import org.nordstjernen.Nordstjernen;
import org.nordstjernen.Page;
import java.nio.file.Path;

try (Page page = Nordstjernen.open("https://example.com", 360, 600)) {
    page.title();                                  // page <title>
    page.url();                                    // final URL (after redirects)
    page.pageSize();                               // Size[width=…, height=…] (CSS px)
    page.text();                                   // extracted (laid-out) text
    page.links();                                  // List<String> of absolute link URLs
    page.linkAt(40, 72);                           // link URL at a CSS-px point, or null

    page.renderRgba(0, 0, 720, 1280, 2.0);         // premultiplied RGBA8888 bytes
    page.render(0, 0, 720, 1280, 2.0);             // BufferedImage of a viewport
    page.renderFullPage(2.0);                      // BufferedImage of the whole page
    page.renderToFile(Path.of("example.png"));     // .pdf path → PDF, else PNG
}
Nordstjernen.shutdown();
```

`open(url, viewportWidthCss, settleMs)` lays out at `viewportWidthCss` CSS
pixels (e.g. 360 for a phone, 1000 for desktop) and lets scripts/animations run
for `settleMs` before the final layout. Render at `scale` to map CSS pixels to
output device pixels (e.g. `2.0` for a HiDPI screenshot).

### One-shot helpers

For the common "open → do one thing → close" pattern (the engine stays
initialized; call `shutdown()` at the end):

```java
BufferedImage shot = Nordstjernen.screenshot("https://example.com", 360, 2.0);
Nordstjernen.saveScreenshot("https://example.com", 1000, Path.of("page.pdf"));
String text       = Nordstjernen.textOf("https://example.com", 1000);
List<String> urls = Nordstjernen.linksOf("https://example.com", 1000);
```

These make the headline use cases — thumbnails/OG images, HTML→PDF, search/RAG
text extraction, and link crawling — one line each.

### Threading & lifecycle

The engine uses a single GLib context and is **not** thread-safe; drive it from
one thread and treat `Page` as non-thread-safe. `Page` holds native memory —
always `close()` it (it is `AutoCloseable`). `Nordstjernen.shutdown()` releases
process-wide engine state.

## Build

```sh
# 1. Build the engine + JNI bridge (stages libs into src/main/resources/native/<os>-<arch>/)
JAVA_HOME=/path/to/jdk-21 java/scripts/build-native.sh

# 2. Build the jar (bundles the native libs)
cd java && gradle build      # -> build/libs/nordstjernen-java-<version>.jar
```

`scripts/build-native.sh` compiles `libnordstjernen` (via meson) and
`libnordstjernenjni` (the JNI bridge, linked with `RPATH=$ORIGIN` so it finds
the engine beside it), staging both into `src/main/resources/native/`. The
Gradle jar bundles them; `NativeLoader` extracts and loads them at runtime.

### Native library resolution

`NativeLoader` resolves, in order:
1. the directory in the `nordstjernen.native.dir` system property;
2. libraries bundled in the jar under `/native/<os>-<arch>/`;
3. the platform loader (`java.library.path` / `LD_LIBRARY_PATH`).

> The jar bundles `libnordstjernen` and the JNI bridge, but **not** the engine's
> own shared dependencies (GLib, cairo, pango, libcurl, sqlite, …). Those must
> be present on the host. Self-contained bundling of the whole stack is future
> work and tracks the Android dependency cross-build.

## CI

`.github/workflows/java.yml` builds the native libs and the jar on JDK 21, then
runs `examples/Example.java` against the **bundled** jar (exercising the
resource-extraction load path) and checks it renders `about:start`.
