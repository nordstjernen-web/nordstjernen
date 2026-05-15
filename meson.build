project(
  'nordstjernen',
  ['c', 'cpp'],
  version: '0.4.0',
  license: 'FSL-1.1-MIT',
  default_options: [
    'c_std=gnu2x,c2x,gnu11,c11',
    'cpp_std=c++17',
    'warning_level=2',
    'buildtype=debugoptimized',
    'b_ndebug=true',
    'b_pie=true',
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

hardening_c_flags = [
  '-fstack-protector-strong',
  '-fstack-clash-protection',
  '-fcf-protection=full',
  '-D_FORTIFY_SOURCE=2',
]

hardening_link_flags = [
  '-Wl,-z,relro',
  '-Wl,-z,now',
  '-Wl,-z,noexecstack',
  '-Wl,-z,separate-code',
]

add_project_arguments(
  cc.get_supported_arguments(common_warning_flags + suppressed_warnings +
                             hardening_c_flags),
  language: 'c',
)

add_project_link_arguments(
  cc.get_supported_link_arguments(hardening_link_flags),
  language: 'c',
)

nd_buildtype = get_option('buildtype')
nd_is_release = (nd_buildtype == 'release' or nd_buildtype == 'minsize')
if nd_is_release
  add_project_arguments(
    cc.get_supported_arguments(['-flto=auto', '-ffunction-sections', '-fdata-sections']),
    language: 'c',
  )
  add_project_link_arguments(
    cc.get_supported_link_arguments(['-flto=auto', '-Wl,--gc-sections', '-Wl,-s']),
    language: 'c',
  )
else
  add_project_arguments(
    cc.get_supported_arguments(['-g3', '-fno-omit-frame-pointer']),
    language: 'c',
  )
endif
message('nordstjernen build profile: ' +
        (nd_is_release ? 'release (LTO + stripped + hardened)'
                       : 'development (debug symbols + frame pointer + hardened)'))

gtk4 = dependency('gtk4', version: '>=4.6')
libcurl = dependency('libcurl', version: '>=7.85')
libm = cc.find_library('m', required: false)

libvpx = dependency('vpx', required: false)
have_vpx = libvpx.found()
if have_vpx
  add_project_arguments('-DND_HAVE_VPX=1', language: 'c')
endif

libopus = dependency('opus', required: false)
libpulse_simple = dependency('libpulse-simple',
                             required: host_machine.system() == 'linux' ? false : false)
have_audio = libopus.found() and libpulse_simple.found()
if have_audio
  add_project_arguments('-DND_HAVE_AUDIO=1', language: 'c')
endif

libuchardet = dependency('uchardet')

librsvg = dependency('librsvg-2.0', version: '>=2.46')

libpoppler = dependency('poppler-glib', required: false)
have_poppler = libpoppler.found()
if have_poppler
  add_project_arguments('-DND_HAVE_POPPLER=1', language: 'c')
endif

libfontconfig = dependency('fontconfig', required: false)
have_fontconfig = libfontconfig.found()
if have_fontconfig
  add_project_arguments('-DND_HAVE_FONTCONFIG=1', language: 'c')
endif
libpangoft2 = dependency('pangoft2', required: false)
have_pangoft2 = libpangoft2.found()
if have_pangoft2
  add_project_arguments('-DND_HAVE_PANGOFT2=1', language: 'c')
endif

libavif = dependency('libavif', required: false)
have_avif = libavif.found()
if have_avif
  add_project_arguments('-DND_HAVE_AVIF=1', language: 'c')
endif

libseccomp = dependency(
  'libseccomp',
  required: host_machine.system() == 'linux',
)
have_seccomp = libseccomp.found()
if have_seccomp
  add_project_arguments('-DND_HAVE_SECCOMP=1', language: 'c')
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

libwuffs = dependency('wuffs', fallback: ['wuffs', 'wuffs_dep'])

liblexbor = dependency('', required: false)
if cc.has_header('lexbor/html/html.h')
  liblexbor_lib = cc.find_library('lexbor_static', required: false)
  if liblexbor_lib.found()
    liblexbor = declare_dependency(dependencies: liblexbor_lib)
  endif
endif
if not liblexbor.found()
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
                                 required: true)
  liblexbor = lexbor_proj.dependency('lexbor_static')
endif

fs = import('fs')
lexbor_dir = ''
foreach line : fs.read('subprojects/lexbor.wrap').split('\n')
  if line.startswith('directory')
    lexbor_dir = line.split('=')[1].strip()
  endif
endforeach
lexbor_version = '0.0.0'
if lexbor_dir != '' and fs.is_file('subprojects/' + lexbor_dir + '/version')
  lexbor_version_raw = fs.read('subprojects/' + lexbor_dir + '/version').strip()
  lexbor_version = lexbor_version_raw.split('=')[1]
endif
add_project_arguments('-DND_LEXBOR_VERSION="' + lexbor_version + '"',
                      language: 'c')

install_data(
  'data/icons/hicolor/scalable/apps/nordstjernen.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

install_subdir(
  'data/compatibility-css',
  install_dir: get_option('datadir') / 'nordstjernen',
  exclude_files: ['README.md'],
  strip_directory: false,
)

subdir('src')
