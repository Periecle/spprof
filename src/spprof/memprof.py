"""
spprof.memprof - Memory Allocation Profiler

Production-grade memory profiling using Poisson sampling with native
allocator interposition. Provides statistically accurate heap profiling
with ultra-low overhead (<0.1% at default sampling rate).

Example:
    >>> import spprof.memprof as memprof
    >>> memprof.start()
    >>> # ... your workload ...
    >>> snapshot = memprof.get_snapshot()
    >>> print(f"Estimated heap: {snapshot.estimated_heap_bytes / 1e6:.1f} MB")
    >>> memprof.stop()
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Union
import json


# ============================================================================
# Data Classes
# ============================================================================

@dataclass
class StackFrame:
    """A frame in the allocation call stack."""
    address: int
    function: str
    file: str
    line: int
    is_python: bool = False
    
    def __str__(self) -> str:
        if self.line > 0:
            return f"{self.function} ({self.file}:{self.line})"
        return f"{self.function} ({self.file})"


@dataclass
class AllocationSample:
    """A single sampled allocation."""
    address: int
    size: int
    weight: int
    estimated_bytes: int
    timestamp_ns: int
    lifetime_ns: Optional[int] = None
    stack: List[StackFrame] = field(default_factory=list)
    gc_epoch: int = 0
    
    @property
    def is_live(self) -> bool:
        """True if allocation has not been freed."""
        return self.lifetime_ns is None


@dataclass
class FramePointerHealth:
    """
    Metrics for assessing native stack capture quality.
    
    Use this to detect if C extensions are missing frame pointers,
    which results in truncated stack traces.
    """
    shallow_stack_warnings: int
    total_native_stacks: int
    avg_native_depth: float
    min_native_depth: int
    
    @property
    def truncation_rate(self) -> float:
        """Percentage of stacks that were truncated."""
        if self.total_native_stacks == 0:
            return 0.0
        return self.shallow_stack_warnings / self.total_native_stacks
    
    @property
    def confidence(self) -> str:
        """
        Human-readable confidence level for profile data.
        
        Returns:
            'high': <5% truncation, good frame pointer coverage
            'medium': 5-20% truncation, some extensions missing FP
            'low': >20% truncation, many extensions missing FP
        """
        rate = self.truncation_rate
        if rate < 0.05:
            return 'high'
        elif rate < 0.20:
            return 'medium'
        else:
            return 'low'
    
    @property
    def recommendation(self) -> Optional[str]:
        """Action recommendation if confidence is not high."""
        if self.confidence == 'high':
            return None
        return (
            f"Stack truncation rate is {self.truncation_rate:.1%}. "
            f"For better visibility, rebuild C extensions with: "
            f"CFLAGS='-fno-omit-frame-pointer' pip install --no-binary :all: <package>"
        )


@dataclass
class MemProfStats:
    """Profiler statistics."""
    total_samples: int
    live_samples: int
    freed_samples: int
    unique_stacks: int
    estimated_heap_bytes: int
    heap_map_load_percent: float
    collisions: int
    sampling_rate_bytes: int
    shallow_stack_warnings: int = 0
    death_during_birth: int = 0
    zombie_races_detected: int = 0


@dataclass
class HeapSnapshot:
    """Snapshot of live (unfreed) sampled allocations."""
    samples: List[AllocationSample]
    total_samples: int
    live_samples: int
    estimated_heap_bytes: int
    timestamp_ns: int
    frame_pointer_health: FramePointerHealth
    
    def top_allocators(self, n: int = 10) -> List[Dict[str, Any]]:
        """
        Get top N allocation sites by estimated bytes.
        
        Returns list of dicts with keys:
        - function: str
        - file: str  
        - line: int
        - estimated_bytes: int
        - sample_count: int
        """
        # Group by top stack frame
        sites: Dict[str, Dict[str, Any]] = {}
        
        for sample in self.samples:
            if not sample.stack:
                continue
            
            # Use top frame as key
            top = sample.stack[0]
            key = f"{top.function}:{top.file}:{top.line}"
            
            if key not in sites:
                sites[key] = {
                    'function': top.function,
                    'file': top.file,
                    'line': top.line,
                    'estimated_bytes': 0,
                    'sample_count': 0,
                }
            
            sites[key]['estimated_bytes'] += sample.weight
            sites[key]['sample_count'] += 1
        
        # Sort by estimated bytes
        sorted_sites = sorted(
            sites.values(),
            key=lambda x: x['estimated_bytes'],
            reverse=True
        )
        
        return sorted_sites[:n]
    
    def save(self, path: Union[str, Path], format: str = "speedscope") -> None:
        """
        Save snapshot to file.
        
        Args:
            path: Output file path
            format: 'speedscope' (default) or 'collapsed'
        """
        path = Path(path)
        
        if format == "speedscope":
            self._save_speedscope(path)
        elif format == "collapsed":
            self._save_collapsed(path)
        else:
            raise ValueError(f"Unknown format: {format}")
    
    def _save_speedscope(self, path: Path) -> None:
        """Save in Speedscope JSON format."""
        # Build frame index
        frames: List[Dict[str, Any]] = []
        frame_index: Dict[str, int] = {}
        
        for sample in self.samples:
            for frame in sample.stack:
                key = f"{frame.function}:{frame.file}:{frame.line}"
                if key not in frame_index:
                    frame_index[key] = len(frames)
                    frames.append({
                        'name': frame.function,
                        'file': frame.file,
                        'line': frame.line,
                    })
        
        # Build samples
        sample_data = []
        weights = []
        
        for sample in self.samples:
            stack_indices = []
            for frame in reversed(sample.stack):  # Root to leaf
                key = f"{frame.function}:{frame.file}:{frame.line}"
                if key in frame_index:
                    stack_indices.append(frame_index[key])
            
            if stack_indices:
                sample_data.append(stack_indices)
                weights.append(sample.weight)
        
        # Create Speedscope JSON
        data = {
            '$schema': 'https://www.speedscope.app/file-format-schema.json',
            'version': '0.0.1',
            'shared': {
                'frames': frames,
            },
            'profiles': [{
                'type': 'sampled',
                'name': 'Memory Profile',
                'unit': 'bytes',
                'startValue': 0,
                'endValue': self.estimated_heap_bytes,
                'samples': sample_data,
                'weights': weights,
            }],
        }
        
        with open(path, 'w') as f:
            json.dump(data, f, indent=2)
    
    def _save_collapsed(self, path: Path) -> None:
        """Save in collapsed stack format (for FlameGraph)."""
        lines = []
        
        for sample in self.samples:
            if not sample.stack:
                continue
            
            # Build stack string (root to leaf, semicolon-separated)
            stack_str = ';'.join(
                frame.function
                for frame in reversed(sample.stack)
            )
            
            lines.append(f"{stack_str} {sample.weight}")
        
        with open(path, 'w') as f:
            f.write('\n'.join(lines))


# ============================================================================
# Module State
# ============================================================================

_initialized = False
_running = False
_shutdown = False


# ============================================================================
# Core API
# ============================================================================

def start(sampling_rate_kb: int = 512) -> None:
    """
    Start memory profiling.
    
    Args:
        sampling_rate_kb: Average KB between samples. Lower = more accuracy,
                         higher overhead. Default 512 KB gives <0.1% overhead.
    
    Raises:
        RuntimeError: If memory profiler is already running.
        RuntimeError: If interposition hooks could not be installed.
        ValueError: If sampling_rate_kb < 1.
    """
    global _initialized, _running, _shutdown
    
    if _shutdown:
        raise RuntimeError("Cannot restart after shutdown")
    
    if _running:
        raise RuntimeError("Memory profiler is already running")
    
    if sampling_rate_kb < 1:
        raise ValueError("sampling_rate_kb must be >= 1")
    
    sampling_rate_bytes = sampling_rate_kb * 1024
    
    try:
        from . import _native
        
        if not _initialized:
            result = _native._memprof_init(sampling_rate_bytes)
            if result != 0:
                raise RuntimeError("Failed to initialize memory profiler")
            _initialized = True
        
        result = _native._memprof_start()
        if result != 0:
            raise RuntimeError("Failed to start memory profiler")
        
        _running = True
        
    except ImportError:
        raise RuntimeError(
            "spprof native extension not available. "
            "Ensure spprof is properly installed."
        )


def stop() -> None:
    """
    Stop memory profiling.
    
    Important:
        - Stops tracking NEW allocations (malloc sampling disabled)
        - CONTINUES tracking frees (free lookup remains active)
        - This prevents "fake leaks" where objects allocated during profiling
          but freed after stop() would incorrectly appear as live
    
    This function is idempotent - calling it multiple times is safe.
    
    Raises:
        RuntimeError: If memory profiler is not running (strict mode only).
    """
    global _running
    
    # Idempotent: if already stopped, just return
    if not _running:
        return
    
    from . import _native
    
    # Native stop is also idempotent and always succeeds
    _native._memprof_stop()
    
    _running = False


def get_snapshot() -> HeapSnapshot:
    """
    Get snapshot of currently live (unfreed) sampled allocations.
    
    Can be called while profiling is active or after stop().
    
    Returns:
        HeapSnapshot containing all live sampled allocations.
    
    Raises:
        RuntimeError: If profiler is not initialized or snapshot fails.
    """
    global _initialized
    
    if not _initialized:
        raise RuntimeError("Memory profiler is not initialized")
    
    from . import _native
    import time
    
    # Get raw snapshot data from native extension
    raw_data = _native._memprof_get_snapshot()
    
    if not raw_data or not isinstance(raw_data, dict):
        raise RuntimeError("Failed to retrieve memory snapshot")
    
    # Parse into AllocationSample objects
    samples = []
    for entry in raw_data.get('entries', []):
        stack_frames = []
        for frame_data in entry.get('stack', []):
            stack_frames.append(StackFrame(
                address=frame_data.get('address', 0),
                function=frame_data.get('function', '<unknown>'),
                file=frame_data.get('file', '<unknown>'),
                line=frame_data.get('line', 0),
                is_python=frame_data.get('is_python', False),
            ))
        
        samples.append(AllocationSample(
            address=entry.get('address', 0),
            size=entry.get('size', 0),
            weight=entry.get('weight', 0),
            estimated_bytes=entry.get('weight', 0),  # Weight IS the estimate
            timestamp_ns=entry.get('timestamp_ns', 0),
            lifetime_ns=entry.get('lifetime_ns'),
            stack=stack_frames,
        ))
    
    # Get frame pointer health
    fp_health = raw_data.get('frame_pointer_health', {})
    frame_pointer_health = FramePointerHealth(
        shallow_stack_warnings=fp_health.get('shallow_stack_warnings', 0),
        total_native_stacks=fp_health.get('total_native_stacks', 0),
        avg_native_depth=fp_health.get('avg_native_depth', 0.0),
        min_native_depth=fp_health.get('min_native_depth', 0),
    )
    
    # Calculate totals
    live_samples = [s for s in samples if s.is_live]
    estimated_heap = sum(s.weight for s in live_samples)
    
    return HeapSnapshot(
        samples=live_samples,
        total_samples=raw_data.get('total_samples', len(samples)),
        live_samples=len(live_samples),
        estimated_heap_bytes=estimated_heap,
        timestamp_ns=int(time.time_ns()),
        frame_pointer_health=frame_pointer_health,
    )


def get_stats() -> MemProfStats:
    """
    Get profiler statistics.
    
    Returns:
        MemProfStats with current profiler state.
    
    Raises:
        RuntimeError: If profiler is not initialized.
    """
    from . import _native
    
    raw_stats = _native._memprof_get_stats()
    
    if raw_stats is None:
        raise RuntimeError("Memory profiler is not initialized")
    
    return MemProfStats(
        total_samples=raw_stats.get('total_samples', 0),
        live_samples=raw_stats.get('live_samples', 0),
        freed_samples=raw_stats.get('freed_samples', 0),
        unique_stacks=raw_stats.get('unique_stacks', 0),
        estimated_heap_bytes=raw_stats.get('estimated_heap_bytes', 0),
        heap_map_load_percent=raw_stats.get('heap_map_load_percent', 0.0),
        collisions=raw_stats.get('collisions', 0),
        sampling_rate_bytes=raw_stats.get('sampling_rate_bytes', 0),
        shallow_stack_warnings=raw_stats.get('shallow_stack_warnings', 0),
        death_during_birth=raw_stats.get('death_during_birth', 0),
        zombie_races_detected=raw_stats.get('zombie_races_detected', 0),
    )


def shutdown() -> None:
    """
    Shutdown profiler and prepare for process exit.
    
    ⚠️ WARNING: This is a ONE-WAY operation.
    
    - Disables all hooks (no more sampling or free tracking)
    - Does NOT free internal memory (intentional, prevents crashes)
    - Should only be called at process exit or before unloading the module
    
    After shutdown(), calling start() again raises RuntimeError.
    """
    global _initialized, _running, _shutdown
    
    if _shutdown:
        return  # Idempotent
    
    from . import _native
    
    _native._memprof_shutdown()
    
    _initialized = False
    _running = False
    _shutdown = True


# ============================================================================
# Context Manager
# ============================================================================

class MemoryProfiler:
    """
    Context manager for memory profiling.
    
    Example:
        >>> with MemoryProfiler(sampling_rate_kb=512) as mp:
        ...     # ... run workload ...
        >>> mp.snapshot.save("memory_profile.json")
    """
    
    def __init__(self, sampling_rate_kb: int = 512):
        self._sampling_rate_kb = sampling_rate_kb
        self._snapshot: Optional[HeapSnapshot] = None
    
    def __enter__(self) -> 'MemoryProfiler':
        start(sampling_rate_kb=self._sampling_rate_kb)
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self._snapshot = get_snapshot()
        stop()
    
    @property
    def snapshot(self) -> Optional[HeapSnapshot]:
        """Get the captured snapshot (available after context exit)."""
        return self._snapshot


# ============================================================================
# Module Exports
# ============================================================================

__all__ = [
    # Core API
    'start',
    'stop',
    'get_snapshot',
    'get_stats',
    'shutdown',
    
    # Context Manager
    'MemoryProfiler',
    
    # Data Classes
    'AllocationSample',
    'StackFrame',
    'HeapSnapshot',
    'FramePointerHealth',
    'MemProfStats',
]

