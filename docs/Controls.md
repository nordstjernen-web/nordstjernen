# Controls — keyboard, mouse, and touch

Every way to drive Nordstjernen's GTK shell from the keyboard and the
mouse. This is a living map of the actual bindings; the browser's runtime
behaviour is the source of truth. Window-level accelerators are registered
in `src/gtk/procwindow.c` (`install_shortcuts`); the in-page key and
pointer handling lives in `src/gtk/procview.c` (`on_key`, `on_pressed`,
`on_motion`, `on_scroll`).

Shortcuts are written with **Ctrl** and use a literal Ctrl modifier.

## Window shortcuts

These work from anywhere in the window.

### Navigation

| Shortcut | Action |
| --- | --- |
| `Ctrl+L` | Focus the address bar (selects the URL) |
| `Alt+←` | Back |
| `Alt+→` | Forward |
| `Alt+Home` | Go to the home page |
| `Ctrl+R` · `F5` | Reload |
| `Escape` | Return focus to the page |

Back and Forward restore the previous page from an in-memory back/forward
cache when it is eligible — a successful `http(s)` GET without a
`Cache-Control: no-store` directive — so the page reappears instantly with
its scripts' state intact and fires a `pageshow` event with `persisted`
true, instead of re-fetching and re-rendering. Reload always re-fetches.

### Tabs

| Shortcut | Action |
| --- | --- |
| `Ctrl+T` | New tab |
| `Ctrl+Alt+P` | New private tab (ephemeral — nothing saved to disk) |
| `Ctrl+W` | Close tab (closes the window if it is the last tab) |
| `Ctrl+Tab` · `Ctrl+Page Down` | Next tab |
| `Ctrl+Shift+Tab` · `Ctrl+Page Up` | Previous tab |
| `Ctrl+Q` | Quit |

### Zoom

| Shortcut | Action |
| --- | --- |
| `Ctrl++` · `Ctrl+=` | Zoom in |
| `Ctrl+-` | Zoom out |
| `Ctrl+0` | Reset zoom to 100% |

Zoom is clamped to the 0.25×–5.0× range.

### Find, tools, and settings

| Shortcut | Action |
| --- | --- |
| `Ctrl+F` | Open the find bar |
| `Ctrl+G` | Find next |
| `Ctrl+Shift+G` | Find previous |
| `Enter` / `Shift+Enter` | Next / previous match (while the find field is focused) |
| `Escape` | Close the find bar |
| `Ctrl+P` | Print the page through the system print dialog (see [printing.md](printing.md)) |
| `F12` · `Ctrl+Shift+J` | Toggle the developer console |
| `Ctrl+,` | Open Settings |

Bookmarks are managed from the bookmarks toolbar button (bookmark the
current page, open or remove saved bookmarks). The **☰** toolbar menu
offers New Tab, New Private Tab, Reload, Settings, and About.

A **private tab** (☰ menu → New Private Tab, or `Ctrl+Alt+P`) runs its own
renderer in an ephemeral mode: cookies, the HTTP cache, history,
`localStorage`, and IndexedDB are kept in memory and discarded when the
tab closes, so the session leaves no trace on disk. Private tabs are
marked with a distinct icon in the tab bar. Launching with `--private`
opens the first tab this way. (Private tabs require the default
multi-process mode; the entry is disabled under `--single-process`.)

Settings includes a **Search engine** drop-down with the common engines
(DuckDuckGo Lite — the default — DuckDuckGo, Baidu, Google, Bing, Yandex,
Yahoo, Yahoo! Japan, Sogou, Naver, Startpage, Brave, Ecosia) plus a
**Custom…** entry for any `…?q=%s`-style query URL. The choice drives both
the address-bar search and the `about:start` search box.

## In-page keys

These act on the rendered document and only fire when the page surface
has focus (click the page, or `Tab` into it). When a text input is
focused, typing and editing keys go to that field instead.

| Key | Action |
| --- | --- |
| `Tab` · `Shift+Tab` | Move focus between links and form fields |
| `Space` · `Shift+Space` | Scroll down / up by ~one viewport |
| `Page Down` · `Page Up` | Scroll down / up by ~one viewport |
| `Home` · `End` | Jump to the top / bottom of the page |
| `↑` · `↓` · `←` · `→` | Scroll by a few lines |
| `Ctrl+A` | Select all text on the page |
| `Ctrl+C` | Copy the selected text |

## Mouse

| Input | Action |
| --- | --- |
| Left click | Follow a link, activate a control, focus an input, or place the caret |
| Left drag | Select text |
| `Ctrl`+left click | Open the link in a new tab |
| Middle click | Open the link in a new tab |
| Right click | Open the context menu |
| Wheel | Scroll |
| `Ctrl`+wheel | Zoom the page in / out |

Moving the pointer updates the cursor (a hand over links) and shows a
link's target URL in the status bar while hovered, and `:hover` styling
and `mouseover`/`mousemove` JS events fire. Clicking an `<audio>` or
`<video>` poster hands the media URL to an external player.

The right-click context menu adapts to what was clicked: over a link it
offers Open Link / Open Link in New Tab / Copy Link Address; over a
selection it offers Copy; and every menu has Back, Forward, Reload,
Select All, Copy Page Address, and Save Page as PDF / Image.

## Touch

Basic touch is supported through GTK's gesture translation: a tap acts as
a left click and a touch drag selects text or drags the scrollbars. Pinch
zoom, kinetic/flick scrolling, and long-press menus are not implemented in
the process-per-tab shell yet.

> **Mobile sites vs. touch input.** Whether a mobile or a desktop page is
> requested is decided once for the whole build, not per host: the desktop
> shells always send the desktop user-agent, and the Android app sends the
> mobile one until "page for computer" is chosen
> (`ns_net_is_mobile_mode` in `src/net.c`). That is about which page a
> site serves, not about touch input.
