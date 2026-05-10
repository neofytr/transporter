# Vendored Apple ALAC reference codec. Apache-2.0 (see LICENSE).
# Upstream pulls in -Wsign-conversion / -Wcast-align violations we do not
# patch; suppress strict warnings for this target only.

alac_codec_dir = 'codec'

alac_sources = files(
  'codec/ag_dec.c',
  'codec/ag_enc.c',
  'codec/ALACBitUtilities.c',
  'codec/ALACDecoder.cpp',
  'codec/ALACEncoder.cpp',
  'codec/dp_dec.c',
  'codec/dp_enc.c',
  'codec/EndianPortable.c',
  'codec/matrix_dec.c',
  'codec/matrix_enc.c',
)

alac_quiet = [
  '-w',
  '-Wno-multichar',
]

alac_lib = static_library(
  'alac_vendored',
  alac_sources,
  include_directories : include_directories('codec'),
  c_args : alac_quiet,
  cpp_args : alac_quiet,
)

alac_dep = declare_dependency(
  link_with : alac_lib,
  include_directories : include_directories('codec'),
)
