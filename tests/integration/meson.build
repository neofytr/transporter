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

# Live-bus smoke test for DBus. Not registered as a `meson test` because it
# requires a reachable session bus and exits 0 when none is found.
test_dbus_smoke = executable(
  'test_dbus_smoke',
  'dbus_smoke.cpp',
  dependencies : [dbus_dep],
)

# Bit-perfect verification harness. Runs the production Engine against an
# `snd-aloop` virtual loopback card and byte-compares the captured payload to
# the source WAV. Owner-side acceptance test: not a `meson test` because it
# needs `sudo modprobe snd-aloop` on the target box.
test_bit_perfect_loopback = executable(
  'test_bit_perfect_loopback',
  ['bit_perfect_loopback.cpp', '../support/loopback_capture.cpp'],
  dependencies : [engine_dep],
)

# xrun stress harness. Drives long playback against a real DAC; meant to be
# run alongside stress-ng. Owner-side; not a `meson test`.
test_xrun_stress = executable(
  'test_xrun_stress',
  'xrun_stress.cpp',
  dependencies : [engine_dep],
)
