# Third-party software notices

Nordstjernen links to (and in some cases statically includes) the
following open-source libraries. One of them, ns-pango, is a **modified**
LGPL library; its source and our relink offer are described in its entry
below. Their copyright notices and license
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

### WebAssembly Micro Runtime (WAMR) — Apache License 2.0 with LLVM exceptions

> WebAssembly runtime. Vendored in `src/wamr/`.
> <https://github.com/bytecodealliance/wasm-micro-runtime>
>
> Copyright (c) The WebAssembly Micro Runtime contributors.

Licensed under the Apache License, Version 2.0, with LLVM exceptions.
See the lexbor section above for the base Apache 2.0 text; the full
license including the LLVM exceptions is reproduced in
`src/wamr/LICENSE`.

---

### ns-pango — GNU LGPL 2.1 or later (modified Pango, statically linked)

> Text itemization, shaping and line breaking, with a cache of shaped runs
> that Pango itself does not keep.
> <https://github.com/nordstjernen-web/ns-pango>
>
> Copyright the GNU Project and contributors, and Northstar contributors
> for the modifications.

**This is a modified copy of Pango.** It adds a process-wide cache of
shaped glyph strings and of context font metrics, renames every exported
symbol so it can share a process with the system Pango that GTK loads, and
removes the backends and tooling a browser does not use. It remains
licensed under the GNU Lesser General Public License version 2.1 or, at
your option, any later version, and its complete corresponding source --
including every modification -- is published at the URL above. The full
license text is available at:

  <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>

Unlike the other LGPL libraries listed here, ns-pango is linked
**statically** into the Nordstjernen executables on desktop platforms. Per
LGPL section 6(a), you are entitled to modify ns-pango and relink
Nordstjernen against your modified copy: write to the address in
`README.md` and we will supply the Nordstjernen object files, together with
any data and utility programs needed, so that you can produce a modified
executable. Android and iOS builds do not include ns-pango; they link the
system Pango dynamically as before.

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

### OpenSSL (libcrypto) — Apache License 2.0

> Web Cryptography (`crypto.subtle`) primitives and the TLS backend used
> transitively by libcurl.
> <https://www.openssl.org>
>
> Copyright (c) 1998-2026 The OpenSSL Project Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0. See the lexbor section
above for the license text (same license).

### libwebp — BSD 3-Clause License

> WebP image decoding.
> <https://chromium.googlesource.com/webm/libwebp>
>
> Copyright (c) 2010, Google Inc. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met: redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer;
redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution;
neither the name of Google nor the names of its contributors may be used
to endorse or promote products derived from this software without
specific prior written permission. THIS SOFTWARE IS PROVIDED BY THE
COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE.

### libpsl — MIT License

> Public Suffix List handling for cookie / origin policy.
> <https://github.com/rockdaboot/libpsl>
>
> Copyright (c) 2014-2024 Tim Rühsen

Licensed under the MIT License. See the quickjs-ng section above for the
full text (same license).

### SQLite — public domain

> Embedded SQL database used for history, bookmarks, and caches.
> <https://www.sqlite.org>

SQLite is in the public domain. The authors disclaim copyright to the
source code; it may be used for any purpose, commercial or
non-commercial, without restriction.

### libepoxy — MIT License

> OpenGL / OpenGL ES function-pointer management (used by the WebGL
> backend and GTK's GL rendering).
> <https://github.com/anholt/libepoxy>
>
> Copyright (c) 2013-2014 Intel Corporation

Licensed under the MIT License. See the quickjs-ng section above for the
full text (same license).

### zlib — zlib License

> DEFLATE compression used by image decoders and HTTP content decoding.
> <https://zlib.net>
>
> Copyright (c) 1995-2024 Jean-loup Gailly and Mark Adler

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software. Permission is granted to anyone to
use this software for any purpose, including commercial applications, and
to alter it and redistribute it freely, subject to the restrictions in
the zlib license: the origin of this software must not be misrepresented;
altered source versions must be plainly marked as such; and this notice
may not be removed from any source distribution.

### libseccomp — GNU LGPL 2.1 (Linux only)

> Syscall-filter sandbox for the renderer process. Linked only on Linux.
> <https://github.com/seccomp/libseccomp>
>
> Copyright (c) Paul Moore and the libseccomp contributors.

Licensed under the GNU Lesser General Public License version 2.1. See the
LGPL section below for terms and obligations.

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

### FFmpeg — libavformat / libavcodec / libavutil / libswscale / libswresample — GNU LGPL 2.1 or later (optional, inline WebM)

> Container demuxing and audio/video decoding for the inline WebM path
> (VP9/VP8 video, Opus/Vorbis audio). <https://ffmpeg.org>
>
> Copyright the FFmpeg developers.

Only present when Nordstjernen was built with WebM support. The copy
bundled in the macOS / Windows releases is built **LGPL-only** — its
`configure` uses `--disable-gpl --disable-nonfree --disable-version3
--disable-autodetect`, so it contains no GPL components and no external
codec libraries; the enabled VP8/VP9/Opus/Vorbis decoders are FFmpeg's
own LGPL implementations (see `scripts/build-ffmpeg-lgpl.sh`). The full
license text is available at:

  <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>

Per LGPL section 6, the libraries are linked dynamically: on the
macOS / Windows bundles they ship beside the executable as ordinary
dylibs / DLLs you can replace and re-link against; on Linux they are
loaded from the distribution's FFmpeg packages.

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

> Not used by the browser: SVG is rendered in-engine by `src/svg.c`.
> librsvg reaches the Windows and macOS bundles only as a transitive
> dependency of the gdk-pixbuf loader directory and of GTK's symbolic
> icon loading, so its notice is reproduced here for redistribution.
> <https://gitlab.gnome.org/GNOME/librsvg>
>
> Copyright the GNU Project and contributors.

Licensed under the GNU Lesser General Public License version 2.1 or
later. See the LGPL section above for terms and obligations.

### Optional dynamic dependencies

These are linked only when present on the build host (meson
`required: false`). When a build bundles them, their notices apply:

- **libavif** — BSD 2-Clause, © the AOMedia / libavif authors. AVIF
  image decoding.
- **Poppler** (`poppler-glib`) — GNU GPL 2.0 or later, © the Poppler
  developers. PDF rendering. Note: Poppler is GPL; a build that links it
  is subject to the GPL for that combined binary.
- **Fontconfig** — MIT-style license, © Keith Packard and contributors.
- **FreeType** — FreeType License (BSD-style with credit clause) or GNU
  GPL 2.0, at your option, © The FreeType Project.

---


## NOTICE files

Apache 2.0 section 4(d) requires propagating any `NOTICE` files
shipped with the upstream sources. As of this release:

- lexbor ships no `NOTICE` file.
- Wuffs ships no `NOTICE` file.

If a future upstream release adds one, it will be included verbatim
in this section.
