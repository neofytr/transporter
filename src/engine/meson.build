# Audio engine. No UI deps. Public headers under include/transporter/engine/.

alsa_dep = dependency('alsa')
flac_dep = dependency('flac')
mpg123_dep = dependency('libmpg123')
vorbisfile_dep = dependency('vorbisfile')
opusfile_dep = dependency('opusfile')
udev_dep = dependency('libudev')

# alac.cpp pulls Apple's ALACAudioTypes.h whose multi-char enum literals
# trip -Wpedantic. The vendored sources are silenced inside their own
# meson.build; consumers need -Wno-multichar locally.
alac_glue_lib = static_library(
  'transporter_engine_alac_glue',
  files('decoder/alac.cpp'),
  include_directories : inc_public,
  dependencies : [alac_dep],
  cpp_args : ['-Wno-multichar'],
)

engine_sources = files(
  'engine.cpp',
  'alsa/output.cpp',
  'decoder/aiff.cpp',
  'decoder/detect.cpp',
  'decoder/flac.cpp',
  'decoder/mp3.cpp',
  'decoder/mp4_demux.cpp',
  'decoder/opus.cpp',
  'decoder/vorbis.cpp',
  'decoder/wav.cpp',
  'device/fingerprint.cpp',
  'device/list.cpp',
  'device/probe.cpp',
  'format/match.cpp',
)

engine_internal_inc = include_directories('.')

engine_lib = static_library(
  'transporter_engine',
  engine_sources,
  include_directories : inc_public,
  link_whole : [alac_glue_lib],
  dependencies : [alsa_dep, flac_dep, mpg123_dep, vorbisfile_dep, opusfile_dep, alac_dep, udev_dep],
)

engine_dep = declare_dependency(
  link_with : engine_lib,
  include_directories : inc_public,
  dependencies : [alsa_dep, flac_dep, mpg123_dep, vorbisfile_dep, opusfile_dep, alac_dep, udev_dep],
)
