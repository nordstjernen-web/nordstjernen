# QuickJS — integrated as part of Nordstjernen (no longer a meson
# subproject). Public surface: `libquickjs` (declared dependency).
# Also builds in-tree `qjs` / `qjsc` host tools used by the polyfill
# verifier at build time.

qjs_host_system = host_machine.system()

qjs_warning_args = ['-w']
qjs_extra_args = ['-D_GNU_SOURCE']
if qjs_host_system != 'sunos'
  qjs_extra_args += '-funsigned-char'
endif
if qjs_host_system == 'windows'
  qjs_extra_args += ['-DWIN32_LEAN_AND_MEAN', '-D_WIN32_WINNT=0x0601']
endif

qjs_c_args = cc.get_supported_arguments(qjs_warning_args) + qjs_extra_args

qjs_sys_deps = [
  cc.find_library('m', required: false),
  dependency('threads', required: false),
  dependency('dl', required: false),
]

qjs_srcs = files(
  'dtoa.c',
  'libregexp.c',
  'libunicode.c',
  'quickjs.c',
)
qjs_libc_srcs = files('quickjs-libc.c')

qjs_lib = static_library(
  'qjs',
  qjs_srcs,
  c_args: qjs_c_args,
  dependencies: qjs_sys_deps,
  gnu_symbol_visibility: 'hidden',
)

qjs_libc_lib = static_library(
  'quickjs-libc',
  qjs_libc_srcs,
  c_args: qjs_c_args,
  dependencies: qjs_sys_deps,
  include_directories: include_directories('.'),
  gnu_symbol_visibility: 'hidden',
)

libquickjs = declare_dependency(
  link_with: qjs_lib,
  dependencies: qjs_sys_deps,
  include_directories: include_directories('.'),
)

qjs_libc_dep = declare_dependency(
  link_with: qjs_libc_lib,
  include_directories: include_directories('.'),
  dependencies: libquickjs,
)

qjsc_exe = executable(
  'qjsc',
  files('qjsc.c'),
  c_args: qjs_c_args,
  dependencies: [libquickjs, qjs_libc_dep],
  install: false,
  build_by_default: true,
)

qjs_exe = executable(
  'qjs',
  files('gen/repl.c', 'gen/standalone.c', 'qjs.c'),
  c_args: qjs_c_args,
  dependencies: [libquickjs, qjs_libc_dep],
  export_dynamic: true,
  install: false,
  build_by_default: true,
)

meson.override_find_program('qjsc', qjsc_exe)
meson.override_find_program('qjs', qjs_exe)
