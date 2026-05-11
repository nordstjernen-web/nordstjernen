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
fs = import('fs')

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
libm = cc.find_library('m', required: false)

quickjs_version = fs.read('third_party/quickjs/VERSION').strip()
quickjs_inc = include_directories('third_party/quickjs')

quickjs_sources = files(
  'third_party/quickjs/quickjs.c',
  'third_party/quickjs/libregexp.c',
  'third_party/quickjs/libunicode.c',
  'third_party/quickjs/cutils.c',
  'third_party/quickjs/dtoa.c',
)

quickjs_args = [
  '-D_GNU_SOURCE',
  '-DCONFIG_VERSION="' + quickjs_version + '"',
  '-w',
]

add_project_arguments('-DHAVE_QUICKJS=1', language: 'c')

libquickjs_static = static_library(
  'quickjs',
  quickjs_sources,
  include_directories: quickjs_inc,
  c_args: quickjs_args,
  override_options: ['werror=false', 'warning_level=0', 'c_std=gnu11'],
)

libquickjs = declare_dependency(
  link_with: libquickjs_static,
  include_directories: quickjs_inc,
  dependencies: cc.find_library('pthread', required: false),
)

subdir('src')
