plugin = shared_library('spa-iris',
  'camera.c',
  'controls.c',
  'source.c',
  dependencies: [spa, iris, dependency('threads')],
  gnu_symbol_visibility: 'hidden',
  install: true,
  install_dir: spa.get_variable(pkgconfig: 'plugindir') / 'iris',
)
