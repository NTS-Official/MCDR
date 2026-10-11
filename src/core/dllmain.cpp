// dllmain.cpp — module entry point for the MCDR native render library.
//
// Deliberately minimal. There is no per-thread work to do (the render thread and
// the descriptor workers are created and torn down explicitly by the JVM side via
// initAsyncRenderer/destroyAsyncRenderer), and DLL_PROCESS_DETACH runs under the
// loader lock, so blocking on the GPU there is not safe. Resource teardown is the
// JVM side's job: it calls mcdr_dx12DestroyDevice / flushPendingDeletes(force)
// before releasing the library.
//
// The old Visual Studio "DLL template" DllMain depended on pch.h, and the rest of
// that template (MCDR.cpp, MCDR.h, framework.h, pch.h, pch.cpp) exported a
// placeholder C++ class that has nothing to do with this library. The real export
// surface is the pure C ABI in include/mcdr_abi.h.

#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)lpReserved;
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            // We never create threads from DllMain and do not need per-thread
            // notifications; skipping them avoids a loader-lock acquisition per
            // thread, and this library is loaded into a JVM with many threads.
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
        case DLL_PROCESS_DETACH: break;
        default: break;
    }
    return TRUE;
}
