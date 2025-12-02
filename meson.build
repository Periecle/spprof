# SPDX-License-Identifier: MIT
# meson.build - Root build configuration for spprof

project(
  'spprof',
  'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.2.0',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'buildtype=release',
  ],
)

# Get Python installation
py = import('python').find_installation(pure: false)
py_dep = py.dependency()

# Get Python version info for preprocessor defines
py_version = py.language_version().split('.')
py_major = py_version[0]
py_minor = py_version[1]

# Compiler setup
cc = meson.get_compiler('c')

# Common compile args
common_c_args = []

if cc.get_id() in ['gcc', 'clang']
  common_c_args += [
    '-Wno-unused-function',
    '-Wno-missing-field-initializers',
    '-Wno-unused-parameter',
    '-fvisibility=hidden',
    '-fPIC',
  ]
elif cc.get_id() == 'msvc'
  common_c_args += [
    '/D_CRT_SECURE_NO_WARNINGS',
  ]
endif

# Debug build configuration
if get_option('buildtype') == 'debug'
  common_c_args += cc.get_id() in ['gcc', 'clang'] ? ['-g', '-O0'] : ['/Zi', '/Od']
  add_project_arguments('-DSPPROF_DEBUG=1', language: 'c')
endif

# Version defines
add_project_arguments(
  '-DSPPROF_PY_MAJOR=' + py_major,
  '-DSPPROF_PY_MINOR=' + py_minor,
  language: 'c',
)

# Process the Python package
subdir('src/spprof')

