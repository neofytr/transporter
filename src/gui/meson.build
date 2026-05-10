wayland_client_dep = dependency('wayland-client')
xkbcommon_dep      = dependency('xkbcommon')

wayland_scanner = find_program('wayland-scanner')

xdg_shell_xml = '/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml'

xdg_shell_client_h = custom_target(
  'xdg-shell-client-protocol.h',
  output : 'xdg-shell-client-protocol.h',
  input  : xdg_shell_xml,
  command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_client_c = custom_target(
  'xdg-shell-protocol.c',
  output : 'xdg-shell-protocol.c',
  input  : xdg_shell_xml,
  command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

# Generated wayland glue: warnings unrelated to our code.
wayland_proto_lib = static_library(
  'transporter_gui_wayland_protocols',
  xdg_shell_client_c,
  xdg_shell_client_h,
  c_args : ['-w'],
  dependencies : [wayland_client_dep],
)

wayland_proto_inc = include_directories('platform')

gui_sources = files(
  'gui.cpp',
  'platform/platform.cpp',
  'platform/wayland.cpp',
  'platform/xdg_shell.cpp',
  'platform/egl.cpp',
  'platform/shm.cpp',
  'platform/render.cpp',
  'platform/input.cpp',
  'views/app.cpp',
  'views/main.cpp',
  'views/library.cpp',
  'views/pipeline.cpp',
  'views/queue.cpp',
)

gui_lib = static_library(
  'transporter_gui',
  gui_sources,
  xdg_shell_client_h,
  include_directories : [inc_public],
  link_with : [wayland_proto_lib],
  dependencies : [
    imgui_dep,
    engine_dep,
    library_dep,
    config_dep,
    dbus_dep,
    theme_dep,
    wayland_client_dep,
    xkbcommon_dep,
  ],
  cpp_args : ['-Wno-old-style-cast'],
)

gui_dep = declare_dependency(
  link_with : gui_lib,
  include_directories : [inc_public],
  dependencies : [
    imgui_dep,
    engine_dep,
    library_dep,
    config_dep,
    dbus_dep,
    theme_dep,
    wayland_client_dep,
    xkbcommon_dep,
  ],
)
