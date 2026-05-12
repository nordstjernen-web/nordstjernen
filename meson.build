project(
  'nordstjernen',
  'c',
  version: '0.0.1',
  license: 'proprietary',
  default_options: [
    'c_std=gnu23,c23,gnu2x,c2x,gnu11,c11',
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
  '-Wmissing-declarations',
  '-Wold-style-definition',
  '-Wcast-align',
  '-Wwrite-strings',
  '-Wformat=2',
  '-Wnull-dereference',
  '-Wdouble-promotion',
  '-Wlogical-op',
]

add_project_arguments(
  cc.get_supported_arguments(common_warning_flags),
  language: 'c',
)

gtk4 = dependency('gtk4', version: '>=4.6')
libcurl = dependency('libcurl', version: '>=7.85')
libm = cc.find_library('m', required: false)

libquickjs = dependency(
  'quickjs',
  fallback: ['quickjs', 'qjs_dep'],
  default_options: [
    'default_library=static',
    'tests=disabled',
    'examples=disabled',
    'warning_level=0',
    'werror=false',
    'c_std=gnu11,c11',
  ],
)

gumbo_feature = get_option('gumbo')
libgumbo = dependency(
  'gumbo',
  fallback: ['gumbo', 'gumbo_dep'],
  required: gumbo_feature,
)
have_gumbo = libgumbo.found()
if have_gumbo
  add_project_arguments('-DND_HAVE_GUMBO=1', language: 'c')
endif

subdir('src')
