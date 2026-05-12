project(
  'nordstjernen',
  'c',
  version: '0.0.1',
  license: 'proprietary',
  default_options: [
    'c_std=gnu2x,c2x,gnu11,c11',
    'warning_level=2',
    'buildtype=debugoptimized',
    'b_ndebug=true',
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
  '-Wlogical-op',
  '-Wundef',
  '-Wvla',
  '-Winit-self',
  '-Wredundant-decls',
]

suppressed_warnings = [
  '-Wno-constant-logical-operand',
]

add_project_arguments(
  cc.get_supported_arguments(common_warning_flags + suppressed_warnings),
  language: 'c',
)

gtk4 = dependency('gtk4', version: '>=4.6')
libcurl = dependency('libcurl', version: '>=7.85')
libm = cc.find_library('m', required: false)

libvpx = dependency('vpx', required: false)
have_vpx = libvpx.found()
if have_vpx
  add_project_arguments('-DND_HAVE_VPX=1', language: 'c')
endif

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
    'b_ndebug=true',
  ],
)

libgumbo = dependency(
  'gumbo',
  fallback: ['gumbo', 'gumbo_dep'],
)

subdir('src')
