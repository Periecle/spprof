"""
Build configuration for spprof C extension.

This builds the native extension with internal API access for async-signal-safe
frame walking. The internal API is used exclusively on all supported Python
versions (3.9-3.14).

Requirements:
  - Python 3.9+ (internal API supports 3.9-3.14)
  - C11 compiler (gcc 4.9+, clang 3.3+, MSVC 2015+)
  - Linux: librt, libdl
  - macOS: Xcode command line tools
  - Windows: Visual Studio 2015+
"""

import os
import platform
import subprocess
import sys
from pathlib import Path

from setuptools import Extension, find_packages, setup
from setuptools.command.build_ext import build_ext


class SpProfBuildExt(build_ext):
    """
    Custom build_ext that:
    1. Detects Python version and sets appropriate flags
    2. Uses internal API mode (only mode supported)
    3. Falls back gracefully if build fails
    """

    def build_extensions(self):
        # Detect compiler and add appropriate flags
        compiler_type = self.compiler.compiler_type

        for ext in self.extensions:
            if compiler_type == "unix":
                # GCC/Clang flags
                ext.extra_compile_args.extend(
                    [
                        "-Wno-unused-function",
                        "-Wno-missing-field-initializers",
                    ]
                )

                # Enable debug symbols in debug mode
                if os.environ.get("SPPROF_DEBUG"):
                    ext.extra_compile_args.extend(["-g", "-O0"])
                    ext.define_macros.append(("SPPROF_DEBUG", "1"))

            elif compiler_type == "msvc":
                # MSVC flags
                if os.environ.get("SPPROF_DEBUG"):
                    ext.extra_compile_args.extend(["/Zi", "/Od"])
                    ext.define_macros.append(("SPPROF_DEBUG", "1"))

        try:
            super().build_extensions()
            print(
                f"\n[spprof] Successfully built C extension for Python {sys.version_info.major}.{sys.version_info.minor}"
            )
            print("[spprof] Using internal API for async-signal-safe frame walking")
        except Exception as e:
            print(f"\n[spprof] WARNING: Failed to build C extension: {e}")
            print("[spprof] The package will work with reduced functionality.")
            self.extensions = []


def get_python_version_defines():
    """Get version-specific preprocessor defines."""
    defines = []

    # Python version info
    major = sys.version_info.major
    minor = sys.version_info.minor
    micro = sys.version_info.micro

    defines.append(("SPPROF_PY_MAJOR", str(major)))
    defines.append(("SPPROF_PY_MINOR", str(minor)))
    defines.append(("SPPROF_PY_MICRO", str(micro)))

    return defines


