sqlite3_dep = dependency('sqlite3')

library_sources = files(
  'library.cpp',
  'db.cpp',
  'schema.cpp',
  'scanner.cpp',
)

library_lib = static_library(
  'transporter_library',
  library_sources,
  include_directories : inc_public,
  dependencies : [sqlite3_dep, engine_dep],
)

library_dep = declare_dependency(
  link_with : library_lib,
  include_directories : inc_public,
  dependencies : [sqlite3_dep, engine_dep],
)
