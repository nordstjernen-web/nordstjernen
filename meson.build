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

lexbor_feature = get_option('lexbor')
have_lexbor = false
liblexbor = dependency('', required: false)
if not lexbor_feature.disabled()
  if cc.has_header('lexbor/html/html.h')
    liblexbor_lib = cc.find_library('lexbor_static', required: false)
    if liblexbor_lib.found()
      liblexbor = declare_dependency(dependencies: liblexbor_lib)
      have_lexbor = true
    endif
  endif
  if not have_lexbor
    cmake = import('cmake')
    lexbor_cmake_opts = cmake.subproject_options()
    lexbor_cmake_opts.add_cmake_defines({
      'LEXBOR_BUILD_SHARED': 'OFF',
      'LEXBOR_BUILD_STATIC': 'ON',
      'LEXBOR_BUILD_TESTS': 'OFF',
      'LEXBOR_BUILD_EXAMPLES': 'OFF',
      'LEXBOR_INSTALL_HEADERS': 'OFF',
    })
    lexbor_proj = cmake.subproject('lexbor',
                                   options: lexbor_cmake_opts,
                                   required: lexbor_feature.enabled())
    if lexbor_proj.found()
      liblexbor = lexbor_proj.dependency('lexbor_static')
      have_lexbor = true
    endif
  endif
endif
if have_lexbor
  add_project_arguments('-DND_HAVE_LEXBOR=1', language: 'c')
endif

install_data(
  'data/icons/hicolor/scalable/apps/nordstjernen.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

install_subdir(
  'compatibility-css',
  install_dir: get_option('datadir') / 'nordstjernen',
  exclude_files: ['README.md'],
)

subdir('src')
