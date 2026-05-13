project('ada', 'cpp',
        version: '2.9.2',
        default_options: [
          'cpp_std=c++17,c++20',
          'warning_level=0',
          'werror=false',
          'b_ndebug=true',
        ])

cxx = meson.get_compiler('cpp')
deps = []
threads_dep = dependency('threads', required: false)
if threads_dep.found()
  deps += [threads_dep]
endif

ada_lib = static_library(
  'ada',
  'ada.cpp',
  include_directories: include_directories('.'),
  dependencies: deps,
  cpp_args: cxx.get_supported_arguments(['-w']),
  install: false,
)

ada_dep = declare_dependency(
  link_with: ada_lib,
  include_directories: include_directories('.'),
)

meson.override_dependency('ada', ada_dep)
