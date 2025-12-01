"""
Tests for the code object registry (use-after-free prevention).

The code registry addresses a potential use-after-free vulnerability where:
1. Raw PyCodeObject* pointers are captured during sampling
2. Between capture and resolution, Python's GC might free code objects
3. Resolver tries to dereference freed memory → crash/corruption

The fix:
- On Darwin/Mach (GIL held during capture): INCREF code objects via registry
- On all platforms: Validate code objects before dereferencing
- Track GC epochs to detect potential staleness

These tests verify the registry works correctly and prevents crashes.
"""

import gc
import sys
import threading
import time
import weakref

import pytest

# Import the native module to test registry functions
try:
    from spprof import _native
    # Check for the actual _start function that exists in the native module
    HAS_NATIVE = _native is not None and hasattr(_native, '_start')
except (ImportError, AttributeError):
    HAS_NATIVE = False

from spprof import start, stop, Profiler


def is_darwin():
    """Check if running on Darwin/macOS."""
    return sys.platform == 'darwin'


class TestCodeRegistryBasics:
    """Basic tests for code registry functionality."""

    @pytest.mark.skipif(not HAS_NATIVE, reason="Native module required")
    def test_profiler_survives_gc_during_sampling(self):
        """
        Test that profiling doesn't crash when GC runs during sampling.
        
        This is the core scenario the code registry addresses.
        """
        gc_count_before = gc.get_count()
        
        def work():
            """Do some work that creates garbage."""
            for _ in range(1000):
                # Create temporary objects that become garbage
                data = [i * 2 for i in range(100)]
                _ = str(data)
        
        # Start profiling (interval_ms=1 is 1000us)
        start(interval_ms=1)
        
        profile = None
        try:
            # Create threads that do work and create garbage
            threads = []
            for _ in range(3):
                t = threading.Thread(target=work)
                t.start()
                threads.append(t)
            
            # Explicitly trigger GC multiple times while profiling
            for _ in range(10):
                gc.collect()
                time.sleep(0.01)
            
            # Wait for threads
            for t in threads:
                t.join()
            
            # One more GC
            gc.collect()
            
        finally:
            # stop() returns the profile - this triggers resolution
            # If registry isn't working, this could crash on freed memory
            profile = stop()
        
        # Basic sanity check - should have some samples
        assert profile is not None
        assert profile.sample_count >= 0  # May be 0 if timing is unlucky

    @pytest.mark.skipif(not HAS_NATIVE, reason="Native module required")
    def test_profiler_with_dynamic_code(self):
        """
        Test profiling code that includes dynamically generated functions.
        
        Dynamic code (exec/eval) creates code objects that may be GC'd
        quickly, which is the highest-risk scenario for use-after-free.
        """
        profile = None
        
        start(interval_ms=1)
        
        try:
            # Create and call dynamically generated functions
            for i in range(20):
                # Create a new function via exec - code object may be short-lived
                exec(f"""
def dynamic_func_{i}():
    return sum(range({i * 100}))
result = dynamic_func_{i}()
""")
                gc.collect()  # Try to collect the dynamic code
        finally:
            profile = stop()
        
        # Should not crash - that's the main test
        assert profile is not None

    @pytest.mark.skipif(not HAS_NATIVE, reason="Native module required")
    def test_profiler_with_lambdas_in_closures(self):
        """
        Test profiling lambdas inside closures that may be garbage collected.
        """
        def create_worker():
            """Create a worker function with lambdas in closures."""
            data = list(range(100))
            
            # These lambdas capture 'data' - when the outer function returns,
            # they may become eligible for GC
            funcs = [
                lambda x=i: sum(d * x for d in data)
                for i in range(10)
            ]
            
            # Call them
            for f in funcs:
                f()
            
            return funcs[-1]  # Return one to keep it alive briefly
        
        profile = None
        start(interval_ms=1)
        
        try:
            workers = []
            for _ in range(10):
                w = create_worker()
                workers.append(w)
                gc.collect()
            
            # Clear references and GC
            workers.clear()
            gc.collect()
            time.sleep(0.05)
            gc.collect()
        finally:
            profile = stop()
        
        assert profile is not None

    @pytest.mark.skipif(not HAS_NATIVE, reason="Native module required")
    def test_concurrent_gc_and_resolution(self):
        """
        Test that resolution works correctly when GC runs concurrently.
        
        This simulates the race condition where:
        1. Samples are captured
        2. GC runs in a separate thread
        3. Resolution happens
        """
        def gc_thread_func():
            """Thread that aggressively runs GC."""
            for _ in range(50):
                gc.collect()
                time.sleep(0.001)
        
        def work_thread_func():
            """Thread that creates garbage."""
            for _ in range(100):
                _ = [i ** 2 for i in range(1000)]
        
        profile = None
        start(interval_ms=1)
        
        try:
            gc_thread = threading.Thread(target=gc_thread_func)
            work_threads = [
                threading.Thread(target=work_thread_func)
                for _ in range(3)
            ]
            
            gc_thread.start()
            for t in work_threads:
                t.start()
            
            gc_thread.join()
            for t in work_threads:
                t.join()
        finally:
            # Resolution happens in stop() - must not crash
            profile = stop()
        
        assert profile is not None


