# Builds raylib as a static library from a source checkout.
# Only the sources are read from raylib_dir; all objects land in this project's build dir.
#
# Without -Draylib_dir, the pinned raylib release below is downloaded once with git into
# external/downloads/ (not tracked) and used from there.

raylib_version = '6.0' # a tag of https://github.com/raysan5/raylib
raylib_url = 'https://github.com/raysan5/raylib.git'

fs = import('fs')
raylib_dir = get_option('raylib_dir')
# raylib_root: raylib_dir as sources and include_directories() can take it. A folder inside
# the source tree must be relative, so for the downloaded copy it's relative to
# external/raylib (and, being a sibling, external/raygui).
raylib_root = raylib_dir
if raylib_dir == ''
  raylib_root = '../downloads/raylib-' + raylib_version
  raylib_dir = meson.project_source_root() / 'external' / 'downloads' / 'raylib-' + raylib_version
  if not fs.is_file(raylib_dir / 'src' / 'raylib.h')
    if fs.exists(raylib_dir)
      error('"@0@" exists but has no src/raylib.h: delete it to download raylib again'.format(raylib_dir))
    endif
    git = find_program('git', required : false)
    if not git.found()
      error('raylib not given and git not found to download it: install git, or set -Draylib_dir=path/to/raylib')
    endif
    message('Downloading raylib @0@ into @1@'.format(raylib_version, raylib_dir))
    clone = run_command(git, '-c', 'advice.detachedHead=false', 'clone', '--depth', '1', '--branch', raylib_version,
                        raylib_url, raylib_dir, check : false)
    if clone.returncode() != 0
      error('Downloading raylib failed (set -Draylib_dir=path/to/raylib to use a local copy):\n' + clone.stderr())
    endif
  endif
endif
raylib_src = raylib_dir / 'src'

if not fs.is_file(raylib_src / 'raylib.h')
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

raylib_inc = include_directories(raylib_root / 'src', is_system : true)
raylib_glfw_inc = include_directories(raylib_root / 'src/external/glfw/include', is_system : true)

raylib_lib = static_library(
  'raylib',
  raylib_root / 'src/rcore.c',
  raylib_root / 'src/rshapes.c',
  raylib_root / 'src/rtextures.c',
  raylib_root / 'src/rtext.c',
  raylib_root / 'src/rmodels.c',
  raylib_root / 'src/raudio.c',
  raylib_root / 'src/rglfw.c',
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
