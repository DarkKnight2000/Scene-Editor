project('entt', 'cpp', version : '4.0.0', license : 'MIT')

entt_dep = declare_dependency(
  include_directories : include_directories('single_include', is_system : true),
)
meson.override_dependency('entt', entt_dep)
