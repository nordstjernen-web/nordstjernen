nordstjernen_lib_sources = files(
  'css.c',
  'dom.c',
  'html.c',
  'layout.c',
  'net.c',
)

nordstjernen_sources = nordstjernen_lib_sources + files(
  'main.c',
)

nordstjernen_deps = [
  gtk4,
  libcurl,
]

executable(
  'nordstjernen',
  nordstjernen_sources,
  dependencies: nordstjernen_deps,
  install: true,
)
