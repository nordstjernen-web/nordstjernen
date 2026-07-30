project(
  'nordstjernen',
  ['c', 'cpp'],
  version: '1.0.22',
  license: 'LicenseRef-NSL-1.0',
  default_options: [
    'c_std=gnu2x,c2x,gnu11,c11',
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=release',
    'optimization=3',
    'b_ndebug=true',
    'b_pie=true',
  ],
  meson_version: '>=1.0',
)

cc = meson.get_compiler('c')

common_warning_flags = [
  '-Wshadow',
  '-Wpointer-arith',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wmissing-declarations',
  '-Wold-style-definition',
  '-Wcast-align',
  '-Wwrite-strings',
  '-Wformat=2',
  '-Wformat-security',
  '-Wnull-dereference',
  '-Wlogical-op',
  '-Wundef',
  '-Wvla',
  '-Winit-self',
  '-Wredundant-decls',
]

suppressed_warnings = [
  '-Wno-constant-logical-operand',
]

fortify_level = '-D_FORTIFY_SOURCE=2'
if cc.has_argument('-D_FORTIFY_SOURCE=3')
  fortify_level = '-D_FORTIFY_SOURCE=3'
endif

hardening_c_flags = [
  '-fstack-protector-strong',
  '-fstack-clash-protection',
  '-fcf-protection=full',
  '-U_FORTIFY_SOURCE',
  fortify_level,
]

hardening_link_flags = [
  '-Wl,-z,relro',
  '-Wl,-z,now',
  '-Wl,-z,noexecstack',
  '-Wl,-z,separate-code',
]

add_project_arguments(
  cc.get_supported_arguments(common_warning_flags + suppressed_warnings +
                             hardening_c_flags),
  language: 'c',
)

add_project_link_arguments(
  cc.get_supported_link_arguments(hardening_link_flags),
  language: 'c',
)

add_project_link_arguments(
  cc.get_supported_link_arguments(hardening_link_flags),
  language: 'cpp',
)

ns_buildtype = get_option('buildtype')
ns_is_release = (ns_buildtype == 'release' or ns_buildtype == 'minsize')
# Every build profile gets -O3 (via default_options optimization=3) plus
# dead-section pruning. Nordstjernen leans on the optimizer to inline the
# lexbor parser walks, QuickJS interpreter dispatch, and our box-layout
# loops; -O2 left ~30% on the table for JS-heavy pages. The -O level is
# left to meson's optimization option so it isn't passed twice (which
# warns); we only add the section-GC flags here. Dev/non-release builds
# keep -g3 + frame pointers so step debugging still works under -O3.
add_project_arguments(
  cc.get_supported_arguments(['-ffunction-sections', '-fdata-sections']),
  language: 'c',
)
add_project_link_arguments(
  cc.get_supported_link_arguments(['-Wl,--gc-sections']),
  language: 'c',
)
if ns_is_release
  add_project_link_arguments(
    cc.get_supported_link_arguments(['-Wl,-s']),
    language: 'c',
  )
else
  add_project_arguments(
    cc.get_supported_arguments(['-g3', '-fno-omit-frame-pointer']),
    language: 'c',
  )
endif
message('nordstjernen build profile: ' +
        (ns_is_release ? 'release (LTO + stripped + hardened)'
                       : 'development (debug symbols + frame pointer + hardened)'))

is_android = host_machine.system() == 'android'
# iOS (set by the ios/ meson cross file) cross-compiles the same GTK-free
# embeddable engine as Android, driven by a UIKit host app instead of GTK 4.
# is_mobile gathers the two: both drop GTK 4, gdk-pixbuf, epoxy/WebGL
# and the desktop-only helper executables, building only libnordstjernen.
is_ios = get_option('ios')
is_mobile = is_android or is_ios

# Android builds only the embeddable engine library, driven by the JNI host app
# in android/. It links the GLib/cairo/pango graphics stack directly instead of
# GTK 4: the engine uses no GTK widgets, and GdkTexture is replaced by the
# ns_texture abstraction (src/texture.c). gtk4 is therefore desktop-only.
# The engine paints with Cairo/Pango and wraps decoded images in the
# ns_texture abstraction (no GdkTexture), so the graphics stack is declared
# directly rather than pulled in transitively through gtk4. The toolkit
# (gtk4/epoxy) stays out of the shared engine deps and is added only to the
# GTK shell, keeping the out-of-process renderer and the embeddable library
# free of GTK. The GTK shell itself is a feature: disabling it yields a
# renderer-only build that needs no GTK 4 at all.
gtk_feature = get_option('gtk')

