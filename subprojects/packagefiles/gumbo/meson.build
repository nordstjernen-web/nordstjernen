project('gumbo', 'c',
        version: '0.13.0',
        default_options: [
          'c_std=gnu11,c11',
          'warning_level=0',
          'werror=false',
          'b_ndebug=true',
        ])

cc = meson.get_compiler('c')

src_dir = 'src'
gumbo_sources = files(
  src_dir / 'attribute.c',
  src_dir / 'char_ref.c',
  src_dir / 'error.c',
  src_dir / 'parser.c',
  src_dir / 'string_buffer.c',
  src_dir / 'string_piece.c',
  src_dir / 'tag.c',
  src_dir / 'tokenizer.c',
  src_dir / 'utf8.c',
  src_dir / 'util.c',
  src_dir / 'vector.c',
)

gumbo_inc = include_directories(src_dir)

gumbo_lib = static_library(
  'gumbo',
  gumbo_sources,
  include_directories: gumbo_inc,
  c_args: cc.get_supported_arguments(['-w', '-fvisibility=hidden']),
  install: false,
)

gumbo_dep = declare_dependency(
  link_with: gumbo_lib,
  include_directories: gumbo_inc,
)

meson.override_dependency('gumbo', gumbo_dep)
