project(
  'nordstjernen',
  ['c', 'cpp'],
  version: '0.8.2-dev',
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

nd_buildtype = get_option('buildtype')
nd_is_release = (nd_buildtype == 'release' or nd_buildtype == 'minsize')
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
if nd_is_release
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
        (nd_is_release ? 'release (LTO + stripped + hardened)'
                       : 'development (debug symbols + frame pointer + hardened)'))

is_android = host_machine.system() == 'android'

# Android builds only the embeddable engine library, driven by the JNI host app
# in android/. It links the GLib/cairo/pango graphics stack directly instead of
# GTK 4: the engine uses no GTK widgets, and GdkTexture is replaced by the
# nd_texture abstraction (src/texture.c). gtk4 is therefore desktop-only.
if is_android
  glib       = dependency('glib-2.0')
  gobject    = dependency('gobject-2.0')
  gio        = dependency('gio-2.0')
  cairo      = dependency('cairo')
  pango      = dependency('pango')
  pangocairo = dependency('pangocairo')
elif host_machine.system() == 'windows'
  gtk4 = dependency('gtk4', version: '>=4.22.4')
  message('GTK @0@ — Windows requires the MSYS2 stock GTK (>= 4.22.4).'.format(gtk4.version()))
else
  gtk4 = dependency('gtk4', version: '>=4.14')
  if gtk4.version().version_compare('>=4.22')
    message('GTK @0@ — preferred (>= 4.22).'.format(gtk4.version()))
  else
    warning('GTK @0@ meets the 4.14 minimum, but Nordstjernen prefers >= 4.22 for the newer GSK renderers.'.format(gtk4.version()))
  endif
endif
gmodule = dependency('gmodule-2.0')
libcurl = dependency('libcurl', version: '>=7.85')
libm = cc.find_library('m', required: false)

libuchardet = dependency('uchardet')

libpsl = dependency('libpsl')

sqlite3 = dependency('sqlite3')

# gdk-pixbuf (image fallback decoder) and librsvg (SVG) are desktop-only; on
# Android, Wuffs handles PNG/JPEG/GIF/BMP and other formats degrade gracefully.
librsvg = dependency('librsvg-2.0', version: '>=2.46', required: not is_android)
have_librsvg = librsvg.found() and not is_android
if have_librsvg
  add_project_arguments('-DND_HAVE_LIBRSVG=1', language: 'c')
endif

have_gdk_pixbuf = not is_android
if have_gdk_pixbuf
  add_project_arguments('-DND_HAVE_GDK_PIXBUF=1', language: 'c')
endif

libpoppler = dependency('poppler-glib', required: false)
have_poppler = libpoppler.found() and not is_android
if have_poppler
  add_project_arguments('-DND_HAVE_POPPLER=1', language: 'c')
endif

libfontconfig = dependency('fontconfig', required: false)
have_fontconfig = libfontconfig.found()
if have_fontconfig
  add_project_arguments('-DND_HAVE_FONTCONFIG=1', language: 'c')
endif
libpangoft2 = dependency('pangoft2', required: false)
have_pangoft2 = libpangoft2.found()
if have_pangoft2
  add_project_arguments('-DND_HAVE_PANGOFT2=1', language: 'c')
endif

libfreetype = dependency('freetype2', required: false)
have_freetype = libfreetype.found()
if have_freetype
  add_project_arguments('-DND_HAVE_FREETYPE=1', language: 'c')
endif

libavif = dependency('libavif', required: false)
have_avif = libavif.found() and not is_android
if have_avif
  add_project_arguments('-DND_HAVE_AVIF=1', language: 'c')
endif

libseccomp = dependency(
  'libseccomp',
  required: host_machine.system() == 'linux',
)
have_seccomp = libseccomp.found() and not is_android
if have_seccomp
  add_project_arguments('-DND_HAVE_SECCOMP=1', language: 'c')
endif

libwuffs = dependency('wuffs', fallback: ['wuffs', 'wuffs_dep'])

fs = import('fs')
lexbor_version = '0.0.0'
if fs.is_file('src/lexbor/version')
  lexbor_version_raw = fs.read('src/lexbor/version').strip()
  lexbor_version = lexbor_version_raw.split('=')[1]
endif
add_project_arguments('-DND_LEXBOR_VERSION="' + lexbor_version + '"',
                      language: 'c')

build_date = get_option('build_date')
if build_date == ''
  date_cmd = run_command('date', '-u', '+%Y-%m-%d', check: false)
  if date_cmd.returncode() == 0
    build_date = date_cmd.stdout().strip()
  endif
endif
add_project_arguments('-DND_BUILD_DATE="' + build_date + '"', language: 'c')

subdir('src/lexbor')
subdir('src/quickjs')

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
  install_dir: get_option('datadir') / 'nordstjernen',
)

install_data(
  'data/nordstjernen.desktop',
  install_dir: get_option('datadir') / 'applications',
  rename: 'com.nordstjernen.Browser.desktop',
)

subdir('src')
