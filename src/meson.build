nordstjernen_lib_sources = files(
  'bookmarks.c',
  'cache.c',
  'compatibility.c',
  'config.c',
  'csp.c',
  'css.c',
  'dom.c',
  'headless.c',
  'html.c',
  'image.c',
  'js.c',
  'layout.c',
  'net.c',
  'paint.c',
  'security.c',
  'video.c',
  'webm.c',
  'window.c',
  'youtube.c',
)

if have_lexbor
  nordstjernen_lib_sources += files('html_lexbor.c')
endif

nordstjernen_sources = nordstjernen_lib_sources + files(
  'main.c',
)

nordstjernen_deps = [
  gtk4,
  libcurl,
  libm,
  libquickjs,
  libgumbo,
]

if have_vpx
  nordstjernen_deps += [libvpx]
endif

if have_lexbor
  nordstjernen_deps += [liblexbor]
endif

nordstjernen_win_subsystem = (
  host_machine.system() == 'windows' ? 'windows' : 'console'
)

nordstjernen_link_args = []
if host_machine.system() == 'windows'
  nordstjernen_link_args += cc.get_supported_link_arguments(
    ['-Wl,--stack,8388608'],
  )
endif

executable(
  'nordstjernen',
  nordstjernen_sources,
  dependencies: nordstjernen_deps,
  win_subsystem: nordstjernen_win_subsystem,
  link_args: nordstjernen_link_args,
  install: true,
)
