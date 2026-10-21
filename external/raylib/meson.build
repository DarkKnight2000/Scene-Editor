# Builds raylib as a static library from an external source checkout.
# Only the sources are read from raylib_dir; all objects land in this project's build dir.

raylib_dir = get_option('raylib_dir')
raylib_src = raylib_dir / 'src'

if not import('fs').is_file(raylib_src / 'raylib.h')
  error('raylib sources not found at "@0@" (set -Draylib_dir=...)'.format(raylib_dir))
endif

cc = meson.get_compiler('c')

raylib_c_args = [
  '-DPLATFORM_DESKTOP',
  '-DGRAPHICS_API_OPENGL_33',
  '-D_GNU_SOURCE',
]
# Third-party code: don't flood the build with its warnings.
raylib_c_args += cc.get_supported_arguments('-w')

raylib_deps = []
if host_machine.system() == 'windows'
  foreach lib : ['opengl32', 'gdi32', 'winmm', 'shell32', 'user32']
    raylib_deps += cc.find_library(lib)
  endforeach
else
  raylib_deps += [dependency('threads'), cc.find_library('m', required : false),
                  cc.find_library('dl', required : false), dependency('gl')]
  raylib_c_args += '-D_GLFW_X11'
  raylib_deps += dependency('x11')
endif

raylib_inc = include_directories(raylib_src, is_system : true)
raylib_glfw_inc = include_directories(raylib_src / 'external/glfw/include', is_system : true)

raylib_lib = static_library(
  'raylib',
  raylib_src / 'rcore.c',
  raylib_src / 'rshapes.c',
  raylib_src / 'rtextures.c',
  raylib_src / 'rtext.c',
  raylib_src / 'rmodels.c',
  raylib_src / 'raudio.c',
  raylib_src / 'rglfw.c',
  c_args : raylib_c_args,
  include_directories : [raylib_inc, raylib_glfw_inc],
  dependencies : raylib_deps,
  override_options : ['warning_level=0', 'c_std=gnu99'],
)

raylib_dep = declare_dependency(
  link_with : raylib_lib,
  include_directories : raylib_inc,
  dependencies : raylib_deps,
)
