# Audio engine. No UI deps. Public headers under include/transporter/engine/.

alsa_dep = dependency('alsa')

engine_sources = files(
  'engine.cpp',
  'alsa/output.cpp',
  'decoder/wav.cpp',
  'format/match.cpp',
)

engine_lib = static_library(
  'transporter_engine',
  engine_sources,
  include_directories : inc_public,
  dependencies : [alsa_dep],
)

engine_dep = declare_dependency(
  link_with : engine_lib,
  include_directories : inc_public,
  dependencies : [alsa_dep],
)
