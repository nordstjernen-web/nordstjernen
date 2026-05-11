nordstjernen_lib_sources = files(
  'bookmarks.c',
  'css.c',
  'dom.c',
  'html.c',
  'image.c',
  'layout.c',
  'net.c',
  'paint.c',
)

nordstjernen_sources = nordstjernen_lib_sources + files(
  'main.c',
)

nordstjernen_deps = [
  gtk4,
  libcurl,
  libm,
]

executable(
  'nordstjernen',
  nordstjernen_sources,
  dependencies: nordstjernen_deps,
  install: true,
)
