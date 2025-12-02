# SPDX-License-Identifier: MIT
# src/spprof/meson.build - Python package and C extension

# Install Python source files
py.install_sources(
  '__init__.py',
  'output.py',
  '_profiler.pyi',
  'py.typed',
  subdir: 'spprof',
)

# ============================================================================
# C Extension: _native
# ============================================================================

ext_src_dir = '_ext'

# Core sources (always included)
core_sources = files(
  ext_src_dir / 'module.c',
  ext_src_dir / 'ringbuffer.c',
  ext_src_dir / 'resolver.c',
  ext_src_dir / 'unwind.c',
  ext_src_dir / 'code_registry.c',
  ext_src_dir / 'framewalker.c',
  ext_src_dir / 'signal_handler.c',
)

# Include directories
ext_inc_dirs = include_directories(
  ext_src_dir,
  ext_src_dir / 'platform',
  ext_src_dir / 'internal',
)

# Platform-specific sources and dependencies
platform_sources = []
platform_deps = []
platform_link_args = []

if host_machine.system() == 'linux'
  platform_sources += files(ext_src_dir / 'platform' / 'linux.c')
  
  # Linux libraries
  rt_dep = cc.find_library('rt', required: true)
  dl_dep = cc.find_library('dl', required: true)
  pthread_dep = cc.find_library('pthread', required: true)
  platform_deps += [rt_dep, dl_dep, pthread_dep]
  
  # Optional libunwind for advanced unwinding
  libunwind_dep = dependency('libunwind', required: false)
  if libunwind_dep.found()
    platform_deps += libunwind_dep
    add_project_arguments('-DSPPROF_HAS_LIBUNWIND=1', language: 'c')
    message('Found libunwind - enabling advanced unwinding')
  endif

elif host_machine.system() == 'darwin'
  platform_sources += files(
    ext_src_dir / 'platform' / 'darwin.c',
    ext_src_dir / 'platform' / 'darwin_mach.c',
  )
  
  # macOS frameworks
  corefoundation_dep = dependency('appleframeworks', modules: ['CoreFoundation'], required: true)
  platform_deps += corefoundation_dep
  
  # macOS minimum version
  if cc.get_id() in ['gcc', 'clang']
    common_c_args += ['-mmacosx-version-min=10.15']
    platform_link_args += ['-mmacosx-version-min=10.15']
  endif

elif host_machine.system() == 'windows'
  platform_sources += files(ext_src_dir / 'platform' / 'windows.c')
  
  # Windows libraries for symbol resolution
  dbghelp_dep = cc.find_library('dbghelp', required: true)
  platform_deps += dbghelp_dep
endif

# Build the extension module
py.extension_module(
  '_native',
  sources: core_sources + platform_sources,
  include_directories: ext_inc_dirs,
  dependencies: [py_dep] + platform_deps,
  c_args: common_c_args,
  link_args: platform_link_args,
  install: true,
  subdir: 'spprof',
)

