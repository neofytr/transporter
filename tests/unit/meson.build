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

decoder_format_tests = [
  ['decoder_wav', 'sine_44100_16.wav'],
  ['decoder_aiff', 'sine_44100_16.aiff'],
  ['decoder_flac', 'sine_44100_16.flac'],
  ['decoder_alac', 'sine_44100_16.m4a'],
  ['decoder_mp3', 'sine_44100_16.mp3'],
  ['decoder_vorbis', 'sine_44100_16.ogg'],
  ['decoder_opus', 'sine_44100_16.opus'],
]

foreach t : decoder_format_tests
  exe = executable(
    'test_' + t[0],
    t[0] + '.cpp',
    dependencies : [engine_dep],
  )
  test(t[0],
    exe,
    args : [meson.project_source_root() / 'fixtures' / t[1]],
  )
endforeach
