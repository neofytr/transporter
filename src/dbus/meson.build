# DBus services: standard MPRIS 2 + custom transporter interface.
# sdbus-c++ 2.x; we pin the minimum to 1.4.0 (older 1.x branches lack the
# fluent VTableAdder API). Detected version on this box: 2.1.0.

sdbus_cpp_dep = dependency('sdbus-c++', version : '>=1.4.0')

dbus_sources = files(
  'bus.cpp',
  'mpris_metadata.cpp',
  'mpris_player.cpp',
  'mpris_root.cpp',
  'mpris_tracklist.cpp',
  'service.cpp',
  'snapshot_serialize.cpp',
  'transporter_iface.cpp',
)

dbus_internal_inc = include_directories('.')

dbus_lib = static_library(
  'transporter_dbus',
  dbus_sources,
  include_directories : [inc_public, dbus_internal_inc],
  dependencies : [engine_dep, library_dep, sdbus_cpp_dep],
)

dbus_dep = declare_dependency(
  link_with : dbus_lib,
  include_directories : [inc_public],
  dependencies : [engine_dep, library_dep, sdbus_cpp_dep],
)
