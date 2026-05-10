config_lib = static_library(
  'transporter_config',
  'config.cpp',
  include_directories : inc_public,
  dependencies : [tomlpp_dep],
)

config_dep = declare_dependency(
  link_with : config_lib,
  include_directories : inc_public,
  dependencies : [tomlpp_dep],
)
