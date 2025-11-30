# spprof

[![CI](https://github.com/Periecle/spprof/actions/workflows/ci.yml/badge.svg)](https://github.com/Periecle/spprof/actions/workflows/ci.yml)
[![codecov](https://codecov.io/gh/Periecle/spprof/branch/main/graph/badge.svg)](https://codecov.io/gh/Periecle/spprof)
[![PyPI version](https://badge.fury.io/py/spprof.svg)](https://badge.fury.io/py/spprof)
[![Python](https://img.shields.io/pypi/pyversions/spprof.svg)](https://pypi.org/project/spprof/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Ruff](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/astral-sh/ruff/main/assets/badge/v2.json)](https://github.com/astral-sh/ruff)
[![Types: MyPy](https://img.shields.io/badge/types-mypy-blue.svg)](https://mypy-lang.org/)

High-performance sampling profiler for Python applications with Speedscope output.

## Features

- **Low overhead**: < 1% CPU overhead at 10ms sampling interval
- **Cross-platform**: Linux, macOS, Windows support
- **Python 3.9–3.14**: Supports all modern Python versions including free-threaded builds
- **Speedscope output**: Generate flame graphs viewable at [speedscope.app](https://www.speedscope.app)
- **Container-friendly**: Works in restricted Kubernetes environments (no SYS_PTRACE required)
- **Multi-threaded**: Automatic profiling of all Python threads

## Installation

```bash
pip install spprof
```

Or from source:

```bash
git clone https://github.com/Periecle/spprof.git
cd spprof
pip install -e .
```

## Quick Start

### Basic Usage

```python
import spprof

# Start profiling
spprof.start(interval_ms=10)

# Run your workload
result = expensive_computation()

# Stop and save
profile = spprof.stop()
profile.save("profile.json")

# View at https://www.speedscope.app
```

### Context Manager

```python
import spprof

with spprof.Profiler(interval_ms=5) as p:
    result = expensive_computation()

p.profile.save("profile.json")
```

### Decorator

```python
import spprof

@spprof.profile(output_path="compute.json")
def heavy_computation():
    result = 0
    for i in range(10_000_000):
        result += i
    return result

heavy_computation()  # Profile saved automatically
```

## Multi-threaded Profiling

spprof automatically profiles all Python threads:

```python
import spprof
import threading

def worker(n):
    total = 0
    for i in range(n):
        total += i ** 2
    return total

spprof.start(interval_ms=5)

threads = [threading.Thread(target=worker, args=(1_000_000,)) for _ in range(4)]
for t in threads:
    t.start()
for t in threads:
    t.join()

profile = spprof.stop()
print(f"Collected {len(profile.samples)} samples")
profile.save("threaded_profile.json")
```

## Configuration

| Parameter | Default | Range | Description |
|-----------|---------|-------|-------------|
| `interval_ms` | 10 | 1–1000 | Sampling interval in milliseconds |
| `output_path` | None | - | Auto-save path (optional) |
| `memory_limit_mb` | 100 | 16–1024 | Maximum memory usage |

### Choosing Sample Interval

| Interval | Overhead | Use Case |
|----------|----------|----------|
| 1 ms | ~3-5% | Short-running scripts, high precision |
| 10 ms | < 1% | Production profiling (default) |
| 100 ms | < 0.1% | Long-running monitoring |

## Output Formats

### Speedscope (default)

```python
profile.save("profile.json")  # or format="speedscope"
```

Open at [speedscope.app](https://www.speedscope.app) for interactive flame graphs.

### FlameGraph (collapsed stacks)

```python
profile.save("profile.collapsed", format="collapsed")
```

Use with [FlameGraph](https://github.com/brendangregg/FlameGraph):

```bash
./flamegraph.pl profile.collapsed > profile.svg
```

## Platform Notes

| Platform | Mechanism | Notes |
|----------|-----------|-------|
| Linux | `timer_create` + SIGPROF | Full per-thread CPU sampling |
| macOS | `setitimer` + SIGPROF | Process-wide signal delivery |
| Windows | Timer + thread suspend | Higher overhead than Unix |

## Requirements

- Python 3.9 or later
- C compiler for extension (gcc, clang, MSVC)
- No runtime dependencies

## Development

### Setup

```bash
git clone https://github.com/Periecle/spprof.git
cd spprof
pip install -e ".[dev]"
pre-commit install
```

### Running Tests

```bash
# Run all tests
pytest

# Run with coverage
pytest --cov=spprof --cov-report=html

# Run specific test file
pytest tests/test_profiler.py -v
```

### Code Quality

```bash
# Lint and format check
ruff check src/ tests/
ruff format --check src/ tests/

# Auto-fix issues
ruff check --fix src/ tests/
ruff format src/ tests/

# Type checking
mypy src/spprof
```

### Pre-commit Hooks

Pre-commit hooks run automatically on `git commit`:

```bash
# Install hooks (one-time)
pre-commit install

# Run manually on all files
pre-commit run --all-files
```

## License

MIT License. See [LICENSE](LICENSE) for details.

