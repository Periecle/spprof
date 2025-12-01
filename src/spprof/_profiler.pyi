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

# --- Safe Mode Functions ---

def _set_safe_mode(enabled: bool) -> None:
    """Enable or disable safe mode for code validation.

    When safe mode is enabled, code objects captured without holding a
    reference (signal-handler samples on Linux) will be discarded rather
    than validated via PyCode_Check. This trades sample completeness for
    guaranteed memory safety.

    Note: Darwin/Mach samples are always safe (INCREF'd during capture).
    Safe mode only affects Linux signal-handler samples.
    """
    ...

def _is_safe_mode() -> bool:
    """Check if safe mode is enabled."""
    ...

def _get_code_registry_stats() -> dict[str, Any]:
    """Get code registry statistics including safe mode rejects.

    Returns a dict with:
        - refs_held: Number of references currently held
        - refs_added: Total references added
        - refs_released: Total references released
        - validations: Total validation calls
        - invalid_count: Validations that returned invalid
        - safe_mode_rejects: Samples discarded due to safe mode
        - safe_mode_enabled: Whether safe mode is enabled
    """
    ...

# --- Module Constants ---

__version__: str
platform: str
frame_walker: str
unwind_method: str
native_unwinding_available: int
