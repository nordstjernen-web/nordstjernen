project(
  'nordstjernen',
  'c',
  version: '0.0.1',
  license: 'proprietary',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
    'b_ndebug=if-release',
  ],
  meson_version: '>=1.0',
)

cc = meson.get_compiler('c')

common_warning_flags = [
  '-Wshadow',
  '-Wpointer-arith',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wold-style-definition',
  '-Wcast-align',
  '-Wwrite-strings',
  '-Wformat=2',
  '-Wno-unused-parameter',
]

add_project_arguments(
  cc.get_supported_arguments(common_warning_flags),
  language: 'c',
)

gtk4 = dependency('gtk4', version: '>=4.6')
libcurl = dependency('libcurl', version: '>=7.85')

subdir('src')
subdir('tests')