glib       = dependency('glib-2.0')
gobject    = dependency('gobject-2.0')
gio        = dependency('gio-2.0')
cairo      = dependency('cairo')

# Text is shaped through ns-pango, a Pango fork that caches finished glyph
# strings across layouts; every symbol in it is renamed, because GTK loads the
# system Pango into the same process. Android and iOS link the system Pango
# instead -- the fork carries no CoreText backend and requires fontconfig --
# and src/ns_pango_names.h maps the renamed API back for them.
use_ns_pango = not is_mobile and get_option('ns-pango').allowed()
if use_ns_pango
  ns_pango_debug = ns_is_release ? 'false' : 'true'
  ns_pango_ndebug = ns_is_release ? 'true' : 'false'
  ns_pango_lto = ns_is_release ? 'true' : 'false'
  ns_pango_sp = subproject('ns-pango',
    default_options: [
      'default_library=static',
      'c_std=gnu17',
      'optimization=3',
      'debug=' + ns_pango_debug,
      'b_ndebug=' + ns_pango_ndebug,
      'b_lto=' + ns_pango_lto,
      'fontconfig=enabled',
      'freetype=enabled',
      'introspection=disabled',
      'sysprof=disabled',
      'libthai=auto',
      'build-testsuite=false',
      'nordstjernen=true',
      'warning_level=0',
      'werror=false',
    ])
  pango      = ns_pango_sp.get_variable('libpango_dep')
  pangocairo = ns_pango_sp.get_variable('libpangocairo_dep')
  add_project_arguments('-DNS_USE_NS_PANGO=1', language: 'c')
else
  pango      = dependency('pango')
  pangocairo = dependency('pangocairo')
endif

if not is_mobile
  # libepoxy is the GL/EGL dispatch layer for WebGL. It lives in the shared
  # engine deps (not the GTK shell) because WebGL renders to an offscreen
  # EGL/GLES context in the renderer, with no toolkit involved.
  epoxy      = dependency('epoxy')
  have_egl   = cc.has_header('epoxy/egl.h', dependencies: epoxy)
endif

have_gtk = false
if not is_mobile and gtk_feature.allowed()
  gtk4_min = host_machine.system() == 'windows' ? '>=4.22.1' : '>=4.14'
  gtk4 = dependency('gtk4', version: gtk4_min, required: gtk_feature)
  if gtk4.found()
    have_gtk = true
    gtk4_dep = gtk4
    if host_machine.system() == 'windows'
      message('GTK @0@ — Windows requires the MSYS2 stock GTK (>= 4.22.1).'.format(gtk4.version()))
    elif gtk4.version().version_compare('>=4.22')
      message('GTK @0@ — preferred (>= 4.22).'.format(gtk4.version()))
    else
      warning('GTK @0@ meets the 4.14 minimum, but Nordstjernen prefers >= 4.22 for the newer GSK renderers.'.format(gtk4.version()))
    endif
  endif
endif
gmodule = dependency('gmodule-2.0')
libcurl = dependency('libcurl', version: '>=8.5')
if libcurl.version().version_compare('<8.11')
  warning('libcurl @0@ predates stable WebSocket support (curl 8.11): WebSocket will be unavailable at runtime unless this libcurl was built with the ws/wss protocols.'.format(libcurl.version()))
endif
libcrypto = dependency('libcrypto')
libssl = dependency('libssl')
libm = cc.find_library('m', required: false)

