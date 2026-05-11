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

test_rt_apply = executable(
  'test_rt_apply',
  'rt_apply.cpp',
  dependencies : [engine_dep],
)
test('rt_apply', test_rt_apply)

test_trace_ring = executable(
  'test_trace_ring',
  'trace_ring.cpp',
  dependencies : [engine_dep],
)
test('trace_ring', test_trace_ring)

test_pipeline_snapshot = executable(
  'test_pipeline_snapshot',
  'pipeline_snapshot.cpp',
  include_directories : engine_internal_inc,
  dependencies : [engine_dep],
)
test('pipeline_snapshot',
  test_pipeline_snapshot,
  args : [meson.project_source_root() / 'fixtures' / 'sine_44100_16.flac'],
)

test_hotplug_fsm = executable(
  'test_hotplug_fsm',
  'hotplug_fsm.cpp',
  include_directories : engine_internal_inc,
  dependencies : [engine_dep, hotplug_dep],
)
test('hotplug_fsm',
  test_hotplug_fsm,
  args : [meson.project_source_root() / 'fixtures' / 'sine_44100_16.flac'],
)

test_library_schema = executable(
  'test_library_schema',
  'library_schema.cpp',
  dependencies : [library_dep],
)
test('library_schema', test_library_schema)

test_library_scan = executable(
  'test_library_scan',
  'library_scan.cpp',
  dependencies : [library_dep],
)
test('library_scan',
  test_library_scan,
  args : [
    meson.project_source_root() / 'tests' / 'data' / 'library',
    meson.project_build_root() / 'tests' / 'library_scan_tree',
  ],
  timeout : 60,
)

test_library_search = executable(
  'test_library_search',
  'library_search.cpp',
  dependencies : [library_dep],
)
test('library_search',
  test_library_search,
  args : [
    meson.project_source_root() / 'tests' / 'data' / 'library',
    meson.project_build_root() / 'tests' / 'library_search_tree',
  ],
  timeout : 60,
)

test_config_toml = executable(
  'test_config_toml',
  'config_toml.cpp',
  dependencies : [config_dep],
)
test('config_toml', test_config_toml)

test_gui_pipeline_render = executable(
  'test_gui_pipeline_render',
  'gui_pipeline_render.cpp',
  dependencies : [gui_dep, engine_dep, imgui_dep],
)
test('gui_pipeline_render', test_gui_pipeline_render)

test_dbus_metadata = executable(
  'test_dbus_metadata',
  'dbus_metadata.cpp',
  include_directories : dbus_internal_inc,
  dependencies : [dbus_dep, engine_dep],
)
test('dbus_metadata', test_dbus_metadata)

# doctest is the project's locked unit-test framework (single-header, vendored
# under third_party/doctest). The pre-doctest tests above use plain main()
# returning 0/non-zero and are not migrated. New unit tests should prefer
# doctest.
test_loopback_byte_compare = executable(
  'test_loopback_byte_compare',
  'loopback_byte_compare.cpp',
  include_directories : include_directories('..'),
  dependencies : [doctest_dep],
)
test('loopback_byte_compare', test_loopback_byte_compare)

test_queue_ops = executable(
  'test_queue_ops',
  'queue_ops.cpp',
  dependencies : [gui_dep, engine_dep, imgui_dep, doctest_dep],
)
test('queue_ops', test_queue_ops)
