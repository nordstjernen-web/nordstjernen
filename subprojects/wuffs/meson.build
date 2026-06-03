project('wuffs', 'c', license: 'Apache-2.0 OR MIT')

cc = meson.get_compiler('c')

wuffs_module_defs = [
  '-DWUFFS_IMPLEMENTATION',
  '-DWUFFS_CONFIG__MODULES',
  '-DWUFFS_CONFIG__MODULE__BASE',
  '-DWUFFS_CONFIG__MODULE__ADLER32',
  '-DWUFFS_CONFIG__MODULE__CRC32',
  '-DWUFFS_CONFIG__MODULE__DEFLATE',
  '-DWUFFS_CONFIG__MODULE__LZW',
  '-DWUFFS_CONFIG__MODULE__ZLIB',
  '-DWUFFS_CONFIG__MODULE__PNG',
  '-DWUFFS_CONFIG__MODULE__GIF',
  '-DWUFFS_CONFIG__MODULE__BMP',
  '-DWUFFS_CONFIG__MODULE__JPEG',
  '-DWUFFS_CONFIG__MODULE__VP8',
  '-DWUFFS_CONFIG__MODULE__WEBP',
]

wuffs_warn_off = cc.get_supported_arguments([
  '-w',
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-unused-but-set-variable',
  '-Wno-unused-parameter',
  '-Wno-shadow',
  '-Wno-pointer-arith',
  '-Wno-format-nonliteral',
  '-Wno-cast-align',
  '-Wno-undef',
  '-Wno-missing-prototypes',
  '-Wno-missing-declarations',
  '-Wno-redundant-decls',
  '-Wno-old-style-definition',
  '-Wno-strict-prototypes',
  '-Wno-write-strings',
  '-Wno-vla',
  '-Wno-sign-compare',
  '-Wno-stringop-overflow',
])

wuffs_lib = static_library(
  'wuffs',
  'wuffs-v0.4.c',
  c_args: wuffs_module_defs + wuffs_warn_off,
)

wuffs_dep = declare_dependency(
  link_with: wuffs_lib,
  include_directories: include_directories('.'),
)

meson.override_dependency('wuffs', wuffs_dep)
