# Check if nlohmann_json is a system dep; fall back to the vendored header.
nlohmann_dep = dependency('nlohmann_json', required : false)
if not nlohmann_dep.found()
  nlohmann_dep = declare_dependency(
    include_directories : include_directories('../../third_party/nlohmann'),
  )
endif

# httplib + nlohmann are header-only third-party code; the project's strict
# warning set (-Wconversion, -Wshadow, -Werror-adjacent flags) trips on their
# intentional patterns. Compile this TU with warnings suppressed.
web_lib = static_library(
  'transporter_web',
  'server.cpp',
  include_directories : [inc_public, include_directories('../..')],
  dependencies : [engine_dep, queue_dep, library_dep, httplib_dep, nlohmann_dep, flac_dep],
  cpp_args : ['-w'],
)

web_dep = declare_dependency(
  link_with : web_lib,
  include_directories : [inc_public],
  dependencies : [engine_dep, queue_dep, library_dep, httplib_dep, nlohmann_dep, flac_dep],
)
