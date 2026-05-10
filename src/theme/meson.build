theme_lib = static_library(
  'transporter_theme',
  'theme.cpp',
  'hypr_parser.cpp',
  include_directories : [inc_public, include_directories('.')],
  dependencies : [imgui_dep],
)

theme_dep = declare_dependency(
  link_with : theme_lib,
  include_directories : [inc_public],
)
