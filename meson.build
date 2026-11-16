project('pipewireao-spa-plugin-iris', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.3',
  default_options: ['c_std=c11', 'warning_level=2'],
)

add_project_arguments('-D_GNU_SOURCE', language: 'c')
cc = meson.get_compiler('c')
spa = dependency('libspa-ao-0.2', include_type: 'system')
iris_libdir = get_option('iris-libdir')
if iris_libdir == ''
  iris_libdir = get_option('iris-prefix') / 'lib'
endif
iris = declare_dependency(
  dependencies: cc.find_library('iris', dirs: [iris_libdir]),
  include_directories: include_directories(get_option('iris-prefix') / 'include'),
)
subdir('spa/plugins/iris')
subdir('tests')
