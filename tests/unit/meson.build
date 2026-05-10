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

# Engine FSM tests use the engine-internal test seam (output_iface.hpp,
# engine_test_access.hpp). Add the engine source dir to includes.
test_engine_fsm = executable(
  'test_engine_fsm',
  'engine_fsm.cpp',
  include_directories : engine_internal_inc,
  dependencies : [engine_dep],
)
test('engine_fsm',
  test_engine_fsm,
  args : [meson.project_source_root() / 'fixtures' / 'sine_44100_16.flac'],
)

test_engine_rate_switch = executable(
  'test_engine_rate_switch',
  'engine_rate_switch.cpp',
  include_directories : engine_internal_inc,
  dependencies : [engine_dep],
)
test('engine_rate_switch',
  test_engine_rate_switch,
  args : [
    meson.project_source_root() / 'fixtures' / 'sine_44100_16.flac',
    meson.project_source_root() / 'fixtures' / 'sine_48000_16.flac',
  ],
)

test_engine_format_refused = executable(
  'test_engine_format_refused',
  'engine_format_refused.cpp',
  include_directories : engine_internal_inc,
  dependencies : [engine_dep],
)
test('engine_format_refused',
  test_engine_format_refused,
  args : [meson.project_source_root() / 'fixtures' / 'sine_44100_24.wav'],
)

test_device_stable_id = executable(
  'test_device_stable_id',
  'device_stable_id.cpp',
  dependencies : [engine_dep],
)
test('device_stable_id', test_device_stable_id)
