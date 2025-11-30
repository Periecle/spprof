"""Tests for output format generation."""

import json
import tempfile
from datetime import datetime
from pathlib import Path


def test_speedscope_format():
    """Verify output matches Speedscope JSON schema."""
    from spprof import Frame, Profile, Sample

    # Create a sample profile
    frames = [
        Frame(function_name="main", filename="app.py", lineno=10),
        Frame(function_name="process", filename="worker.py", lineno=42),
    ]
    sample = Sample(
        timestamp_ns=1000000000,
        thread_id=12345,
        thread_name="MainThread",
        frames=frames,
    )

    profile = Profile(
        start_time=datetime.now(),
        end_time=datetime.now(),
        interval_ms=10,
        samples=[sample],
        dropped_count=0,
        python_version="3.12.0",
        platform="Linux-5.15-x86_64",
    )

    # Convert to Speedscope format
    speedscope = profile.to_speedscope()

    # Verify structure
    assert speedscope["$schema"] == "https://www.speedscope.app/file-format-schema.json"
    assert speedscope["version"] == "1.0.0"
    assert "shared" in speedscope
    assert "frames" in speedscope["shared"]
    assert "profiles" in speedscope
    assert len(speedscope["profiles"]) > 0
    assert speedscope["exporter"].startswith("spprof")


def test_collapsed_format():
    """Verify collapsed stack format output."""
    from spprof import Frame, Profile, Sample

    frames = [
        Frame(function_name="main", filename="app.py", lineno=10),
        Frame(function_name="process", filename="worker.py", lineno=42),
    ]
    sample = Sample(
        timestamp_ns=1000000000,
        thread_id=12345,
        thread_name="MainThread",
        frames=frames,
    )

    profile = Profile(
        start_time=datetime.now(),
        end_time=datetime.now(),
        interval_ms=10,
        samples=[sample, sample],  # Same stack twice
        dropped_count=0,
        python_version="3.12.0",
        platform="Linux-5.15-x86_64",
    )

    collapsed = profile.to_collapsed()

    # Should contain stack with count
    lines = collapsed.strip().split("\n")
    assert len(lines) == 1  # One unique stack
    assert "main" in lines[0]
    assert "process" in lines[0]
    assert lines[0].endswith(" 2")  # Count of 2


def test_profile_save_file():
    """Test saving profile to file."""
    from spprof import Frame, Profile, Sample

    frames = [
        Frame(function_name="test_func", filename="test.py", lineno=1),
    ]
    sample = Sample(
        timestamp_ns=1000000000,
        thread_id=1,
        thread_name="TestThread",
        frames=frames,
    )

    profile = Profile(
        start_time=datetime.now(),
        end_time=datetime.now(),
        interval_ms=10,
        samples=[sample],
        dropped_count=0,
        python_version="3.12.0",
        platform="TestPlatform",
    )

    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
        output_path = Path(f.name)

    try:
        profile.save(output_path)
        assert output_path.exists()

        # Verify JSON is valid
        content = json.loads(output_path.read_text())
        assert "$schema" in content
        assert "profiles" in content
    finally:
        output_path.unlink()


def test_save_collapsed_format():
    """Test saving profile in collapsed format."""
    from spprof import Frame, Profile, Sample

    frames = [
        Frame(function_name="test_func", filename="test.py", lineno=1),
    ]
    sample = Sample(
        timestamp_ns=1000000000,
        thread_id=1,
        thread_name=None,
        frames=frames,
    )

    profile = Profile(
        start_time=datetime.now(),
        end_time=datetime.now(),
        interval_ms=10,
        samples=[sample],
        dropped_count=0,
        python_version="3.12.0",
        platform="TestPlatform",
    )

    with tempfile.NamedTemporaryFile(suffix=".collapsed", delete=False) as f:
        output_path = Path(f.name)

    try:
        profile.save(output_path, format="collapsed")
        assert output_path.exists()

        content = output_path.read_text()
        assert "test_func" in content
        assert " 1" in content  # Count
    finally:
        output_path.unlink()


def test_zero_samples_valid_output():
    """Verify empty profile produces valid output."""
    from spprof import Profile

    profile = Profile(
        start_time=datetime.now(),
        end_time=datetime.now(),
        interval_ms=10,
        samples=[],
        dropped_count=0,
        python_version="3.12.0",
        platform="TestPlatform",
    )

    # Should not raise
    speedscope = profile.to_speedscope()
    collapsed = profile.to_collapsed()

    assert speedscope["profiles"] == []
    assert collapsed == ""


def test_multi_thread_output():
    """Verify output correctly handles multiple threads."""
    from spprof import Frame, Profile, Sample

    frame1 = Frame(function_name="worker1", filename="w1.py", lineno=1)
    frame2 = Frame(function_name="worker2", filename="w2.py", lineno=1)

    sample1 = Sample(
        timestamp_ns=1000000000,
        thread_id=100,
        thread_name="Thread-1",
        frames=[frame1],
    )
    sample2 = Sample(
        timestamp_ns=1000000000,
        thread_id=200,
        thread_name="Thread-2",
        frames=[frame2],
    )

    profile = Profile(
        start_time=datetime.now(),
        end_time=datetime.now(),
        interval_ms=10,
        samples=[sample1, sample2],
        dropped_count=0,
        python_version="3.12.0",
        platform="TestPlatform",
    )

    speedscope = profile.to_speedscope()

    # Should have separate profiles for each thread
    assert len(speedscope["profiles"]) == 2

    thread_names = {p["name"] for p in speedscope["profiles"]}
    assert "Thread-1" in thread_names
    assert "Thread-2" in thread_names
