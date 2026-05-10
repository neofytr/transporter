# Audio engine. No UI deps. Public headers under include/transporter/engine/.
# Subdirs (alsa, decoder, format, ring, rt, trace, device) hold implementation
# files; for Phase 0 the engine is an empty stub. Submodule meson.build files
# are added here as those phases land.

engine_lib = static_library(
  'transporter_engine',
  'engine.cpp',
  include_directories : inc_public,
)