# HTTP transport backend. 'curl' (default) drives libcurl for page fetches;
# 'nghttp2' swaps in the in-tree src/net_http2.c client (libnghttp2 over
# OpenSSL, HTTP/1.1 fallback, zlib/brotli decompression). libcurl stays a hard
# dependency in either case — WebSocket, Server-Sent Events and the audio
# helper use it directly, and the nghttp2 backend falls back to it
# for proxied and FTP hops. So the switch only changes which library performs
# the document/subresource transfer, not what is linked.
http_backend = get_option('http_backend')
http_backend_deps = []
if http_backend == 'nghttp2'
  libnghttp2  = dependency('libnghttp2')
  zlib_http   = dependency('zlib')
  libbrotlidec = dependency('libbrotlidec', required: false)
  libzstd_http = dependency('libzstd', required: false)
  http_backend_deps += [libnghttp2, libssl, libcrypto, zlib_http]
  if host_machine.system() == 'windows'
    http_backend_deps += cc.find_library('ws2_32')
  endif
  add_project_arguments('-DNS_HTTP_BACKEND_NGHTTP2=1', language: 'c')
  if libbrotlidec.found()
    http_backend_deps += [libbrotlidec]
    add_project_arguments('-DNS_HTTP_HAVE_BROTLI=1', language: 'c')
  endif
  if libzstd_http.found()
    http_backend_deps += [libzstd_http]
    add_project_arguments('-DNS_HTTP_HAVE_ZSTD=1', language: 'c')
  endif
  # HTTP/3 over QUIC is an auto-detected sub-feature of the nghttp2 backend:
  # built only when ngtcp2 + its gnutls crypto binding + libnghttp3 + gnutls are
  # all present. libnghttp3 is the HTTP/3 application layer, ngtcp2 the QUIC
  # transport, and gnutls the QUIC-capable TLS stack (system OpenSSL 3.0 has no
  # QUIC API). A machine without these gets an HTTP/2-only nghttp2 backend.
  libngtcp2 = dependency('libngtcp2', required: false)
  libngtcp2_gnutls = dependency('libngtcp2_crypto_gnutls', required: false)
  libnghttp3 = dependency('libnghttp3', required: false)
  libgnutls = dependency('gnutls', required: false)
  if host_machine.system() != 'windows' and libngtcp2.found() \
     and libngtcp2_gnutls.found() and libnghttp3.found() and libgnutls.found()
    http_backend_deps += [libngtcp2, libngtcp2_gnutls, libnghttp3, libgnutls]
    add_project_arguments('-DNS_HTTP_HAVE_HTTP3=1', language: 'c')
    message('HTTP backend: nghttp2 + HTTP/3 (ngtcp2 + nghttp3 + gnutls)')
  else
    if host_machine.system() == 'windows'
      message('HTTP backend: nghttp2 (HTTP/2 over Winsock; HTTP/3 disabled)')
    else
      message('HTTP backend: nghttp2 (HTTP/2 only; ngtcp2/nghttp3/gnutls absent)')
    endif
  endif
  message('HTTP backend: nghttp2 (libnghttp2 + OpenSSL; HTTP/1.1 fallback; ' +
          'libcurl retained for WebSocket/SSE/audio and proxied/FTP hops)')
else
  message('HTTP backend: curl (libcurl)')
endif

libuchardet = dependency('uchardet')

libpsl = dependency('libpsl')

sqlite3 = dependency('sqlite3')

if not is_mobile
  add_project_arguments('-DNS_ENABLE_WEBGL=1', language: 'c')
  # The offscreen GL context (src/glctx.c) has three backends: WGL on Windows,
  # CGL on macOS (which has no EGL — its epoxy dispatches to the OpenGL
  # framework, and its GL 4.1 core profile compiles GLSL-ES shaders), and EGL
  # everywhere else. Pick CGL on darwin, otherwise EGL when its header is
  # present; without any, ns_gl_context_create() stubs out and WebGL is simply
  # unavailable rather than failing the build.
  if host_machine.system() == 'darwin'
    add_project_arguments('-DNS_HAVE_CGL=1', language: 'c')
  elif have_egl
    add_project_arguments('-DNS_HAVE_EGL=1', language: 'c')
  endif
endif

