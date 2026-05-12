test_wav_play = executable(
  'test_wav_play',
  'wav_play.cpp',
  dependencies : [engine_dep],
)

test_play_each_format = executable(
  'test_play_each_format',
  'play_each_format.cpp',
  dependencies : [engine_dep],
)

test_engine_play = executable(
  'test_engine_play',
  'engine_play.cpp',
  dependencies : [engine_dep],
)

test_device_list = executable(
  'test_device_list',
  'device_list.cpp',
  dependencies : [engine_dep],
)

test_telemetry_dump = executable(
  'test_telemetry_dump',
  'telemetry_dump.cpp',
  dependencies : [engine_dep],
)

test_library_browse = executable(
  'test_library_browse',
  'library_browse.cpp',
  dependencies : [library_dep],
)

# Bit-perfect verification harness. Runs the production Engine against an
# `snd-aloop` virtual loopback card and byte-compares the captured payload to
# the source WAV. Lives in the needs-loopback suite: it skips (exit 77) when
# no card named "Loopback" is present.
test_bit_perfect_loopback = executable(
  'test_bit_perfect_loopback',
  ['bit_perfect_loopback.cpp', '../support/loopback_capture.cpp'],
  dependencies : [engine_dep],
)

# xrun stress harness. Drives long playback through the production engine
# against a real `hw:` device. Lives in the needs-stress suite: meant to be
# run alongside `stress-ng`. Default duration trimmed for CI registration.
test_xrun_stress = executable(
  'test_xrun_stress',
  'xrun_stress.cpp',
  dependencies : [engine_dep],
)

# Live-bus smoke test for DBus. Default suite; exits 77 (SKIP) when no
# session bus is reachable.
test_dbus_smoke = executable(
  'test_dbus_smoke',
  'dbus_smoke.cpp',
  dependencies : [dbus_dep],
)

# Default suite: runs unattended on any environment. Hardware-free.
test('device_list', test_device_list)

test('library_browse',
  test_library_browse,
  args : [
    meson.project_source_root() / 'tests' / 'data' / 'library',
    meson.project_build_root() / 'tests' / 'library_browse_tree',
  ],
  timeout : 60,
)

test('dbus_smoke', test_dbus_smoke)

# needs-alsa suite: requires an openable `hw:` playback device. Each test
# auto-discovers a usable device; if none, exits 77 (SKIP).
alsa_fixture = meson.project_source_root() / 'fixtures' / 'sine_44100_16.wav'
alsa_flac = meson.project_source_root() / 'fixtures' / 'sine_44100_16.flac'

test('wav_play',
  test_wav_play,
  args : ['auto', alsa_fixture],
  suite : 'needs-alsa',
  timeout : 60,
)

test('play_each_format',
  test_play_each_format,
  args : ['auto', meson.project_source_root() / 'fixtures'],
  suite : 'needs-alsa',
  timeout : 120,
)

test('engine_play',
  test_engine_play,
  args : ['auto', alsa_flac],
  suite : 'needs-alsa',
  timeout : 90,
)

test('telemetry_dump',
  test_telemetry_dump,
  args : ['auto', alsa_flac],
  suite : 'needs-alsa',
  timeout : 90,
)

# needs-loopback suite: requires `snd-aloop` kernel module loaded.
test('bit_perfect_loopback',
  test_bit_perfect_loopback,
  args : ['auto', alsa_fixture],
  suite : 'needs-loopback',
  timeout : 120,
)

# needs-stress suite: companion to `stress-ng`. Registered with a short
# duration so the suite is invokable; real stress runs override duration.
test('xrun_stress',
  test_xrun_stress,
  args : ['auto', '5', alsa_flac],
  suite : 'needs-stress',
  timeout : 60,
)
