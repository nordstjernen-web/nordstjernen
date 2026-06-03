python = import('python').find_installation()

qjsc_prog = find_program('qjsc', required: false)
qjs_prog  = find_program('qjs',  required: false)

if qjsc_prog.found() and qjs_prog.found()
  # files() so the script path is absolute — a raw '../scripts/...' string would
  # be resolved relative to the build dir and break out-of-tree builds.
  verify_polyfills_py = files('../scripts/verify-polyfills.py')
  polyfills_verify = custom_target(
    'polyfills-verify',
    input: ['../data/js/polyfills.js', '../scripts/verify-polyfills.js'],
    output: 'polyfills.verified',
    command: [
      python, verify_polyfills_py,
      qjsc_prog, qjs_prog, '@INPUT0@', '@INPUT1@', '@OUTPUT@',
    ],
    build_by_default: true,
  )
endif

polyfills_h = custom_target(
  'polyfills.h',
  input: ['../scripts/embed-text.py', '../data/js/polyfills.js'],
  output: 'polyfills.h',
  command: [python, '@INPUT0@', '@INPUT1@', '@OUTPUT@',
            'nd_js_polyfills_src'],
)

nordstjernen_engine_sources = files(
  'anim.c',
  'bcache.c',
  'bookmarks.c',
  'cache.c',
  'config.c',
  'csp.c',
  'css.c',
  'datetime.c',
  'debuglog.c',
  'dom.c',
  'engine.c',
  'env.c',
  'forms.c',
  'font.c',
  'html.c',
  'image.c',
  'image_ico.c',
  'image_wuffs.c',
  'idb.c',
  'js.c',
  'layout.c',
  'mobile.c',
  'net.c',
  'paint.c',
  'pdf.c',
  'profiler.c',
  'render.c',
  'security.c',
  'selection.c',
  'sha3.c',
  'texture.c',
  'video.c',
  'ws.c',
  'html_lexbor.c',
)

nordstjernen_ui_sources = files(
  'console.c',
  'ctxmenu.c',
  'dialogs.c',
  'export.c',
  'find.c',
  'headless.c',
  'media.c',
  'window.c',
)

nordstjernen_lib_sources = nordstjernen_engine_sources + nordstjernen_ui_sources

if have_avif
  nordstjernen_engine_sources += files('image_avif.c')
  nordstjernen_lib_sources += files('image_avif.c')
endif

nordstjernen_sources = nordstjernen_lib_sources + files(
  'main.c',
) + [polyfills_h]

if host_machine.system() == 'windows'
  windows = import('windows')
  nordstjernen_sources += windows.compile_resources(
    'nordstjernen.rc',
    depend_files: files('../data/icons/nordstjernen.ico'),
  )
endif

if is_android
  nordstjernen_deps = [
    glib,
    gobject,
    gio,
    gmodule,
    cairo,
    pango,
    pangocairo,
    libcurl,
    libm,
    libquickjs,
    libwuffs,
    libuchardet,
    liblexbor,
    sqlite3,
  ]
else
  nordstjernen_deps = [
    gtk4,
    gmodule,
    libcurl,
    libm,
    libquickjs,
    libwuffs,
    libuchardet,
    liblexbor,
    librsvg,
    sqlite3,
  ]

  if have_seccomp
    nordstjernen_deps += [libseccomp]
  endif

  if have_poppler
    nordstjernen_deps += [libpoppler]
  endif

  if have_avif
    nordstjernen_deps += [libavif]
  endif
endif

if have_fontconfig
  nordstjernen_deps += [libfontconfig]
endif

if have_pangoft2
  nordstjernen_deps += [libpangoft2]
endif

if have_freetype
  nordstjernen_deps += [libfreetype]
endif

nordstjernen_deps += [libpsl]

nordstjernen_win_subsystem = (
  host_machine.system() == 'windows' ? 'windows' : 'console'
)

nordstjernen_link_args = []
if host_machine.system() == 'windows'
  nordstjernen_link_args += cc.get_supported_link_arguments(
    ['-Wl,--stack,8388608'],
  )
  nordstjernen_link_args += ['-lbcrypt', '-ladvapi32', '-lshell32']
endif

if not is_android
  executable(
    'nordstjernen',
    nordstjernen_sources,
    dependencies: nordstjernen_deps,
    win_subsystem: nordstjernen_win_subsystem,
    link_args: nordstjernen_link_args,
    install: true,
  )
endif

nordstjernen_embed_sources = nordstjernen_engine_sources + files(
  'embed_shim.c',
  'libnordstjernen.c',
) + [polyfills_h]

library(
  'nordstjernen',
  nordstjernen_embed_sources,
  dependencies: nordstjernen_deps,
  link_args: nordstjernen_link_args,
  install: true,
)

install_headers('libnordstjernen.h', subdir: 'nordstjernen')
