source_test = executable('test-iris-source', 'test-source.c',
  dependencies: [spa, cc.find_library('dl', required: false)],
)
test('factory', source_test, args: [plugin.full_path()], depends: plugin)
mock_cti = get_option('test-mock-cti')
if mock_cti != ''
  foreach case : [
    {'name': 'automatic-buffer-count', 'min': '1', 'expected': '8', 'args': []},
    {'name': 'automatic-buffer-count-producer-minimum', 'min': '10', 'expected': '10', 'args': []},
    {'name': 'explicit-buffer-count', 'min': '2', 'expected': '4', 'args': ['4']},
  ]
    test(case['name'], source_test,
      args: [plugin.full_path(), mock_cti] + case['args'],
      depends: plugin,
      env: environment({
        'IRIS_FAULT': 'progressive-pool',
        'IRIS_MOCK_MIN_BUFFERS': case['min'],
        'IRIS_MOCK_EXPECT_BUFFERS': case['expected'],
      }),
    )
  endforeach
endif

host_bindir = get_option('test-pipewireao-bindir')
if host_bindir != ''
  if mock_cti == ''
    error('Set test-mock-cti to enable the PipeWireAO daemon test')
  endif
  pipewire = dependency('libpipewire-ao-0.3', include_type: 'system')
  host_client = executable('test-iris-host-client', 'test-host-client.c',
    dependencies: [spa, pipewire],
  )
  python = import('python').find_installation()
  test('pipewireao-mock-graph', python,
    args: [
      files('test-host.py'), host_bindir, host_client.full_path(),
      plugin.full_path(), spa.get_variable(pkgconfig: 'plugindir'),
      pipewire.get_variable(pkgconfig: 'moduledir'), mock_cti,
    ],
    depends: [plugin, host_client],
    env: environment({'IRIS_FAULT': 'progressive-pool'}),
    timeout: 30,
  )
endif
