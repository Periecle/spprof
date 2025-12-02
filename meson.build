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
    'warning_level=3',
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
    # Pedantic warnings for high code quality
    '-pedantic',
    '-Wextra',
    '-Wconversion',
    '-Wsign-conversion',
    '-Wdouble-promotion',
    '-Wformat=2',
    '-Wshadow',
    '-Wundef',
    '-Wcast-qual',
    '-Wwrite-strings',
    '-Wnull-dereference',
    # Suppress warnings that are unavoidable in Python C extensions
    '-Wno-unused-parameter',       # Python API requires 'self' even when unused
    '-Wno-missing-field-initializers',  # Common with designated initializers
    # Code generation
    '-fvisibility=hidden',
    '-fPIC',
  ]
  # GCC-specific warnings
  if cc.get_id() == 'gcc'
    common_c_args += [
      '-Wlogical-op',
      '-Wduplicated-cond',
      '-Wduplicated-branches',
    ]
  endif
elif cc.get_id() == 'msvc'
  common_c_args += [
    '/W4',                        # High warning level
    '/D_CRT_SECURE_NO_WARNINGS',
    '/wd4100',                    # unreferenced formal parameter (like -Wno-unused-parameter)
    '/wd4115',                    # named type definition in parentheses (from Python's pytime.h)
    '/wd4702',                    # unreachable code (false positives from uthash macros)
  ]
endif

# Debug build configuration
if get_option('buildtype') == 'debug'
  common_c_args += cc.get_id() in ['gcc', 'clang'] ? ['-g', '-O0'] : ['/Zi', '/Od']
  add_project_arguments('-DSPPROF_DEBUG=1', language: 'c')
endif

# Treat warnings as errors (for CI)
if get_option('strict')
  common_c_args += cc.get_id() in ['gcc', 'clang'] ? ['-Werror'] : ['/WX']
  message('Strict mode: treating warnings as errors')
endif

# Sanitizer configuration
sanitize_opt = get_option('sanitize')
if sanitize_opt != 'none' and cc.get_id() in ['gcc', 'clang']
  sanitize_args = ['-fsanitize=' + sanitize_opt, '-fno-omit-frame-pointer', '-g']
  common_c_args += sanitize_args
  add_project_link_arguments(sanitize_args, language: 'c')
  message('Sanitizers enabled: ' + sanitize_opt)
endif

# Version defines
add_project_arguments(
  '-DSPPROF_PY_MAJOR=' + py_major,
  '-DSPPROF_PY_MINOR=' + py_minor,
  language: 'c',
)

# Process the Python package
subdir('src/spprof')