# WebGPU (navigator.gpu) layers src/webgpu.c over the external wgpu-native
# library; the webgpu.h/wgpu.h headers are vendored in-tree (pinned API) while
# the library is located via pkg-config (wgpu_native) or an extracted release
# passed with -Dwgpu_native_root. The feature is 'auto': it is built whenever
# wgpu-native is found and skipped otherwise, so the default build does not
# hard-require the dependency. Even when built, WebGPU stays off at runtime
# until the browser is launched with --enable-webgpu (or NS_WEBGPU_ALLOW=1).
have_webgpu = false
webgpu_dep = declare_dependency()
webgpu_feature = get_option('webgpu')
if not webgpu_feature.disabled() and not is_mobile
  wgpu_lib = dependency('wgpu_native', required: false)
  wgpu_root = get_option('wgpu_native_root')
  if not wgpu_lib.found() and wgpu_root != ''
    wgpu_lib = cc.find_library('wgpu_native', dirs: [wgpu_root / 'lib'],
                               required: false)
  endif
  if wgpu_lib.found()
    webgpu_dep = declare_dependency(
      include_directories: include_directories('third_party/wgpu-native/include'),
      dependencies: [
        wgpu_lib,
        cc.find_library('dl', required: false),
        cc.find_library('m', required: false),
        dependency('threads'),
      ],
    )
    add_project_arguments('-DND_HAVE_WEBGPU=1', language: 'c')
    have_webgpu = true
    message('WebGPU: built (wgpu-native found) — experimental, runtime-gated by --enable-webgpu')
  elif webgpu_feature.enabled()
    error('-Dwebgpu=enabled needs wgpu-native: install its pkg-config file ' +
          '(wgpu_native) or pass -Dwgpu_native_root=/path/to/extracted/release')
  else
    message('WebGPU: not built (wgpu-native not found; pass -Dwgpu_native_root ' +
            'or install its pkg-config to enable)')
  endif
endif

libpoppler = dependency('poppler-glib', required: false)
have_poppler = libpoppler.found() and not is_mobile
if have_poppler
  add_project_arguments('-DNS_HAVE_POPPLER=1', language: 'c')
endif

libfontconfig = dependency('fontconfig', required: false)
have_fontconfig = libfontconfig.found()
if have_fontconfig
  add_project_arguments('-DNS_HAVE_FONTCONFIG=1', language: 'c')
endif
if use_ns_pango
  libpangoft2 = ns_pango_sp.get_variable('libpangoft2_dep')
  have_pangoft2 = true
else
  libpangoft2 = dependency('pangoft2', required: false)
  have_pangoft2 = libpangoft2.found()
endif
if have_pangoft2
  add_project_arguments('-DNS_HAVE_PANGOFT2=1', language: 'c')
endif

libfreetype = dependency('freetype2', required: false)
have_freetype = libfreetype.found()
if have_freetype
  add_project_arguments('-DNS_HAVE_FREETYPE=1', language: 'c')
endif

libwebp = dependency('libwebp')
libwebpdemux = dependency('libwebpdemux')

avif_feature = get_option('avif').disable_auto_if(is_mobile)
libavif = dependency('libavif', required: avif_feature)
have_avif = libavif.found() and not is_mobile
if have_avif
  add_project_arguments('-DNS_HAVE_AVIF=1', language: 'c')
endif

# Inline WebM (VP9/VP8 + Opus/Vorbis) over FFmpeg's libav* (github.com/FFmpeg/
# FFmpeg, not the defunct Libav fork). Required on Linux and Windows, where WebM
# is the dominant format served by YouTube and other sites and the
# external-player fallback is undesirable; auto-detected on macOS; excluded on
# Android, whose dependency sysroot (android/scripts/build-deps.sh) does not
# cross-build FFmpeg, so WebM there stays on the external-player path.
#
# The version floors are the library sonames of FFmpeg 6.0 (2023) — the oldest
# release carrying the new AVChannelLayout API this code uses — so a stock Linux
# build links the system FFmpeg (Ubuntu 24.04 ships 6.1, and every later FFmpeg
# through 8.0 also satisfies these floors) with no source build. The
# macOS/Windows packages bundle a minimal LGPL FFmpeg built by
# scripts/build-ffmpeg-lgpl.sh. Below 6.0 the required build fails on
# Linux/Windows and WebM disables on macOS.
libav_required = host_machine.system() == 'windows' or host_machine.system() == 'linux'
libavformat = dependency('libavformat', version: '>=60', required: libav_required)
libavcodec = dependency('libavcodec', version: '>=60', required: libav_required)
libavutil = dependency('libavutil', version: '>=58', required: libav_required)
libswscale = dependency('libswscale', version: '>=7', required: libav_required)
libswresample = dependency('libswresample', version: '>=4', required: libav_required)
have_libav = libavformat.found() and libavcodec.found() and libavutil.found() \
  and libswscale.found() and libswresample.found() and not is_mobile
