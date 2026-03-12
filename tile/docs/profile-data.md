# Profile Data and Output

The `Profile` and `AggregatedProfile` types hold profiling results. Both support export to Speedscope JSON format and collapsed stacks (FlameGraph format).

## Capabilities

### Profile

Returned by `spprof.stop()`. Contains all raw samples collected during the profiling session.

```python { .api }
from dataclasses import dataclass
from datetime import datetime
from typing import Literal, Any
from pathlib import Path

@dataclass
class Profile:
    # Fields set on creation
    start_time: datetime      # When profiling started
    end_time: datetime        # When profiling stopped
    interval_ms: int          # Sampling interval used
    samples: list[Sample]     # All collected samples
    dropped_count: int        # Samples lost due to buffer overflow
    python_version: str       # e.g. "3.11.5"
    platform: str             # e.g. "Linux-5.15.0-x86_64"

    @property
    def sample_count(self) -> int:
        """Number of samples collected (len(samples))."""

    @property
    def total_duration_ms(self) -> float:
        """Total profiling duration in milliseconds."""

    @property
    def effective_rate_hz(self) -> float:
        """Effective sampling rate in Hz (samples per second)."""

    def aggregate(self) -> AggregatedProfile:
        """
        Aggregate identical stacks to reduce memory usage.

        For long-running profiles with repetitive call patterns, many samples
        share the same call stack. This method groups identical stacks and
        stores only unique stacks with occurrence counts.

        Returns:
        - AggregatedProfile: Deduplicated profile with stack counts.
        """

    def to_speedscope(self) -> dict[str, Any]:
        """
        Convert to Speedscope JSON-compatible dict.

        Returns a dict conforming to the Speedscope file format schema.
        Each thread becomes a separate sampled profile in the output.
        Weights are set to interval_ms * 1,000,000 (nanoseconds) per sample.

        Returns:
        - dict: Ready for json.dumps(), loadable at speedscope.app.
        """

    def to_collapsed(self) -> str:
        """
        Convert to collapsed stack format string (FlameGraph compatible).

        Format: "frame1;frame2;...;frameN count" per line.
        Stack order is root-to-leaf (bottom of flame to top).
        Native frames are prefixed with "[native]".

        Returns:
        - str: Collapsed stack format string, one stack per line.
        """

    def save(
        self,
        path: str | Path,
        format: Literal["speedscope", "collapsed"] = "speedscope",
    ) -> None:
        """
        Save profile to file.

        Parameters:
        - path (str | Path): Output file path.
        - format (str): "speedscope" (default) or "collapsed".

        Raises:
        - ValueError: If format is not "speedscope" or "collapsed".
        """
```

### AggregatedProfile

Returned by `Profile.aggregate()`. Memory-efficient representation that stores unique stacks with occurrence counts instead of all individual samples. Useful for long-running profiles.

```python { .api }
@dataclass
class AggregatedProfile:
    # Fields set on creation
    start_time: datetime              # When original profiling started
    end_time: datetime                # When original profiling stopped
    interval_ms: int                  # Sampling interval used
    stacks: list[AggregatedStack]     # Unique stacks with counts
    total_samples: int                # Original sample count before aggregation
    dropped_count: int                # Samples lost due to buffer overflow
    python_version: str               # e.g. "3.11.5"
    platform: str                     # e.g. "Linux-5.15.0-x86_64"

    @property
    def unique_stack_count(self) -> int:
        """Number of unique stacks (len(stacks))."""

    @property
    def compression_ratio(self) -> float:
        """
        Ratio of total_samples to unique_stack_count.
        Higher values indicate more repetition in the profile.
        Returns 1.0 if no stacks.
        """

    @property
    def memory_reduction_pct(self) -> float:
        """
        Estimated memory reduction percentage vs. raw Profile.
        0.0 if total_samples is 0.
        """

    def to_speedscope(self) -> dict[str, Any]:
        """
        Convert to Speedscope JSON-compatible dict.

        Expands aggregated stacks back into individual samples for
        visualization compatibility. Each count becomes count individual samples.

        Returns:
        - dict: Ready for json.dumps(), loadable at speedscope.app.
        """

    def to_collapsed(self) -> str:
        """
        Convert to collapsed stack format string.

        Most efficient conversion: aggregated stacks map directly to
        collapsed format's "stack;count" structure. Lines are sorted.

        Returns:
        - str: Collapsed stack format string, one stack per line.
        """

    def save(
        self,
        path: str | Path,
        format: Literal["speedscope", "collapsed"] = "speedscope",
    ) -> None:
        """
        Save aggregated profile to file.

        Parameters:
        - path (str | Path): Output file path.
        - format (str): "speedscope" (default) or "collapsed".

        Raises:
        - ValueError: If format is not "speedscope" or "collapsed".
        """
```

