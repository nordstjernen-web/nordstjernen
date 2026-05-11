test_html = executable(
  'test_html',
  files('test_html.c') + nordstjernen_lib_sources,
  dependencies: [gtk4, libcurl],
  include_directories: include_directories('..'),
)

test('html', test_html, protocol: 'tap', args: ['--tap'])
