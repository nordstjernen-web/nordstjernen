project(
  'gumbo-parser',
  'c',
  version: '0.13.0',
  license: 'Apache-2.0',
  default_options: [
    'warning_level=0',
    'werror=false',
    'c_std=gnu11,c11',
  ],
)

cc = meson.get_compiler('c')

gumbo_src_dir = meson.current_source_dir() / 'src'
gumbo_sources = []
foreach f : run_command(
    'sh', '-c',
    'ls "@0@"/*.c 2>/dev/null'.format(gumbo_src_dir),
    check: false,
  ).stdout().strip().split('\n')
  if f != ''
    gumbo_sources += [f]
  endif
endforeach

if gumbo_sources.length() == 0
  error('gumbo subproject: no .c files found under src/. ' +
        'Check the wrap-git revision actually contains the parser source.')
endif

gumbo_lib = static_library(
  'gumbo',
  gumbo_sources,
  include_directories: include_directories('src'),
  install: false,
)

gumbo_dep = declare_dependency(
  link_with: gumbo_lib,
  include_directories: include_directories('src'),
)
