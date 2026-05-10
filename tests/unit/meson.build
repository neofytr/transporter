test_wav_decode = executable(
  'test_wav_decode',
  'wav_decode.cpp',
  dependencies : [engine_dep],
)

test_format_match = executable(
  'test_format_match',
  'format_match.cpp',
  dependencies : [engine_dep],
)

test('wav_decode_44100_16',
  test_wav_decode,
  args : [meson.project_source_root() / 'fixtures' / 'sine_44100_16.wav'],
)

test('format_match_refuses_24bit',
  test_format_match,
  args : [meson.project_source_root() / 'fixtures' / 'sine_44100_24.wav'],
)