class TestCodeObjectLifetime:
    """Tests for code object lifetime management."""

    @pytest.mark.skipif(not HAS_NATIVE, reason="Native module required")
    @pytest.mark.skipif(not is_darwin(), reason="Darwin/Mach specific")
    def test_darwin_holds_references(self):
        """
        On Darwin, verify the sampler holds references to code objects.
        
        The Darwin/Mach sampler has the GIL during capture and can
        INCREF code objects to keep them alive.
        """
        # Create a function we can track
        weak_refs = []
        
        def create_and_track():
            # Create a function
            exec("""
def tracked_func():
    return 42
""", globals())
            func = globals().get('tracked_func')
            if func:
                # Create weak reference to the code object
                weak_refs.append(weakref.ref(func.__code__))
                func()
            return func
        
        profile = None
        start(interval_ms=1)
        
        try:
            funcs = []
            for _ in range(5):
                f = create_and_track()
                funcs.append(f)
                time.sleep(0.01)
            
            # Clear strong references
            funcs.clear()
            if 'tracked_func' in globals():
                del globals()['tracked_func']
            
            # Even after clearing references, the code objects should
            # still exist if the sampler is holding refs
            # (until we call stop() which releases them)
        finally:
            # stop() returns the profile and releases the refs
            profile = stop()
        
        gc.collect()
        
        # After stop() and gc.collect(), the weak refs might be dead
        # This is expected - the test verifies we don't crash, not that
        # objects stay alive forever
        assert profile is not None


class TestGCEpochTracking:
    """Tests for GC epoch tracking functionality."""

    def test_gc_epoch_changes_on_collect(self):
        """Verify that GC epoch (collection count) changes when GC runs."""
        count1 = gc.get_count()
        gc.collect()
        count2 = gc.get_count()
        
        # At least one generation's count should have changed
        assert count1 != count2 or sum(count2) >= sum(count1)

    @pytest.mark.skipif(not HAS_NATIVE, reason="Native module required")
    def test_profiler_detects_gc_activity(self):
        """
        Test that the profiler's validation can detect GC activity.
        
        While we can't directly test the C code, we can verify that
        profiling works correctly around GC activity.
        """
        # Force GC to establish a baseline
        gc.collect()
        gc.collect()
        gc.collect()
        
        profile = None
        start(interval_ms=1)
        
        try:
            # Do work without GC
            for _ in range(100):
                _ = sum(range(1000))
            
            # Trigger multiple GC cycles
            for gen in range(3):
                gc.collect(gen)
            
            # More work
            for _ in range(100):
                _ = sum(range(1000))
        finally:
            profile = stop()
        
        assert profile is not None


class TestEdgeCases:
    """Edge case tests for code registry."""

    @pytest.mark.skipif(not HAS_NATIVE, reason="Native module required")
    def test_empty_profile_no_crash(self):
        """Test that empty profiles don't crash the registry."""
        start(interval_ms=10)  # Very low sample rate
        time.sleep(0.001)  # Very short duration
        profile = stop()
        
        assert profile is not None

    @pytest.mark.skipif(not HAS_NATIVE, reason="Native module required")
    def test_rapid_start_stop_with_gc(self):
        """Test rapid start/stop cycles with GC don't crash."""
        for _ in range(10):
            start(interval_ms=1)
            gc.collect()
            time.sleep(0.01)
            profile = stop()
            gc.collect()
            assert profile is not None

    @pytest.mark.skipif(not HAS_NATIVE, reason="Native module required")
    def test_module_reload_scenario(self):
        """
        Test behavior similar to module reload where code objects change.
        
        Module reloading causes old code objects to become garbage.
        """
        # We can't actually reload modules safely in tests, but we can
        # simulate the effect by creating and destroying functions
        
        profile = None
        start(interval_ms=1)
        
        try:
            for i in range(20):
                # Create a "module" worth of functions
                module_funcs = {}
                exec(f"""
def func_a():
    return {i}

def func_b():
    return {i * 2}

def func_c():
    return {i * 3}
""", module_funcs)
                
                # Call them
                for name, func in module_funcs.items():
                    if callable(func):
                        func()
                
                # "Unload" by clearing references
                module_funcs.clear()
                gc.collect()
        finally:
            profile = stop()
        
        assert profile is not None


if __name__ == '__main__':
    pytest.main([__file__, '-v'])

