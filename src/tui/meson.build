notcurses_dep = dependency('notcurses', required : true)

tui_lib = static_library(
  'transporter_tui',
  'notcurses_ctx.cpp',
  include_directories : [inc_public, include_directories('..')],
  dependencies : [engine_dep, library_dep, config_dep, notcurses_dep],
)

tui_dep = declare_dependency(
  link_with : tui_lib,
  include_directories : [include_directories('.')],
  dependencies : [notcurses_dep],
)
