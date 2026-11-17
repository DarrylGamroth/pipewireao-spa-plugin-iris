source_test = executable('test-iris-source', 'test-source.c',
  dependencies: [spa, dependency('threads'), cc.find_library('dl', required: false)],
)
test('factory', source_test, args: [plugin.full_path()], depends: plugin)
mock_cti = get_option('test-mock-cti')
if mock_cti != ''
  foreach case : [
    {'name': 'automatic-buffer-count', 'min': '1', 'expected': '8', 'args': []},
    {'name': 'automatic-buffer-count-producer-minimum', 'min': '10', 'expected': '10', 'args': []},
    {'name': 'explicit-buffer-count', 'min': '2', 'expected': '4', 'args': ['4']},
    {'name': 'buffer-count-below-minimum', 'min': '2', 'expected': '4', 'args': ['4', '1']},
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

control_cti = get_option('test-control-cti')
mock_serial = get_option('test-clserial')
clprotocol = get_option('test-clprotocol')
control_config = get_option('test-clprotocol-gentl-config')
control_fixtures = []
control_env = {'IRIS_FAULT': 'progressive-pool'}
if control_cti != '' or mock_serial != '' or clprotocol != '' or control_config != ''
  if mock_cti == '' or control_cti == '' or mock_serial == '' or (clprotocol == '') == (control_config == '')
    error('Set test-mock-cti, test-control-cti, test-clserial, and either test-clprotocol or test-clprotocol-gentl-config')
  endif
  if control_config == ''
    control_fixtures += [{'name': 'legacy-driver', 'args': []}]
    control_env += {'CRED2_CL_SERIAL': mock_serial, 'CRED2_CL_PROTOCOL': clprotocol}
  else
    control_env += {'CLPROTOCOL_GENTL_SERIAL': mock_serial, 'CLPROTOCOL_GENTL_CONFIG': control_config}
    control_fixtures += [{'name': 'serial-token', 'args': []}, {'name': 'no-serial-token', 'args': ['no-token']}]
  endif
  foreach fixture : control_fixtures
    test('paired-serial-control-' + fixture['name'], source_test,
      args: [plugin.full_path(), mock_cti, '--control', control_cti] + fixture['args'],
      depends: plugin,
      env: environment(control_env),
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
  host_args = [
    files('test-host.py'), host_bindir, host_client.full_path(),
    plugin.full_path(), spa.get_variable(pkgconfig: 'plugindir'),
    pipewire.get_variable(pkgconfig: 'moduledir'), mock_cti,
  ]
  test('pipewireao-mock-graph', python,
    args: host_args,
    depends: [plugin, host_client],
    env: environment({'IRIS_FAULT': 'progressive-pool'}),
    timeout: 30,
  )
  if control_cti != ''
    paired_args = control_config == '' ? [] : ['both']
    test('pipewireao-paired-mock-graph', python,
      args: host_args + [control_cti] + paired_args,
      depends: [plugin, host_client],
      env: environment(control_env),
      timeout: 30,
    )
  endif
endif
