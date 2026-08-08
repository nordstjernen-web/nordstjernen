python = import('python').find_installation()

qjsc_prog = find_program('qjsc', required: false)
qjs_prog  = find_program('qjs',  required: false)

if not meson.is_cross_build() and qjsc_prog.found() and qjs_prog.found()
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
            'ns_js_polyfills_src'],
)

streaming_h = custom_target(
  'streaming.h',
  input: ['../scripts/embed-text.py', '../data/js/streaming.js'],
  output: 'streaming.h',
  command: [python, '@INPUT0@', '@INPUT1@', '@OUTPUT@',
            'ns_js_streaming_src'],
)

wpt_hook_h = custom_target(
  'wpt_hook.h',
  input: ['../scripts/embed-text.py', '../data/js/wpt-hook.js'],
  output: 'wpt_hook.h',
  command: [python, '@INPUT0@', '@INPUT1@', '@OUTPUT@',
            'ns_wpt_hook_src'],
)

nordstjernen_engine_sources = files(
  'anim.c',
  'bytecode_cache.c',
  'bookmarks.c',
  'cache.c',
  'config.c',
  'csp.c',
  'css.c',
  'css_media.c',
  'datetime.c',
  'debuglog.c',
  'dom.c',
  'engine.c',
  'eventsource.c',
  'forms.c',
  'camera.c',
  'mic.c',
  'font.c',
  'glctx.c',
  'history.c',
  'html.c',
  'i18n.c',
  'image.c',
  'svg.c',
  'image_ico.c',
  'image_webp.c',
  'image_wuffs.c',
  'layout.c',
  'mathml.c',
  'net.c',
  'netutil.c',
  'paint.c',
  'print.c',
  'pdf.c',
  'render.c',
  'safebrowsing.c',
  'security.c',
  'selection.c',
  'spellcheck.c',
  'texture.c',
  'video.c',
  'video_decode.c',
  'webcrypto.c',
  'ws.c',
  'html_lexbor.c',
  'xml.c',
)

if js_engine == 'quickjs'
  nordstjernen_engine_sources += files(
    'ext.c',
    'idb.c',
    'js.c',
    'js_canvas.c',
    'js_intl.c',
    'js_date.c',
    'js_perf.c',
    'js_realm.c',
    'wasm.c',
    'webaudio.c',
    'webgl.c',
  )
else
  nordstjernen_engine_sources += files('js_v8.cc')
endif

ns_engine_override_options = js_engine == 'v8' ? ['cpp_std=c++20'] : []

# GTK-free helpers used by the desktop executable but not the engine UI:
# the headless render driver and the crash watchdog. Kept in src/ as common
# code; the GTK frontend itself lives in src/gtk/.
nordstjernen_app_sources = files(
  'headless.c',
  'watchdog.c',
)

if have_avif
  nordstjernen_engine_sources += files('image_avif.c')
endif

if http_backend == 'nghttp2'
  nordstjernen_engine_sources += files('net_http2.c')
endif

if have_webgpu
  nordstjernen_engine_sources += files('webgpu.c')
endif

nordstjernen_win_resources = []
if host_machine.system() == 'windows'
  windows = import('windows')
  nordstjernen_win_resources = windows.compile_resources(
    'nordstjernen.rc',
    depend_files: files('../data/icons/nordstjernen.ico'),
  )
endif

if is_mobile
  nordstjernen_deps = [
    glib,
    gobject,
    gio,
    gmodule,
    cairo,
    pango,
    pangocairo,
    libcurl,
    libcrypto,
    libm,
    libquickjs,
    libwebp,
    libwebpdemux,
    libwuffs,
    libplmpeg,
    libuchardet,
    liblexbor,
    libwamr,
    sqlite3,
    zlib,
  ]
