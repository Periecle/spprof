"""Pytest configuration and fixtures for spprof tests."""

import contextlib

import pytest


@pytest.fixture(autouse=True)
def ensure_profiler_stopped():
    """Ensure profiler is stopped before and after each test.

    This prevents cascade failures when a test leaves the profiler running.
    """
    import spprof

    # Stop profiler before test if it's running
    if spprof.is_active():
        with contextlib.suppress(Exception):
            spprof.stop()

    yield

    # Stop profiler after test if it's still running
    if spprof.is_active():
        with contextlib.suppress(Exception):
            spprof.stop()
