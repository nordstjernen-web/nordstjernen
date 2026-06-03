# Third-party software notices

Nordstjernen links to (and in some cases statically includes) the
following open-source libraries. Their copyright notices and license
texts are reproduced below. For libraries shipped dynamically in the
release bundles, you are entitled by the LGPL terms to replace them
with modified versions; the binary will continue to function with any
ABI-compatible replacement.

The Nordstjernen source code itself is not open-source. See `README.md`
for the project's own license terms.

---

## Statically linked

### lexbor — Apache License 2.0

> HTML / CSS / WHATWG URL parser.
> <https://github.com/lexbor/lexbor>
>
> Copyright (c) 2018-2025 Alexander Borisov

Licensed under the Apache License, Version 2.0 (the "License"); you may
not use this software except in compliance with the License. You may
obtain a copy of the License at:

  <http://www.apache.org/licenses/LICENSE-2.0>

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
implied. See the License for the specific language governing permissions
and limitations under the License.

### Wuffs — Apache License 2.0

> Memory-safe PNG / GIF / BMP / JPEG decoders, transpiled from the
> Wuffs language to C. Vendored as the single-file release in
> `subprojects/wuffs/wuffs-v0.4.c`.
> <https://github.com/google/wuffs-mirror-release-c>
>
> Copyright (c) 2017 The Wuffs Authors.

Licensed under the Apache License, Version 2.0. See the lexbor section
above for the license text (same license).

### quickjs-ng — MIT License

> JavaScript engine.
> <https://github.com/quickjs-ng/quickjs>
>
> Copyright (c) 2017-2026 Fabrice Bellard
> Copyright (c) 2017-2026 Charlie Gordon
> Copyright (c) 2023-2026 the quickjs-ng contributors

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

---

## Dynamically linked

### libcurl — curl license (MIT-like)

> HTTP/TLS client.
> <https://curl.se>
>
> Copyright (c) 1996-2026 Daniel Stenberg, and many contributors.

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE
FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY
DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

### libuchardet — MPL-1.1 / LGPL-2.1+ / GPL-2.0+ (tri-license)

> Charset detection.
> <https://www.freedesktop.org/wiki/Software/uchardet/>
>
> Based on Mozilla's universalchardet, originally Copyright (c)
> 1998-2006 Netscape Communications Corporation and others.

Distributed under the terms of the Mozilla Public License 1.1, the
GNU Lesser General Public License 2.1, or the GNU General Public
License 2.0, at your option. The full license texts are available at:

- <https://www.mozilla.org/MPL/1.1/>
- <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>
- <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>

### GTK 4, GLib, Pango, gdk-pixbuf — GNU LGPL 2.1 or later

> UI toolkit and core utilities.
> <https://www.gtk.org>, <https://gitlab.gnome.org/GNOME/glib>,
> <https://gitlab.gnome.org/GNOME/pango>,
> <https://gitlab.gnome.org/GNOME/gdk-pixbuf>
>
> Copyright the GNU Project and contributors.

These libraries are licensed under the GNU Lesser General Public
License version 2.1, or (at your option) any later version. The full
license text is available at:

  <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>

Per LGPL section 6, since Nordstjernen links to these libraries
dynamically, you are entitled to modify them and re-link Nordstjernen
against the modified copies. On Windows / macOS bundles the libraries
are shipped alongside the executable as ordinary DLLs / dylibs that you
can replace; on Linux distributions they are loaded from the system
package manager.

### Cairo — LGPL-2.1 or MPL-1.1

> 2D drawing.
> <https://www.cairographics.org>
>
> Copyright Carl Worth, Behdad Esfahbod, and the Cairo contributors.

Dual-licensed under the GNU Lesser General Public License 2.1 or the
Mozilla Public License 1.1. See:

- <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>
- <https://www.mozilla.org/MPL/1.1/>

### librsvg — GNU LGPL 2.1 or later (Windows / macOS bundles)

> SVG renderer used by gdk-pixbuf to decode SVG images on Windows
> and macOS. Not linked on Linux (system gdk-pixbuf provides its own
> SVG loader).
> <https://gitlab.gnome.org/GNOME/librsvg>
>
> Copyright the GNU Project and contributors.

Licensed under the GNU Lesser General Public License version 2.1 or
later. See the LGPL section above for terms and obligations.

---

## NOTICE files

Apache 2.0 section 4(d) requires propagating any `NOTICE` files
shipped with the upstream sources. As of this release:

- lexbor ships no `NOTICE` file.
- Wuffs ships no `NOTICE` file.

If a future upstream release adds one, it will be included verbatim
in this section.
