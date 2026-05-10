# Vendored Dear ImGui v1.91.5 + OpenGL3 backend. MIT (see LICENSE).
# Upstream is strict-warning-clean enough but we suppress all warnings on the
# vendored target to keep -Wsign-conversion, -Wold-style-cast etc. quiet.

imgui_sources = files(
  'imgui.cpp',
  'imgui_demo.cpp',
  'imgui_draw.cpp',
  'imgui_tables.cpp',
  'imgui_widgets.cpp',
  'backends/imgui_impl_opengl3.cpp',
)

imgui_quiet = ['-w']

imgui_inc = include_directories('.', 'backends')

imgui_lib = static_library(
  'imgui_vendored',
  imgui_sources,
  include_directories : imgui_inc,
  cpp_args : imgui_quiet,
)

imgui_dep = declare_dependency(
  link_with : imgui_lib,
  include_directories : imgui_inc,
)
