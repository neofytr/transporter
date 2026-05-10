# udev_monitor.cpp is compiled into engine_lib (see src/engine/meson.build)
# so the engine library is self-sufficient. This module exposes only the
# public header at include/transporter/hotplug/monitor.hpp.

hotplug_dep = declare_dependency(
  include_directories : inc_public,
  dependencies : [udev_dep, engine_dep],
)
