"""Type stubs for spprof._native C extension (internal)."""

from typing import Any

# --- Internal C Extension Functions ---
# These are implementation details; use spprof.* public API instead.

def _start(interval_ns: int) -> None:
    """Start profiling (internal). Use spprof.start() instead."""
    ...

def _stop() -> list[dict[str, Any]]:
    """Stop profiling and return raw samples (internal)."""
    ...

def _is_active() -> bool:
    """Check if profiling is active."""
    ...

def _get_stats() -> dict[str, Any]:
    """Get current profiling statistics."""
    ...

def _register_thread() -> bool:
    """Register current thread for per-thread sampling (Linux). Returns True on success."""
    ...

def _unregister_thread() -> bool:
    """Unregister current thread from sampling. Returns True on success."""
    ...

# --- Native Unwinding Functions ---

def _set_native_unwinding(enabled: bool) -> None:
    """Enable or disable native C-stack unwinding."""
    ...

def _native_unwinding_available() -> bool:
    """Check if native unwinding is available on this platform."""
    ...

def _native_unwinding_enabled() -> bool:
    """Check if native unwinding is currently enabled."""
    ...

def _capture_native_stack() -> list[dict[str, Any]]:
    """Capture current native stack (for testing)."""
    ...

# --- Module Constants ---

__version__: str
platform: str
frame_walker: str
unwind_method: str
native_unwinding_available: int
