source_test = executable('test-iris-source', 'test-source.c',
  dependencies: [spa, cc.find_library('dl', required: false)],
)
test('factory', source_test, args: [plugin.full_path()], depends: plugin)
mock_cti = get_option('test-mock-cti')
if mock_cti != ''
  test('in-tree-mock-capture', source_test,
    args: [plugin.full_path(), mock_cti],
    depends: plugin,
    env: environment({'IRIS_FAULT': 'progressive-complete'}),
  )
endif

edt_cti = get_option('test-edt-cti')
andor_cti = get_option('test-andor-cti')
simulator_env = environment({
  'EDTPDV_GENTL_MOCK': '1',
  'EDTPDV_GENTL_MOCK_DEPTH': '16',
  'EDTPDV_ZERO_COPY': '0',
})
if edt_cti != '' and andor_cti != ''
  test('simultaneous-simulator-capture', source_test,
    args: [plugin.full_path(), edt_cti, andor_cti],
    depends: plugin,
    env: simulator_env,
    is_parallel: false,
    timeout: 45,
  )
elif edt_cti != '' or andor_cti != ''
  error('Set both simulator CTI paths to enable simultaneous capture tests')
endif

host_bindir = get_option('test-pipewireao-bindir')
if host_bindir != ''
  if mock_cti == '' and edt_cti == ''
    error('The PipeWireAO daemon test requires the in-tree mock or both external simulator CTIs')
  endif
  pipewire = dependency('libpipewire-ao-0.3', include_type: 'system')
  host_client = executable('test-iris-host-client', 'test-host-client.c',
    dependencies: [spa, pipewire],
  )
  python = import('python').find_installation()
  host_args = [
    files('test-host.py'), host_bindir, host_client.full_path(),
    plugin.full_path(), spa.get_variable(pkgconfig: 'plugindir'),
    pipewire.get_variable(pkgconfig: 'moduledir'),
  ]
  if mock_cti != ''
    test('pipewireao-mock-graph', python,
      args: host_args + [mock_cti],
      depends: [plugin, host_client],
      env: environment({'IRIS_FAULT': 'progressive-paced'}),
      timeout: 30,
    )
  endif
  if edt_cti != ''
    test('pipewireao-simulator-graph', python,
      args: host_args + [edt_cti, andor_cti],
      depends: [plugin, host_client],
      env: simulator_env,
      is_parallel: false,
      timeout: 30,
    )
  endif
endif