if have_libav
  add_project_arguments('-DNS_HAVE_LIBAV=1', language: 'c')
endif

# vorbisfile is the ov_* high-level decode API over libvorbis, used by the audio
# helper (src/audio/main.c) to decode Ogg/Vorbis natively where FFmpeg is absent.
libvorbisfile = dependency('vorbisfile', required: false)
if libvorbisfile.found()
  add_project_arguments('-DNS_HAVE_VORBISFILE=1', language: 'c')
endif
# opusfile is the high-level op_* decode API over libopus + libogg, used by the
# audio helper to decode Ogg/Opus natively where FFmpeg is absent.
libopusfile = dependency('opusfile', required: false)
if libopusfile.found()
  add_project_arguments('-DNS_HAVE_OPUSFILE=1', language: 'c')
endif
# canPlayType/mediaCapabilities may honestly advertise native Ogg audio only
# where the decoder that plays it is built: the desktop (SDL2) nordstjernen-audio
# helper. On mobile audio takes a different path, so these are gated off there.
if not is_mobile and get_option('audio').allowed()
  sdl2_probe = dependency('sdl2', required: false)
  if sdl2_probe.found() and libvorbisfile.found()
    add_project_arguments('-DNS_AUDIO_NATIVE_VORBIS=1', language: 'c')
  endif
  if sdl2_probe.found() and libopusfile.found()
    add_project_arguments('-DNS_AUDIO_NATIVE_OPUS=1', language: 'c')
  endif
endif
libenchant = dependency('enchant-2', required: false)
have_enchant = libenchant.found() and not is_mobile
if have_enchant
  add_project_arguments('-DNS_HAVE_ENCHANT=1', language: 'c')
endif

libseccomp = dependency(
  'libseccomp',
  required: host_machine.system() == 'linux',
)
have_seccomp = libseccomp.found() and not is_mobile
if have_seccomp
  add_project_arguments('-DNS_HAVE_SECCOMP=1', language: 'c')
endif

libwuffs = dependency('wuffs', fallback: ['wuffs', 'wuffs_dep'])
libplmpeg = dependency('plmpeg', fallback: ['plmpeg', 'plmpeg_dep'])
zlib = dependency('zlib')

fs = import('fs')
lexbor_version = '0.0.0'
if fs.is_file('src/lexbor/version')
  lexbor_version_raw = fs.read('src/lexbor/version').strip()
  lexbor_version = lexbor_version_raw.split('=')[1]
endif
add_project_arguments('-DNS_LEXBOR_VERSION="' + lexbor_version + '"',
                      language: 'c')

build_date = get_option('build_date')
if build_date == ''
  date_cmd = run_command('date', '-u', '+%Y-%m-%d', check: false)
  if date_cmd.returncode() == 0
    build_date = date_cmd.stdout().strip()
  endif
endif
add_project_arguments('-DNS_BUILD_DATE="' + build_date + '"', language: 'c')
add_project_arguments('-DNS_VERSION="' + meson.project_version() + '"', language: 'c')

subdir('src/lexbor')

js_engine = get_option('js_engine')
js_engine_deps = []
v8_static_monolith = false
if js_engine == 'v8'
  v8_cpp = meson.get_compiler('cpp')
  v8_dep = dependency('v8', required: false)
  if not v8_dep.found()
    v8_root = get_option('v8_root')
    if v8_root == ''
      error('js_engine=v8 requires V8: provide a pkg-config "v8", or pass ' +
            '-Dv8_root=/path to an extracted V8 monolith build with ' +
            'include/v8.h and libv8_monolith.a (e.g. a ' +
            'https://github.com/just-js/v8 release).')
    endif
    v8_inc_args = ['-isystem', v8_root / 'include']
    if not v8_cpp.has_header('v8.h', args: v8_inc_args)
      error('js_engine=v8: v8.h not found under ' + v8_root / 'include')
    endif
    v8_lib = v8_cpp.find_library('v8_monolith', static: true,
      dirs: [v8_root / 'lib', v8_root / 'obj', v8_root])
    v8_dep = declare_dependency(compile_args: v8_inc_args,
                                dependencies: [v8_lib])
    v8_static_monolith = true
  endif
  js_engine_deps = [v8_dep, dependency('threads'),
                    v8_cpp.find_library('dl', required: false)]
  libquickjs = declare_dependency(
    include_directories: include_directories('src/quickjs'))
  if have_webgpu
    have_webgpu = false
    message('js_engine=v8: WebGPU is QuickJS-bound — disabled for this build')
  endif
  message('JS engine: v8 (experimental js_v8.cc backend; live core DOM ' +
          'bindings, no fetch/XHR/modules yet, ' +
          'WebGL/WebGPU/Wasm-JS/IndexedDB/extensions stubbed)')
