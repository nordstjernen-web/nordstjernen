nordstjernen_lib_sources = files(
  'bookmarks.c',
  'cache.c',
  'config.c',
  'css.c',
  'dom.c',
  'headless.c',
  'html.c',
  'html_gumbo.c',
  'image.c',
  'js.c',
  'layout.c',
  'net.c',
  'paint.c',
  'video.c',
  'webm.c',
)

nordstjernen_sources = nordstjernen_lib_sources + files(
  'main.c',
)

nordstjernen_deps = [
  gtk4,
  libcurl,
  libm,
  libquickjs,
]

if have_gumbo
  nordstjernen_deps += [libgumbo]
endif

if have_vpx
  nordstjernen_deps += [libvpx]
endif

executable(
  'nordstjernen',
  nordstjernen_sources,
  dependencies: nordstjernen_deps,
  install: true,
)
