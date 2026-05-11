nordstjernen_sources = files(
  'main.c',
  'net.c',
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
