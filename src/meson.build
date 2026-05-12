nordstjernen_lib_sources = files(
  'bookmarks.c',
  'cache.c',
  'config.c',
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
)

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

executable(
  'nordstjernen',
  nordstjernen_sources,
  dependencies: nordstjernen_deps,
  install: true,
)
