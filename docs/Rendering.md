# Rendering and scrolling

Nordstjernen renders each tab out of process. The GTK app is a thin
shell (`src/gtk/procview.c`) that spawns one sandboxed
`nordstjernen-renderer` process per tab (`src/renderer_http.c`) and drives it
over a tiny **HTTP/JSON control channel plus a shared-memory framebuffer**
(`src/rproc_http.c`, `src/ipc_http.c`). The engine — HTML parse, the CSS
cascade, layout into a live `ns_box` tree, and Cairo/Pango paint — runs entirely
inside the renderer child; the UI process only blits the framebuffer and
forwards input. The shape was chosen over an opaque binary struct protocol
because it is readable on the wire, has no slot-lifetime bookkeeping to get
wrong, and costs nothing in throughput: the pixels never travel over it.

Every control message is a plain HTTP `POST` with a small JSON body — `/open`,
`/render`, `/link`, `/click`, `/key`, `/hover`, `/find`, `/viewport`, `/select`,
`/eval`, `/console`, `/media`, `/export`, `/quit` — so the protocol is readable
in a trace and trivial to extend. Only the rendered pixels travel through shared
memory. The framebuffer is not a global named object: the shell backs it with an
anonymous `memfd_create` (Linux), an immediately-unlinked `shm_open` segment
(other Unix), or an unnamed inheritable file mapping (Windows), and hands the
renderer the descriptor/handle over the control channel rather than a name.

## The render path

For each frame the shell `POST`s `/render` with the target size, scroll offset,
and a `scale` that maps CSS pixels to device pixels. The renderer:

1. pumps pending JS/animation work for a small time budget
   (`ns_browser_tick`);
2. paints the requested viewport region of the layout tree straight into the
   shared-memory framebuffer in Cairo-native ARGB32
   (`ns_browser_render_argb32` over `src/paint.c`), with no intermediate
   surface;
3. replies with `X-W`/`X-H`/`X-Stride`/`X-Anim` headers and an empty body — the
   pixels are already in the shell's address space via the shared mapping.

Renders are coalesced so at most one is in flight, so the shell's per-view worker
thread copies the frame out of the shared mapping into its display surface
(a Cairo surface on GTK) before issuing the next render. The copy
runs off the UI thread; the data plane never crosses the socket. There is no
retained per-tile paint cache in the UI process — a frame is a full repaint of
the visible region inside the renderer, and the shared-memory handoff is the fast
path. While `ns_browser_animating()` is true
(active CSS transitions/animations or pending `requestAnimationFrame`), the
shell drives a frame loop; otherwise it renders on demand (scroll, resize,
hover restyle, find, click result).

Resizing re-lays-out the page for the new CSS-pixel viewport width in the
renderer (`VIEWPORT` → `ns_browser_set_viewport_width`), re-evaluating
`@media` queries, viewport units, and fluid widths.

## Isolation

Because the engine runs in a separate, sandboxed process, a crash or hang in
untrusted page content cannot take down the UI; the shell detects a dead
renderer and restarts the tab. The renderer is confined with Linux Landlock +
seccomp (`ns_browser_sandbox`); see `docs/tab-isolation.md`.

## Long documents

Layout and document install must stay linear in document size; a 15 MB
single-page document (html.spec.whatwg.org, ~470k nodes) is the working
reference. Two hot spots to keep in mind when touching these paths:

- `ns_doc_class_index_build` / `ns_doc_tag_index_build` append in
  document order without per-insert dedup scans (`g_doc_index_building`
  in `src/dom.c`). Only incremental mutation goes through the ordered
  insert with its linear duplicate check — never route a full rebuild
  through it.
- Static positions for `position: absolute` boxes are resolved in one
  postorder sweep per containing block
  (`static_abs_y_precompute` in `src/layout.c`), not per element.
  Per-element walks (`static_abs_y_walk`) remain only as the fallback
  for targets whose document position cannot be located by node rank;
  they prune subtrees that follow the target.

With `NS_PROFILE=1` (set in the renderer's environment), the
`js phases install=` figure covers the index builds, and `css.cascade=`
is expected to dominate large static pages once layout is linear.