else
  nordstjernen_deps = [
    glib,
    gobject,
    gio,
    cairo,
    pango,
    pangocairo,
    epoxy,
    gmodule,
    libcurl,
    libcrypto,
    libm,
    libquickjs,
    libwebp,
    libwebpdemux,
    libwuffs,
    libplmpeg,
    libuchardet,
    liblexbor,
    libwamr,
    sqlite3,
    zlib,
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

  if have_enchant
    nordstjernen_deps += [libenchant]
  endif

  if have_libav
    nordstjernen_deps += [libavformat, libavcodec, libavutil, libswscale]
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
nordstjernen_deps += js_engine_deps

if http_backend == 'nghttp2'
  nordstjernen_deps += http_backend_deps
endif

# macOS WebGL: src/glctx.c creates the offscreen GL context through CGL, whose
# symbols live in the OpenGL framework. Unused targets drop it via
# -dead_strip_dylibs. iOS has no OpenGL framework (and WebGL is off there, like
# Android), so it is desktop-macOS only.
if host_machine.system() == 'darwin' and not is_ios
  nordstjernen_deps += [dependency('appleframeworks', modules: ['OpenGL'])]
endif

sdl2_dep = dependency('sdl2', required: get_option('audio'))
ns_lib_c_args = []
ns_sdl_deps = []
if sdl2_dep.found()
  ns_sdl_deps += [sdl2_dep]
  ns_lib_c_args += ['-DNS_HAVE_SDL']
endif

if have_webgpu
  nordstjernen_deps += [webgpu_dep]
endif

nordstjernen_win_subsystem = (
  host_machine.system() == 'windows' ? 'windows' : 'console'
)

nordstjernen_link_args = []
if host_machine.system() == 'windows'
  nordstjernen_link_args += cc.get_supported_link_arguments(
    ['-Wl,--stack,8388608'],
  )
  nordstjernen_link_args += ['-lbcrypt', '-ladvapi32', '-lshell32', '-luser32']
endif

# NetBSD pkgsrc libunistring (pulled in transitively through the libcurl /
# libpsl / libidn2 / gnutls stack) carries an undefined reference to libc's
# mbrtoc32, which the hardened --no-undefined link rejects even though the
# dynamic linker resolves it at runtime. Allow undefined symbols originating
# from shared libraries on NetBSD only; our own objects are still checked.
# Every binary that links that stack needs this — the executables below and
# the audio/video helpers, which pull in libunistring via libcurl.
if host_machine.system() == 'netbsd'
  nordstjernen_link_args += cc.get_supported_link_arguments(
    ['-Wl,--allow-shlib-undefined'],
  )
endif

# The gmodule-2.0 dependency injects -Wl,--export-dynamic, which dumps every
# internal engine symbol (~6000: ns_*, lexbor, quickjs) into the executables'
# .dynsym. Combined with -Wl,-z,now that bloats the dynamic-linker symbol
# resolution done on every process start — and the renderer is forked+exec'd
# afresh, so it pays that cost on each cold start. A version script localises
# the executables' own symbols (it overrides --export-dynamic regardless of
# link order); the only runtime dlsym we need (RAND_bytes for RNG warmup)
# resolves through a dependency library, whose exports this leaves untouched.
# Applies to the executables only; the shared libnordstjernen keeps its API.
nordstjernen_exe_link_args = nordstjernen_link_args + \
  cc.get_supported_link_arguments(
    ['-Wl,--version-script=' + meson.current_source_dir() / 'exe-local.map'])

if host_machine.system() == 'windows'
  executable(
    'nordstjernen-launcher',
    files('win_launcher.c') + nordstjernen_win_resources,
    win_subsystem: 'windows',
    link_args: ['-luser32', '-lshell32'],
  )
endif

nordstjernen_embed_sources = nordstjernen_engine_sources + files(
  'embed_shim.c',
  'libnordstjernen.c',
) + [polyfills_h, streaming_h]

if is_android
  nordstjernen_embed_sources += files(
    'ipc_http.c',
    'renderer_serve.c',
    'rproc_http.c',
  )
endif

if v8_static_monolith
  message('js_engine=v8 over a static monolith: skipping the shared ' +
          'libnordstjernen (V8 monolith objects use local-exec TLS and ' +
          'cannot enter a shared library); building the static engine ' +
          'and executables only.')
else
  libnordstjernen_lib = library(
    'nordstjernen',
    nordstjernen_embed_sources,
    dependencies: nordstjernen_deps + ns_sdl_deps,
    c_args: ns_lib_c_args,
    link_args: nordstjernen_link_args,
    override_options: ns_engine_override_options,
    install: true,
  )
endif

# A static variant of the engine so the out-of-process renderer links it in
# and ships as a single self-contained executable (no libnordstjernen.so
# runtime dependency to bundle in distro packages).
libnordstjernen_static = static_library(
  'nordstjernen-static',
  nordstjernen_embed_sources,
  dependencies: nordstjernen_deps + ns_sdl_deps,
  c_args: ns_lib_c_args,
  override_options: ns_engine_override_options,
  install: false,
)

install_headers('libnordstjernen.h', subdir: 'nordstjernen')

# Out-of-process renderer (step 1 of the process-per-tab direction): a small
# executable that drives libnordstjernen over an IPC control channel + shared
# memory framebuffer (src/rproc.*), plus a proof-of-concept client. The mobile
# builds ship only the embeddable library — no standalone executables.
if not is_mobile
  rproc_deps = []
  if host_machine.system() != 'windows'
    rproc_deps += [cc.find_library('rt', required: false)]
  endif

  executable(
    'nordstjernen-renderer',
    files('renderer_http.c', 'renderer_serve.c', 'ipc_http.c', 'threaddump.c'),
    link_with: libnordstjernen_static,
    include_directories: include_directories('.'),
    dependencies: nordstjernen_deps + ns_sdl_deps + rproc_deps,
    c_args: ns_lib_c_args,
    link_args: nordstjernen_exe_link_args,
    win_subsystem: 'windows',
    install: true,
  )

  sdl2_dep = dependency('sdl2', required: get_option('audio'))
  if sdl2_dep.found()
    libminimp3 = static_library(
      'minimp3',
      files('audio/minimp3_impl.c'),
      include_directories: include_directories('audio'),
      c_args: cc.get_supported_arguments(['-w']),
      install: false,
    )

    audio_deps = [dependency('libcurl'), sdl2_dep, libplmpeg]
    if have_libav
      audio_deps += [libavformat, libavcodec, libavutil, libswresample]
    endif
    if libvorbisfile.found()
      audio_deps += [libvorbisfile]
    endif
    if libopusfile.found()
      audio_deps += [libopusfile]
    endif
    # libm is needed by pl_mpeg / minimp3 on Linux and the BSDs; it is part of
    # the C runtime on macOS and Windows, where find_library('m') is a harmless
    # no-op. librt carries shm_open/shm_unlink (the audio clock's POSIX shared
    # memory) on the BSDs, where they live outside libc; it is a harmless no-op
    # where absent, matching the renderer and video-helper link lines.
    audio_lib_deps = [cc.find_library('m', required: false),
                      cc.find_library('rt', required: false)]

    # On Linux the helper confines itself with the same Landlock + seccomp
    # profile as the renderer once its audio device and network stack are up,
    # so attacker-controlled codec bytes are decoded inside the sandbox.
    audio_sandbox_sources = []
    if host_machine.system() == 'linux'
      audio_sandbox_sources += files('security.c')
      audio_deps += [glib]
      if have_seccomp
        audio_deps += [libseccomp]
      endif
    endif

    executable(
      'nordstjernen-audio',
      files('audio/main.c') + audio_sandbox_sources,
      include_directories: include_directories('audio', '.'),
      link_with: libminimp3,
      dependencies: audio_deps + audio_lib_deps,
      link_args: nordstjernen_link_args,
      win_subsystem: 'windows',
      install: true,
    )
  else
    message('SDL2 not found — audio playback helper (nordstjernen-audio) disabled')
  endif

  if have_libav
    videoproc_deps = [libavformat, libavcodec, libavutil, libswscale,
                      dependency('threads')]
    videoproc_sources = files('videoproc/main.c')
    if host_machine.system() == 'linux'
      videoproc_sources += files('security.c')
      videoproc_deps += [glib]
      if have_seccomp
        videoproc_deps += [libseccomp]
      endif
      videoproc_deps += [cc.find_library('rt', required: false)]
    endif
    executable(
      'nordstjernen-video',
      videoproc_sources,
      include_directories: include_directories('.'),
      dependencies: videoproc_deps + [cc.find_library('m', required: false)],
      link_args: nordstjernen_link_args,
      win_subsystem: 'windows',
      install: true,
    )
  endif
endif

if not is_mobile and have_gtk
  subdir('gtk')
endif

if not is_mobile and not have_gtk
  message('GTK 4 shell disabled — building the engine, out-of-process renderer' +
          ' and embeddable library without GTK. WebGL is unavailable in this' +
          ' configuration (its GL context is GTK-backed).')
endif
