# Embedding Nordstjernen in a C application

Nordstjernen ships a small C API for driving the engine from another
program: fetch a URL, run its scripts, lay it out, and pull back the
rendered text or a PNG/PDF image. There is no GTK window — it is the
same synchronous pipeline the `--headless` driver uses, exposed as a
library.

The public header is **`libnordstjernen.h`** and the build installs a
shared library (`libnordstjernen.so` / `.dll` / `.dylib`). The header
is plain C with no GLib or GTK types, so a consumer only needs the
header and the link flag.

## API

```c
int          nd_browser_init(void);
nd_browser  *nd_browser_open(const char *url, int viewport_width, int settle_ms);
char        *nd_browser_render_text(nd_browser *browser);
int          nd_browser_render_image(nd_browser *browser, const char *path);
void         nd_browser_close(nd_browser *browser);
void         nd_browser_shutdown(void);
```

| Function | Behaviour |
| --- | --- |
| `nd_browser_init` | Brings up networking, the disk cache, and the font system. Call once before anything else; returns `0` on success. Enables `file://` and bare local-path loading. |
| `nd_browser_open` | Fetches `url`, decodes and parses the HTML, runs its scripts, pumps the event loop for `settle_ms` milliseconds, then lays the page out at `viewport_width` pixels (height is derived). `url` may be `http(s)://`, `file://`, `about:`, `data:`, or a local filesystem path. Returns an opaque handle, or `NULL` on a hard fetch/parse failure. |
| `nd_browser_render_text` | Returns the rendered visible text as a newly-allocated, NUL-terminated UTF-8 string. **The caller frees it with `free()`.** Returns `NULL` if nothing was laid out. |
| `nd_browser_render_image` | Renders the full page to `path`. A `.pdf` extension writes PDF; anything else writes PNG. Images are fetched lazily on the first call. Returns `0` on success. |
| `nd_browser_close` | Frees a handle from `nd_browser_open`. |
| `nd_browser_shutdown` | Tears down the subsystems brought up by `nd_browser_init`. |

## Example

```c
#include <libnordstjernen.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    if (nd_browser_init() != 0)
        return 1;

    nd_browser *b = nd_browser_open("https://example.org", 1024, 500);
    if (b) {
        char *text = nd_browser_render_text(b);
        if (text) {
            fputs(text, stdout);
            free(text);
        }
        nd_browser_render_image(b, "page.png");
        nd_browser_close(b);
    }

    nd_browser_shutdown();
    return 0;
}
```

Build against the installed library:

```sh
cc app.c -lnordstjernen -o app
```

Or against an uninstalled build tree:

```sh
cc app.c -I src \
   -L builddir/src -lnordstjernen \
   -Wl,-rpath,builddir/src -o app
```

## Notes

- The API is single-threaded and drives a private GLib main loop;
  call it from one thread and do not interleave instances across
  threads.
- `nd_browser_init` / `nd_browser_shutdown` bracket the whole session;
  open and close as many `nd_browser` handles between them as you like
  (one at a time).
- A larger `settle_ms` gives async scripts, fonts, and animations more
  time to converge before layout is captured; `0` skips the wait.
- The root-refusal guard and the seccomp/Landlock sandbox belong to
  the `nordstjernen` executable, not the library — the embedding host
  is responsible for its own privilege and sandbox posture.