else
  subdir('src/quickjs')
  message('JS engine: quickjs (in-tree, full DOM bindings)')
endif

wasm_unsupported_cpu = host_machine.cpu_family() == 'x86'
if get_option('wasm').enabled() and wasm_unsupported_cpu
  error('wasm (WAMR) is not supported on 32-bit x86; build with -Dwasm=disabled')
endif
wasm_feature = get_option('wasm').disable_auto_if(wasm_unsupported_cpu)
have_wasm = false
if wasm_feature.allowed()
  subdir('src/wamr')
  have_wasm = true
else
  libwamr = declare_dependency()
endif

install_data(
  'data/icons/hicolor/scalable/apps/nordstjernen.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen.gif',
  'data/icons/hicolor/scalable/apps/nordstjernen-back.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-forward.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-home.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-reload.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stop.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-go.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-bookmarks.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-console.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-settings.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-about.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-connect.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-fetch.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-parse.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-script.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-render.svg',
  'data/icons/hicolor/scalable/apps/nordstjernen-stage-done.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

install_data(
  'License.md',
  'THIRD-PARTY-LICENSES.md',
  install_dir: get_option('datadir') / 'nordstjernen',
)

install_data(
  'data/safebrowsing.list',
  install_dir: get_option('datadir') / 'nordstjernen',
)

install_data(
  'data/i18n/am.lang',
  'data/i18n/ar.lang',
  'data/i18n/bn.lang',
  'data/i18n/de.lang',
  'data/i18n/el.lang',
  'data/i18n/en.lang',
  'data/i18n/es.lang',
  'data/i18n/fa.lang',
  'data/i18n/fr.lang',
  'data/i18n/gu.lang',
  'data/i18n/ha.lang',
  'data/i18n/hi.lang',
  'data/i18n/hu.lang',
  'data/i18n/id.lang',
  'data/i18n/it.lang',
  'data/i18n/ja.lang',
  'data/i18n/jv.lang',
  'data/i18n/kn.lang',
  'data/i18n/ko.lang',
  'data/i18n/ml.lang',
  'data/i18n/mr.lang',
  'data/i18n/my.lang',
  'data/i18n/nl.lang',
  'data/i18n/pa.lang',
  'data/i18n/pl.lang',
  'data/i18n/pt.lang',
  'data/i18n/ro.lang',
  'data/i18n/ru.lang',
  'data/i18n/su.lang',
  'data/i18n/sw.lang',
  'data/i18n/ta.lang',
  'data/i18n/te.lang',
  'data/i18n/th.lang',
  'data/i18n/tl.lang',
  'data/i18n/tr.lang',
  'data/i18n/uk.lang',
  'data/i18n/ur.lang',
  'data/i18n/vi.lang',
  'data/i18n/yo.lang',
  'data/i18n/zh.lang',
  install_dir: get_option('datadir') / 'nordstjernen' / 'i18n',
)

install_data(
  'data/nordstjernen.desktop',
  install_dir: get_option('datadir') / 'applications',
  rename: 'org.nordstjernen.WebBrowser.desktop',
)

install_data(
  'data/org.nordstjernen.WebBrowser.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)

summary({'WebAssembly (WAMR)': have_wasm}, section: 'Features')
summary({'AVIF images (libavif)': have_avif}, section: 'Features')
summary({'GTK 4 shell': have_gtk}, section: 'Shells')

subdir('src')
