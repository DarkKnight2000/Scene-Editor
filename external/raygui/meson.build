# Builds raygui (header-only) into a static library. The header is read from raygui_dir.

raygui_dir = get_option('raygui_dir')
if raygui_dir == ''
  raygui_dir = raylib_root / 'examples/core' # set by external/raylib
endif

if not fs.is_file(raygui_dir / 'raygui.h')
  error('raygui.h not found in "@0@" (set -Draygui_dir=...)'.format(raygui_dir))
endif

raygui_inc = include_directories(raygui_dir, is_system : true)

raygui_lib = static_library(
  'raygui',
  'raygui_impl.c',
  c_args : meson.get_compiler('c').get_supported_arguments('-w'),
  include_directories : raygui_inc,
  dependencies : raylib_dep,
  override_options : ['warning_level=0'],
)

raygui_dep = declare_dependency(
  link_with : raygui_lib,
  include_directories : raygui_inc,
  dependencies : raylib_dep,
)
