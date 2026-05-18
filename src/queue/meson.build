queue_lib = static_library(
  'transporter_queue',
  'queue.cpp',
  include_directories : inc_public,
  dependencies : [engine_dep],
)

queue_dep = declare_dependency(
  link_with : queue_lib,
  include_directories : inc_public,
  dependencies : [engine_dep],
)
