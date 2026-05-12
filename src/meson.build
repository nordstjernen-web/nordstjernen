nordstjernen_lib_sources = files(
  'bookmarks.c',
  'css.c',
  'dom.c',
  'html.c',
  'html_gumbo.c',
  'image.c',
  'js.c',
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
  libquickjs,
]

if have_gumbo
  nordstjernen_deps += [libgumbo]
endif

executable(
  'nordstjernen',
  nordstjernen_sources,
  dependencies: nordstjernen_deps,
  install: true,
)
