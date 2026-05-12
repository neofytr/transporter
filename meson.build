project(
  'transporter',
  ['cpp', 'c'],
  version : '0.0.0',
  license : 'GPL-3.0-or-later',
  meson_version : '>=1.1.0',
  default_options : [
    'cpp_std=c++23',
    'warning_level=3',
    'b_ndebug=if-release',
    'buildtype=debugoptimized',
  ],
)

cxx = meson.get_compiler('cpp')

# Strict extras on top of Meson's warning_level=3 (which gives -Wall -Wextra -Wpedantic).
# Per-module meson.build files may extend, never relax.
warning_flags = [
  '-Wshadow',
  '-Wnon-virtual-dtor',
  '-Wold-style-cast',
  '-Wcast-align',
  '-Wunused',
  '-Woverloaded-virtual',
  '-Wconversion',
  '-Wsign-conversion',
  '-Wdouble-promotion',
  '-Wformat=2',
  '-Wimplicit-fallthrough',
]
add_project_arguments(cxx.get_supported_arguments(warning_flags), language : 'cpp')

if get_option('native_arch')
  native_flags = cxx.get_supported_arguments(['-march=native', '-mtune=native'])
  add_project_arguments(native_flags, language : 'cpp')
endif

san = get_option('sanitizer')
if san != 'none'
  san_flag = '-fsanitize=' + san
  if cxx.has_argument(san_flag)
    add_project_arguments(san_flag, '-fno-omit-frame-pointer', language : 'cpp')
    add_project_link_arguments(san_flag, language : 'cpp')
  else
    error('compiler does not support sanitizer: ' + san)
  endif
endif

inc_public = include_directories('include')

subdir('third_party/alac')
subdir('third_party/doctest')
subdir('third_party/tomlpp')
subdir('src/engine')
subdir('src/library')
subdir('src/theme')
subdir('src/dbus')
subdir('src/hotplug')
subdir('src/config')

executable(
  'transporter',
  'src/main.cpp',
  include_directories : [inc_public, include_directories('src')],
  dependencies : [engine_dep, library_dep, config_dep, dbus_dep],
  install : true,
)

subdir('tests/unit')
subdir('tests/integration')