## Output Formats

### Speedscope Format (default)

The default output format. Opens directly in [speedscope.app](https://www.speedscope.app) for interactive flame graph visualization.

```python
# Save as Speedscope (default)
profile.save("profile.json")

# Or explicitly:
profile.save("profile.json", format="speedscope")

# Get the dict directly
data = profile.to_speedscope()
import json
json_str = json.dumps(data, indent=2)
```

The Speedscope JSON dict structure:
```python
{
    "$schema": "https://www.speedscope.app/file-format-schema.json",
    "version": "1.0.0",
    "shared": {"frames": [{"name": str, "file": str, "line": int}, ...]},
    "profiles": [  # One per thread
        {
            "type": "sampled",
            "name": str,           # Thread name
            "unit": "nanoseconds",
            "startValue": int,
            "endValue": int,
            "samples": [[int, ...], ...],  # Frame index lists
            "weights": [int, ...],
        }
    ],
    "name": str,
    "exporter": str,
}
```

### Collapsed Stack Format (FlameGraph)

Compatible with Brendan Gregg's FlameGraph tools. Format: `frame1;frame2;...;frameN count` per line. Stack order is root-to-leaf.

```python
# Save as collapsed stacks
profile.save("profile.collapsed", format="collapsed")

# Or get the string directly
collapsed = profile.to_collapsed()
```

Use with FlameGraph:
```bash
flamegraph.pl profile.collapsed > profile.svg
```

## Low-Level Output Functions (spprof.output module)

These functions power the Profile and AggregatedProfile methods. Accessible directly from `spprof.output` if needed.

```python { .api }
from spprof.output import to_speedscope, to_collapsed, aggregated_to_speedscope, aggregated_to_collapsed

def to_speedscope(profile: Profile) -> dict[str, Any]:
    """Convert Profile to Speedscope JSON dict."""

def to_collapsed(profile: Profile, mark_native: bool = True) -> str:
    """
    Convert Profile to collapsed stack format string.

    Parameters:
    - profile (Profile): Profile to convert.
    - mark_native (bool): If True, prefix native frames with "[native]". Default True.
    """

def aggregated_to_speedscope(profile: AggregatedProfile) -> dict[str, Any]:
    """Convert AggregatedProfile to Speedscope JSON dict."""

def aggregated_to_collapsed(profile: AggregatedProfile, mark_native: bool = True) -> str:
    """
    Convert AggregatedProfile to collapsed stack format string.

    Parameters:
    - profile (AggregatedProfile): AggregatedProfile to convert.
    - mark_native (bool): If True, prefix native frames with "[native]". Default True.
    """
```

## Usage Examples

### Basic Save and Load

```python
import spprof

spprof.start()
run_workload()
profile = spprof.stop()

# Check collection stats
print(f"Samples: {profile.sample_count}")
print(f"Duration: {profile.total_duration_ms:.1f}ms")
print(f"Rate: {profile.effective_rate_hz:.1f}Hz")
print(f"Dropped: {profile.dropped_count}")

# Save for Speedscope viewer
profile.save("profile.json")

# Save for FlameGraph
profile.save("profile.collapsed", format="collapsed")
```

### Memory-Efficient Aggregation

```python
import spprof

spprof.start(interval_ms=10)
long_running_task()  # Runs for minutes
profile = spprof.stop()

# Aggregate to reduce memory
agg = profile.aggregate()
print(f"Reduced {profile.sample_count} samples → {agg.unique_stack_count} unique stacks")
print(f"Compression: {agg.compression_ratio:.1f}x")
print(f"Memory reduction: {agg.memory_reduction_pct:.1f}%")

# Save aggregated profile (same file formats)
agg.save("profile.json")
agg.save("profile.collapsed", format="collapsed")
```

### Handling High Dropped Sample Count

```python
import spprof

# If profile.dropped_count is high, increase memory_limit_mb
spprof.start(interval_ms=10, memory_limit_mb=500)
run_workload()
profile = spprof.stop()

if profile.dropped_count > 0:
    print(f"Warning: {profile.dropped_count} samples dropped")
```
