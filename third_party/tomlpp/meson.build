# Vendored toml++ single header. MIT (see LICENSE).

tomlpp_inc = include_directories('.')

tomlpp_dep = declare_dependency(
  include_directories : tomlpp_inc,
  compile_args : ['-DTOML_HEADER_ONLY=1'],
)
