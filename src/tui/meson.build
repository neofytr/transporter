notcurses_dep = dependency('notcurses', required : true)

# notcurses may be installed in a non-standard prefix (e.g. ~/.local when the
# user built from source). Pull its libdir from pkg-config and embed it as
# rpath so executables can find libnotcurses + its transitive deps
# (libqrcodegen, libavformat, …) at runtime without LD_LIBRARY_PATH.
notcurses_libdir = notcurses_dep.get_variable(pkgconfig : 'libdir', default_value : '')
notcurses_rpath = []
if notcurses_libdir != '' and notcurses_libdir != '/usr/lib' and notcurses_libdir != '/usr/lib64' and notcurses_libdir != '/usr/lib/x86_64-linux-gnu'
  # Old-style RPATH (not RUNPATH) so transitive deps of libnotcurses-core
  # (libqrcodegen, libdeflate, libgpm, ...) are also resolved from the
  # non-standard prefix. Modern linkers default to RUNPATH which the dynamic
  # loader only consults for the direct NEEDED list.
  notcurses_rpath = ['-Wl,--disable-new-dtags', '-Wl,-rpath,' + notcurses_libdir]
endif

tui_lib = static_library(
  'transporter_tui',
  'notcurses_ctx.cpp',
  'app.cpp',
  'input.cpp',
  'components/player_bar.cpp',
  include_directories : [inc_public, include_directories('..')],
  dependencies : [engine_dep, library_dep, config_dep, notcurses_dep],
)

tui_dep = declare_dependency(
  link_with : tui_lib,
  include_directories : [include_directories('.')],
  dependencies : [notcurses_dep],
  link_args : notcurses_rpath,
)