def get_extension():
    """
    Get the extension module configuration.
    """
    # Platform detection
    IS_WINDOWS = platform.system() == "Windows"
    IS_MACOS = platform.system() == "Darwin"
    IS_LINUX = platform.system() == "Linux"

    # Check minimum Python version (3.9+)
    if sys.version_info < (3, 9):
        print("[spprof] ERROR: Python 3.9+ required")
        return None

    # Source directory
    SRC_DIR = Path("src/spprof/_ext")

    if not SRC_DIR.exists():
        print(f"[spprof] Source directory not found: {SRC_DIR}")
        return None

    # Core sources (always included)
    SOURCES = [
        str(SRC_DIR / "module.c"),
        str(SRC_DIR / "ringbuffer.c"),
        str(SRC_DIR / "resolver.c"),
        str(SRC_DIR / "unwind.c"),
        str(SRC_DIR / "code_registry.c"),  # Safe code object reference tracking
    ]

    # Add unified framewalker source (handles both internal and public API via #ifdef)
    SOURCES.append(str(SRC_DIR / "framewalker.c"))

    # Add signal handler
    signal_handler = SRC_DIR / "signal_handler.c"
    if signal_handler.exists():
        SOURCES.append(str(signal_handler))

    # Platform-specific sources
    if IS_LINUX:
        platform_src = SRC_DIR / "platform" / "linux.c"
        if platform_src.exists():
            SOURCES.append(str(platform_src))
        else:
            print(f"[spprof] Platform source not found: {platform_src}")
    elif IS_MACOS:
        platform_src = SRC_DIR / "platform" / "darwin.c"
        if platform_src.exists():
            SOURCES.append(str(platform_src))
        else:
            print(f"[spprof] Platform source not found: {platform_src}")
        
        # Add Mach-based sampler for Darwin
        mach_src = SRC_DIR / "platform" / "darwin_mach.c"
        if mach_src.exists():
            SOURCES.append(str(mach_src))
            print("[spprof] Darwin: Using Mach-based sampler for multi-thread support")
        else:
            print(f"[spprof] Mach sampler source not found: {mach_src}")
    elif IS_WINDOWS:
        platform_src = SRC_DIR / "platform" / "windows.c"
        if platform_src.exists():
            SOURCES.append(str(platform_src))
        else:
            print(f"[spprof] Platform source not found: {platform_src}")
    else:
        print(f"[spprof] Unsupported platform: {platform.system()}")
        return None

    # Verify all sources exist
    for src in SOURCES:
        if not Path(src).exists():
            print(f"[spprof] WARNING: Source file missing: {src}")

    # Include directories
    INCLUDE_DIRS = [
        str(SRC_DIR),
        str(SRC_DIR / "platform"),
        str(SRC_DIR / "internal"),
    ]

    # Compiler flags
    EXTRA_COMPILE_ARGS = []
    EXTRA_LINK_ARGS = []
    DEFINE_MACROS = []

    # Add version defines
    DEFINE_MACROS.extend(get_python_version_defines())

    print(
        f"[spprof] Building with internal API for Python {sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}"
    )

    if IS_WINDOWS:
        EXTRA_COMPILE_ARGS = [
            "/O2",
            "/W3",
            "/D_CRT_SECURE_NO_WARNINGS",
            "/std:c11",  # Required for C11 features
        ]
        # Link against required Windows libraries
        # dbghelp.lib is for symbol resolution in native unwinding
        EXTRA_LINK_ARGS = ["dbghelp.lib"]
    else:
        EXTRA_COMPILE_ARGS = [
            "-O2",
            "-Wall",
            "-Wextra",
            "-Wno-unused-parameter",
            "-std=c11",
            "-fvisibility=hidden",  # Hide internal symbols
        ]

        # Position-independent code for shared library
        EXTRA_COMPILE_ARGS.append("-fPIC")

        # Initialize link args for POSIX platforms
        EXTRA_LINK_ARGS = []

    if IS_LINUX:
        EXTRA_LINK_ARGS.extend(["-lrt", "-ldl", "-lpthread"])

        # Check for libunwind
        try:
            result = subprocess.run(
                ["pkg-config", "--exists", "libunwind"], capture_output=True, timeout=5
            )
            if result.returncode == 0:
                EXTRA_LINK_ARGS.append("-lunwind")
                DEFINE_MACROS.append(("SPPROF_HAS_LIBUNWIND", "1"))
                print("[spprof] Found libunwind - enabling advanced unwinding")
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass

    elif IS_MACOS:
        EXTRA_LINK_ARGS.extend(["-framework", "CoreFoundation"])
        EXTRA_COMPILE_ARGS.extend(
            [
                "-mmacosx-version-min=10.15",
            ]
        )

    # Build the extension
    return Extension(
        "spprof._native",
        sources=SOURCES,
        include_dirs=INCLUDE_DIRS,
        extra_compile_args=EXTRA_COMPILE_ARGS,
        extra_link_args=EXTRA_LINK_ARGS,
        define_macros=DEFINE_MACROS,
        language="c",
    )


# Get extension (may be None if platform unsupported)
ext_modules = []
ext = get_extension()
if ext is not None:
    ext_modules.append(ext)

setup(
    name="spprof",
    version="0.1.0",
    description="High-performance sampling profiler for Python",
    long_description=Path("README.md").read_text() if Path("README.md").exists() else "",
    long_description_content_type="text/markdown",
    author="spprof contributors",
    license="MIT",
    python_requires=">=3.9",
    packages=find_packages(where="src"),
    package_dir={"": "src"},
    package_data={
        "spprof": ["py.typed", "_profiler.pyi"],
    },
    ext_modules=ext_modules,
    cmdclass={"build_ext": SpProfBuildExt},
    classifiers=[
        "Development Status :: 4 - Beta",
        "Intended Audience :: Developers",
        "License :: OSI Approved :: MIT License",
        "Operating System :: POSIX :: Linux",
        "Operating System :: MacOS :: MacOS X",
        "Operating System :: Microsoft :: Windows",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
        "Programming Language :: Python :: Implementation :: CPython",
        "Topic :: Software Development :: Debuggers",
        "Topic :: System :: Monitoring",
    ],
    keywords="profiler, sampling, performance, flame-graph, speedscope",
    project_urls={
        "Documentation": "https://github.com/spprof/spprof",
        "Source": "https://github.com/spprof/spprof",
        "Tracker": "https://github.com/spprof/spprof/issues",
    },
)
