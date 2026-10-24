project(
  'SceneEditor',
  'c', 'cpp',
  version : '0.1.0',
  meson_version : '>=1.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=3',
    'buildtype=debug',
    'force_fallback_for=entt,nlohmann_json', # always use the pinned subprojects/*.wrap versions
  ],
)

subdir('external/raylib')
subdir('external/raygui')

entt_dep = dependency('entt')
json_dep = dependency('nlohmann_json')

# Defaults for the Play button: build exported projects with this compiler and this raylib.
cc = meson.get_compiler('c')
play_cflags = '-std=c11 -O2 -Wall -Wextra'
if host_machine.system() == 'windows' and cc.get_id() == 'clang' and cc.get_define('_MSC_VER') != ''
  # MSVC ABI: the game must use the same C runtime as the raylib library it links.
  vscrt = get_option('b_vscrt')
  if vscrt in ['from_buildtype', 'static_from_buildtype']
    vscrt = (vscrt == 'from_buildtype' ? 'md' : 'mt') + (get_option('buildtype') == 'debug' ? 'd' : '')
  endif
  play_cflags += ' -fms-runtime-lib=' + {'md' : 'dll', 'mdd' : 'dll_dbg', 'mt' : 'static', 'mtd' : 'static_dbg'}.get(vscrt, 'dll')
endif
play_make = find_program('mingw32-make', 'gmake', 'make', required : false)
play_conf = configuration_data()
# Forward slashes: set_quoted doesn't escape backslashes, and make and the compilers accept them.
play_conf.set_quoted('SCENE_EDITOR_PLAY_MAKE', (play_make.found() ? play_make.full_path() : 'make').replace('\\', '/'))
play_conf.set_quoted('SCENE_EDITOR_PLAY_CC', ' '.join(cc.cmd_array()))
play_conf.set_quoted('SCENE_EDITOR_PLAY_CFLAGS', play_cflags)
play_conf.set_quoted('SCENE_EDITOR_PLAY_RAYLIB_INCLUDE', raylib_src.replace('\\', '/'))
play_conf.set_quoted('SCENE_EDITOR_PLAY_RAYLIB_LIBS', raylib_lib.full_path().replace('\\', '/'))
configure_file(output : 'PlayDefaults.h', configuration : play_conf)

# Default Box2D folder for new projects (Project Settings can point elsewhere, e.g.
# after the editor's source tree moves). Exports with physics copy Box2D from it.
paths_conf = configuration_data()
paths_conf.set_quoted('SCENE_EDITOR_BOX2D_DIR', (meson.current_source_dir() / 'external/box2d').replace('\\', '/'))
configure_file(output : 'EditorPaths.h', configuration : paths_conf)

app_link_args = []
if host_machine.system() == 'windows'
  # GUI subsystem (no console window) while keeping a portable main().
  app_link_args += '-Wl,/ENTRY:mainCRTStartup'
endif

executable(
  'SceneEditor',
  files(
    'src/main.cpp',
    'src/EditorConfig.cpp',
    'src/EditorLayout.cpp',
    'src/GameRunner.cpp',
    'src/ComponentRegistry.cpp',
    'src/DetailsPanel.cpp',
    'src/EntitiesPanel.cpp',
    'src/History.cpp',
    'src/Log.cpp',
    'src/LogsPanel.cpp',
    'src/Project.cpp',
    'src/ProjectLauncher.cpp',
    'src/Scene.cpp',
    'src/SceneExporter.cpp',
    'src/SceneSerializer.cpp',
    'src/SceneView.cpp',
    'src/Widgets.cpp',
    'src/Workspace.cpp',
  ),
  dependencies : [raygui_dep, entt_dep, json_dep],
  link_args : app_link_args,
  win_subsystem : 'windows',
)
