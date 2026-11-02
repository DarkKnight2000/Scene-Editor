project(
  'SceneEditor',
  'c', 'cpp',
  version : '0.1',
  meson_version : '>=1.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=3',
    'buildtype=debug',
    'force_fallback_for=nlohmann_json', # always use the pinned subprojects/*.wrap versions
  ],
)

# raylib and raygui (subprojects/raylib); -Draylib:raylib_dir=... uses a local raylib checkout.
raylib_proj = subproject('raylib')
raylib_dep = raylib_proj.get_variable('raylib_dep')
raygui_dep = raylib_proj.get_variable('raygui_dep')
raylib_dir = raylib_proj.get_variable('raylib_dir')
raylib_src = raylib_proj.get_variable('raylib_src')
raylib_lib = raylib_proj.get_variable('raylib_lib')

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
paths_conf.set_quoted('SCENE_EDITOR_BOX2D_DIR', (meson.current_source_dir() / 'subprojects/box2d').replace('\\', '/'))
configure_file(output : 'EditorPaths.h', configuration : paths_conf)

# The editor's version (--version), from project() above.
version_conf = configuration_data()
version_conf.set_quoted('SCENE_EDITOR_VERSION', meson.project_version())
configure_file(output : 'EditorVersion.h', configuration : version_conf)

app_link_args = []
if host_machine.system() == 'windows' and meson.get_compiler('cpp').get_linker_id() in ['link', 'lld-link']
  # GUI subsystem (no console window) while keeping a portable main(). MinGW does this itself.
  app_link_args += '-Wl,/ENTRY:mainCRTStartup'
endif

# Everything but main(), shared by the editor and the internal debug exporter.
editor_lib = static_library(
  'editor',
  files(
    'src/AppPaths.cpp',
    'src/Console.cpp',
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
    'src/ProjectExport.cpp',
    'src/ProjectLauncher.cpp',
    'src/Scene.cpp',
    'src/SceneExporter.cpp',
    'src/SceneSerializer.cpp',
    'src/SceneView.cpp',
    'src/Theme.cpp',
    'src/Widgets.cpp',
    'src/Workspace.cpp',
  ),
  dependencies : [raygui_dep, json_dep],
)

# The icon (res/, drawn by tools/make_icon.py).
app_resources = []
if host_machine.system() == 'windows'
  app_resources += import('windows').compile_resources('res/SceneEditor.rc',
                                                       depend_files : 'res/SceneEditor.ico')
endif

editor_exe = executable(
  'SceneEditor',
  'src/main.cpp',
  app_resources,
  link_with : editor_lib,
  dependencies : [raygui_dep, json_dep],
  link_args : app_link_args,
  win_subsystem : 'windows',
)

# Internal, not for users: exports a project and adds debug-only checks to it (debug/), for
# testing the exporter. Build it with: meson compile -C build SceneDebugExport
executable(
  'SceneDebugExport',
  'tools/DebugExporter.cpp',
  link_with : editor_lib,
  include_directories : include_directories('src'),
  dependencies : [raygui_dep, json_dep],
  build_by_default : false,
)

# Windows installer (NSIS, tools/SceneEditor.nsi), from a release build with the C runtime
# linked in, so it runs without the Visual C++ redistributable:
#   meson setup build-release --buildtype=release -Db_vscrt=mt
#   meson compile -C build-release installer      -> SceneEditor-<version>-setup.exe
makensis = find_program('makensis', dirs : ['C:/Program Files (x86)/NSIS', 'C:/Program Files/NSIS'],
                        required : false)
if host_machine.system() == 'windows' and makensis.found()
  if get_option('buildtype') == 'debug' or get_option('b_vscrt') not in ['mt', 'static_from_buildtype']
    message('The installer target is meant for a release build with -Db_vscrt=mt (see meson.build)')
  endif
  run_target('installer',
    command : [makensis, '-V2',
               '-DVERSION=' + meson.project_version(),
               '-DEXE=' + editor_exe.full_path(),
               '-DICON=' + meson.current_source_dir() / 'res/SceneEditor.ico',
               '-DLICENSE=' + meson.current_source_dir() / 'LICENSE',
               '-DBOX2D_DIR=' + meson.current_source_dir() / 'subprojects/box2d',
               '-DRAYLIB_SRC=' + raylib_src,
               '-DRAYLIB_LICENSE=' + raylib_dir / 'LICENSE',
               '-DOUTFILE=' + meson.current_build_dir() / 'SceneEditor-' + meson.project_version() + '-setup.exe',
               meson.current_source_dir() / 'tools/SceneEditor.nsi'],
    depends : editor_exe,
  )
endif
