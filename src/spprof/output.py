"""
Output formatters for spprof profiles.

Supports:
- Speedscope JSON format (default)
- Collapsed stack format (for FlameGraph)
"""

from __future__ import annotations

from collections import defaultdict
from typing import TYPE_CHECKING, Any


if TYPE_CHECKING:
    from spprof import Profile


def to_speedscope(profile: Profile) -> dict[str, Any]:
    """
    Convert profile to Speedscope JSON format.

    Speedscope format specification:
    https://www.speedscope.app/file-format-schema.json

    Args:
        profile: Profile object to convert.

    Returns:
        Dictionary ready for JSON serialization.
    """
    # Build unique frame list
    frame_map: dict[tuple[str, str, int], int] = {}
    frames: list[dict[str, Any]] = []

    def get_frame_index(name: str, file: str, line: int) -> int:
        key = (name, file, line)
        if key not in frame_map:
            idx = len(frames)
            frame_map[key] = idx
            frames.append({"name": name, "file": file, "line": line})
        return frame_map[key]

    # Group samples by thread
    samples_by_thread: dict[int, list[Any]] = defaultdict(list)
    thread_names: dict[int, str] = {}

    for sample in profile.samples:
        samples_by_thread[sample.thread_id].append(sample)
        if sample.thread_name:
            thread_names[sample.thread_id] = sample.thread_name

    # Build profiles for each thread
    profiles: list[dict[str, Any]] = []

    for thread_id, thread_samples in samples_by_thread.items():
        if not thread_samples:
            continue

        thread_name = thread_names.get(thread_id, f"Thread-{thread_id}")

        # Convert samples
        speedscope_samples: list[list[int]] = []
        weights: list[int] = []

        start_time = thread_samples[0].timestamp_ns if thread_samples else 0

        for sample in thread_samples:
            # Build frame stack (bottom to top → indices)
            stack_indices = [
                get_frame_index(f.function_name, f.filename, f.lineno) for f in sample.frames
            ]
            speedscope_samples.append(stack_indices)
            # Weight is the interval in nanoseconds
            weights.append(profile.interval_ms * 1_000_000)

        end_time = thread_samples[-1].timestamp_ns if thread_samples else start_time

        thread_profile = {
            "type": "sampled",
            "name": thread_name,
            "unit": "nanoseconds",
            "startValue": 0,
            "endValue": end_time - start_time,
            "samples": speedscope_samples,
            "weights": weights,
        }
        profiles.append(thread_profile)

    return {
        "$schema": "https://www.speedscope.app/file-format-schema.json",
        "version": "1.0.0",
        "shared": {"frames": frames},
        "profiles": profiles,
        "name": "spprof profile",
        "exporter": f"spprof {_get_version()}",
    }


def to_collapsed(profile: Profile) -> str:
    """
    Convert profile to collapsed stack format.

    This format is compatible with Brendan Gregg's FlameGraph tools:
    https://github.com/brendangregg/FlameGraph

    Format: frame1;frame2;...;frameN count

    Args:
        profile: Profile object to convert.

    Returns:
        Collapsed stack format string.
    """
    # Count stack occurrences
    stack_counts: dict[str, int] = defaultdict(int)

    for sample in profile.samples:
        if not sample.frames:
            continue

        # Build stack string (bottom to top)
        stack_parts = []
        for frame in sample.frames:
            # Format: function (file:line)
            if frame.filename and frame.lineno:
                stack_parts.append(f"{frame.function_name} ({frame.filename}:{frame.lineno})")
            else:
                stack_parts.append(frame.function_name)

        stack_str = ";".join(stack_parts)
        stack_counts[stack_str] += 1

    # Build output
    lines = [f"{stack} {count}" for stack, count in sorted(stack_counts.items())]
    return "\n".join(lines)


def _get_version() -> str:
    """Get spprof version."""
    try:
        from spprof import __version__

        return __version__
    except ImportError:
        return "0.1.0"
