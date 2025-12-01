# spprof

[![CI](https://github.com/Periecle/spprof/actions/workflows/ci.yml/badge.svg)](https://github.com/Periecle/spprof/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/Periecle/spprof/branch/main/graph/badge.svg)](https://codecov.io/gh/Periecle/spprof)
[![PyPI version](https://badge.fury.io/py/spprof.svg)](https://badge.fury.io/py/spprof)
[![Python 3.9–3.14](https://img.shields.io/pypi/pyversions/spprof.svg)](https://pypi.org/project/spprof/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

A high-performance sampling profiler for Python with [Speedscope](https://www.speedscope.app) and FlameGraph output.

## Features

- **Low overhead** — <1% CPU at 10ms sampling, suitable for production
- **Mixed-mode profiling** — Capture Python and C extension frames together
- **Multi-threaded** — Automatic profiling of all Python threads
- **Memory-efficient** — Stack aggregation for long-running profiles
- **Cross-platform** — Linux, macOS, Windows
- **Python 3.9–3.14** — Including free-threaded builds (macOS)
- **Zero dependencies** — No runtime requirements
- **Container-friendly** — No `SYS_PTRACE` capability required

## Installation

```bash
pip install spprof
```

Build from source:

```bash
pip install git+https://github.com/Periecle/spprof.git
```

## Quick Start

```python
import spprof

spprof.start()
# ... your code ...
profile = spprof.stop()
profile.save("profile.json")

# View at https://www.speedscope.app
```

### Context Manager

```python
with spprof.Profiler(interval_ms=5) as p:
    expensive_computation()

p.profile.save("profile.json")
```

### Decorator

```python
@spprof.profile(output_path="func.json")
def heavy_work():
    ...
```

## API

### Core Functions

| Function | Description |
|----------|-------------|
| `start(interval_ms=10)` | Begin profiling |
| `stop()` | Stop and return `Profile` |
| `is_active()` | Check if profiler is running |
| `stats()` | Get live statistics |

### Thread Management

```python
# Option 1: Explicit registration (recommended for Linux)
def worker():
    spprof.register_thread()
    try:
        do_work()
    finally:
        spprof.unregister_thread()

# Option 2: Context manager
def worker():
    with spprof.ThreadProfiler():
        do_work()
```

### Native Unwinding

Capture C/C++ frames alongside Python for debugging extensions:

```python
if spprof.native_unwinding_available():
    spprof.set_native_unwinding(True)

spprof.start()
```

### Memory-Efficient Aggregation

For long-running profiles with repetitive call patterns:

```python
profile = spprof.stop()
aggregated = profile.aggregate()

print(f"Compression: {aggregated.compression_ratio:.1f}x")
aggregated.save("profile.json")
```

## Output Formats

### Speedscope (default)

```python
profile.save("profile.json")
```

Open at [speedscope.app](https://www.speedscope.app) for interactive flame graphs.

### FlameGraph

```python
profile.save("profile.collapsed", format="collapsed")
```

Generate SVG with [FlameGraph](https://github.com/brendangregg/FlameGraph):

```bash
flamegraph.pl profile.collapsed > profile.svg
```

## Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| `interval_ms` | 10 | Sampling interval (1–1000ms) |
| `memory_limit_mb` | 100 | Maximum buffer size |
| `output_path` | None | Auto-save on stop |

### Interval Guidelines

| Interval | Overhead | Use Case |
|----------|----------|----------|
| 1ms | ~5% | Short scripts, benchmarks |
| 10ms | <1% | Development (default) |
| 100ms | <0.1% | Production monitoring |

## Platform Details

| Platform | Mechanism | Thread Sampling |
|----------|-----------|-----------------|
| Linux | `timer_create` + SIGPROF | Per-thread CPU time |
| macOS | Mach thread suspension | All threads automatic |
| Windows | Timer queue + GIL | All threads automatic |

**macOS**: Full support for free-threaded Python (`--disable-gil`) via Mach-based sampling.

**Linux**: Requires explicit thread registration for worker threads. Signal-based sampling is not free-threading safe.

## Development

```bash
git clone https://github.com/Periecle/spprof.git
cd spprof
pip install -e ".[dev]"
```

### Testing

```bash
pytest                           # Run tests
pytest --cov=spprof              # With coverage
```

### Code Quality

```bash
ruff check src/ tests/           # Lint
ruff format src/ tests/          # Format
mypy src/spprof                  # Type check
```

## Documentation

- [Usage Guide](docs/USAGE.md) — Detailed API documentation
- [Architecture](docs/ARCHITECTURE.md) — Internal design
- [Performance Tuning](docs/PERFORMANCE_TUNING.md) — Optimization guide

## License

MIT License. See [LICENSE](LICENSE) for details.
