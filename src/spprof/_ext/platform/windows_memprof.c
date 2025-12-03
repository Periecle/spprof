/* SPDX-License-Identifier: MIT
 * windows_memprof.c - Windows memory profiler hooks (EXPERIMENTAL)
 *
 * STATUS: EXPERIMENTAL - Windows support is minimal in v1.0.
 *
 * Known Limitations:
 * - Only hooks CRT malloc (misses HeapAlloc, VirtualAlloc)
 * - TLS via __declspec(thread) has DLL loading caveats
 * - No realloc/calloc hooks shown (implementation TODO)
 *
 * For Windows profiling in v1.0, consider using Visual Studio's built-in
 * heap profiler or ETW instead.
 */

#if defined(_WIN32)

#include "../memprof/memprof.h"
#include "../memprof/sampling.h"
#include <windows.h>

/* ============================================================================
 * Stub Implementation
 *
 * Full Windows support via MS Detours is planned for v1.1+
 * ============================================================================ */

static int g_windows_hooks_installed = 0;

int memprof_windows_install(void) {
    if (g_windows_hooks_installed) {
        return -1;
    }
    
    g_windows_hooks_installed = 1;
    
    /* TODO: Implement via MS Detours
     *
     * DetourTransactionBegin();
     * DetourUpdateThread(GetCurrentThread());
     * DetourAttach(&(PVOID&)Real_malloc, Hooked_malloc);
     * DetourAttach(&(PVOID&)Real_free, Hooked_free);
     * DetourTransactionCommit();
     */
    
    return 0;
}

void memprof_windows_remove(void) {
    if (!g_windows_hooks_installed) {
        return;
    }
    
    g_windows_hooks_installed = 0;
    
    /* TODO: Implement via MS Detours
     *
     * DetourTransactionBegin();
     * DetourUpdateThread(GetCurrentThread());
     * DetourDetach(&(PVOID&)Real_malloc, Hooked_malloc);
     * DetourDetach(&(PVOID&)Real_free, Hooked_free);
     * DetourTransactionCommit();
     */
}

#endif /* _WIN32 */

