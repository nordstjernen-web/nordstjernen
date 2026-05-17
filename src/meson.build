python = import('python').find_installation()

jquery_shim_h = custom_target(
  'jquery_shim.h',
  input: ['../scripts/embed-text.py', '../data/js/jquery-shim.js'],
  output: 'jquery_shim.h',
  command: [python, '@INPUT0@', '@INPUT1@', '@OUTPUT@',
            'nd_js_jquery_shim_src'],
)

polyfills_h = custom_target(
  'polyfills.h',
  input: ['../scripts/embed-text.py', '../data/js/polyfills.js'],
  output: 'polyfills.h',
  command: [python, '@INPUT0@', '@INPUT1@', '@OUTPUT@',
            'nd_js_polyfills_src'],
)

nordstjernen_lib_sources = files(
  'anim.c',
  'audio.c',
  'bookmarks.c',
  'cache.c',
  'compatibility.c',
  'config.c',
  'csp.c',
  'css.c',
  'dom.c',
  'env.c',
  'font.c',
  'headless.c',
  'html.c',
  'image.c',
  'image_wuffs.c',
  'js.c',
  'layout.c',
  'net.c',
  'paint.c',
  'pdf.c',
  'security.c',
  'selection.c',
  'video.c',
  'webm.c',
  'ws.c',
  'window.c',
  'html_lexbor.c',
  'css_lexbor.c',
)

if have_avif
  nordstjernen_lib_sources += files('image_avif.c')
endif

nordstjernen_sources = nordstjernen_lib_sources + files(
  'main.c',
) + [jquery_shim_h, polyfills_h]

nordstjernen_deps = [
  gtk4,
  libcurl,
  libm,
  libquickjs,
  libwuffs,
  libuchardet,
  liblexbor,
  librsvg,
]

if have_vpx
  nordstjernen_deps += [libvpx]
endif

if have_audio
  nordstjernen_deps += [libopus]
endif
if have_audio_pulse
  nordstjernen_deps += [libpulse_simple]
endif
if have_audio_winmm
  nordstjernen_deps += [libwinmm]
endif

if have_seccomp
  nordstjernen_deps += [libseccomp]
endif

if have_fontconfig
  nordstjernen_deps += [libfontconfig]
endif

if have_poppler
  nordstjernen_deps += [libpoppler]
endif

if have_pangoft2
  nordstjernen_deps += [libpangoft2]
endif

if have_avif
  nordstjernen_deps += [libavif]
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
  nordstjernen_link_args += ['-lbcrypt', '-ladvapi32']
endif

executable(
  'nordstjernen',
  nordstjernen_sources,
  dependencies: nordstjernen_deps,
  win_subsystem: nordstjernen_win_subsystem,
  link_args: nordstjernen_link_args,
  install: true,
)
