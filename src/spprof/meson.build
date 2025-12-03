# SPDX-License-Identifier: MIT
# src/spprof/meson.build - Python package and C extension

# Install Python source files
py.install_sources(
  '__init__.py',
  'output.py',
  'memprof.py',
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

# Memory profiler sources
memprof_sources = files(
  ext_src_dir / 'memprof' / 'memprof.c',
  ext_src_dir / 'memprof' / 'heap_map.c',
  ext_src_dir / 'memprof' / 'stack_intern.c',
  ext_src_dir / 'memprof' / 'bloom.c',
  ext_src_dir / 'memprof' / 'sampling.c',
  ext_src_dir / 'memprof' / 'stack_capture.c',
)

# Include directories
ext_inc_dirs = include_directories(
  ext_src_dir,
  ext_src_dir / 'platform',
  ext_src_dir / 'internal',
  ext_src_dir / 'memprof',
)

# Platform-specific sources and dependencies
platform_sources = []
platform_deps = []
platform_link_args = []

if host_machine.system() == 'linux'
  platform_sources += files(ext_src_dir / 'platform' / 'linux.c')
  platform_sources += files(ext_src_dir / 'platform' / 'linux_memprof.c')
  
  # Linux libraries
  rt_dep = cc.find_library('rt', required: true)
  dl_dep = cc.find_library('dl', required: true)
  pthread_dep = cc.find_library('pthread', required: true)
  m_dep = cc.find_library('m', required: true)  # For math functions (log)
  platform_deps += [rt_dep, dl_dep, pthread_dep, m_dep]
  
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
    ext_src_dir / 'platform' / 'darwin_memprof.c',
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
  platform_sources += files(ext_src_dir / 'platform' / 'windows_memprof.c')
  
  # Windows libraries for symbol resolution
  dbghelp_dep = cc.find_library('dbghelp', required: true)
  platform_deps += dbghelp_dep
endif

# Build the extension module
py.extension_module(
  '_native',
  sources: core_sources + memprof_sources + platform_sources,
  include_directories: ext_inc_dirs,
  dependencies: [py_dep] + platform_deps,
  c_args: common_c_args,
  link_args: platform_link_args,
  install: true,
  subdir: 'spprof',
)

# ============================================================================
# Linux LD_PRELOAD Library: libspprof_alloc.so
# ============================================================================

if host_machine.system() == 'linux'
  # Build the LD_PRELOAD interposition library for complete allocation tracking.
  # This library is loaded via LD_PRELOAD to intercept malloc/free calls.
  
  alloc_lib_sources = files(
    ext_src_dir / 'memprof' / 'memprof.c',
    ext_src_dir / 'memprof' / 'heap_map.c',
    ext_src_dir / 'memprof' / 'stack_intern.c',
    ext_src_dir / 'memprof' / 'bloom.c',
    ext_src_dir / 'memprof' / 'sampling.c',
    ext_src_dir / 'memprof' / 'stack_capture.c',
    ext_src_dir / 'platform' / 'linux_memprof.c',
  )
  
  # Compiler flags for the LD_PRELOAD library
  alloc_lib_c_args = common_c_args + [
    '-DSPPROF_STANDALONE_LIB=1',  # Mark as standalone (no Python)
    '-fPIC',
  ]
  
  # Libraries needed
  alloc_lib_deps = [dl_dep, pthread_dep, m_dep]
  
  shared_library(
    'spprof_alloc',
    sources: alloc_lib_sources,
    include_directories: ext_inc_dirs,
    dependencies: alloc_lib_deps,
    c_args: alloc_lib_c_args,
    install: true,
    install_dir: get_option('libdir'),
  )
endif


