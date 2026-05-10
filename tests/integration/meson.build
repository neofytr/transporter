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
