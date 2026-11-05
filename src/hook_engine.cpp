#include "ts2/hook_engine.hpp"
#include <stdarg.h>

namespace ts2::hook {

static FILE* g_logFile = nullptr;

void Log(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);
    if (n < 0) n = static_cast<int>( sizeof(buf) - 2 );
    buf[n] = '\0';

    if (!g_logFile) {
        char logName[64];
        _snprintf(logName, sizeof(logName), "ts2se_%u.log", static_cast<unsigned>(GetCurrentProcessId()));
        g_logFile = fopen(logName, "a");
    }
    if (g_logFile) {
        fprintf(g_logFile, "[%08u] %s\n", static_cast<unsigned>(GetTickCount()), buf);
        fflush(g_logFile);
    }
    OutputDebugStringA(buf);
}

bool PatchBytes(uintptr_t address, const void* data, size_t size) {
    if (!address || !data || size == 0) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    memcpy(reinterpret_cast<void*>(address), data, size);
    DWORD dummy = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(address), size, oldProtect, &dummy);
    return true;
}

bool NopRange(uintptr_t address, size_t size) {
    if (!address || size == 0 || size > 64) return false;
    uint8_t nops[64];
    memset(nops, 0x90, size);
    return PatchBytes(address, nops, size);
}

bool HookPointerSlot(uintptr_t slotAddress, void* newFunction, void** outOriginalFunction) {
    if (!slotAddress || !newFunction) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(slotAddress), sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    void* orig = *reinterpret_cast<void**>(slotAddress);
    if (outOriginalFunction && !*outOriginalFunction) {
        *outOriginalFunction = orig;
    }
    *reinterpret_cast<void**>(slotAddress) = newFunction;
    DWORD dummy = 0;
    VirtualProtect(reinterpret_cast<LPVOID>(slotAddress), sizeof(void*), oldProtect, &dummy);
    return true;
}

bool InstallDetour(Detour& detour, uintptr_t targetAddr, void* hookFn, size_t stolenLen) {
    if (detour.active || stolenLen < 5 || stolenLen > sizeof(detour.origBytes)) {
        return false;
    }
    uint8_t* tramp = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)
    );
    if (!tramp) return false;

    detour.targetAddr = targetAddr;
    detour.detourAddr = reinterpret_cast<uintptr_t>(hookFn);
    detour.trampoline = tramp;
    detour.stolenLen  = stolenLen;

    const uint8_t* src = reinterpret_cast<const uint8_t*>(targetAddr);
    memcpy(detour.origBytes, src, stolenLen);
    memcpy(tramp, src, stolenLen);

    // Fixup relative E8/E9 if the first instruction in stolen bytes is a relative call/jmp
    if ((tramp[0] == 0xE8 || tramp[0] == 0xE9) && stolenLen >= 5) {
        int32_t origRel = *reinterpret_cast<const int32_t*>(src + 1);
        uintptr_t absDest = (targetAddr + 5) + static_cast<uintptr_t>(origRel);
        int32_t newRel = static_cast<int32_t>(absDest - (reinterpret_cast<uintptr_t>(tramp) + 5));
        *reinterpret_cast<int32_t*>(tramp + 1) = newRel;
    }

    // Append JMP back to targetAddr + stolenLen
    tramp[stolenLen] = 0xE9;
    int32_t backRel = static_cast<int32_t>((targetAddr + stolenLen) - (reinterpret_cast<uintptr_t>(tramp + stolenLen) + 5));
    *reinterpret_cast<int32_t*>(tramp + stolenLen + 1) = backRel;

    // Write JMP to hookFn at targetAddr
    uint8_t patch[16];
    memset(patch, 0x90, stolenLen);
    patch[0] = 0xE9;
    int32_t hookRel = static_cast<int32_t>(detour.detourAddr - (targetAddr + 5));
    *reinterpret_cast<int32_t*>(patch + 1) = hookRel;

    if (!PatchBytes(targetAddr, patch, stolenLen)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        detour.trampoline = nullptr;
        return false;
    }

    detour.active = true;
    return true;
}

bool RemoveDetour(Detour& detour) {
    if (!detour.active) return false;
    PatchBytes(detour.targetAddr, detour.origBytes, detour.stolenLen);
    if (detour.trampoline) {
        VirtualFree(detour.trampoline, 0, MEM_RELEASE);
        detour.trampoline = nullptr;
    }
    detour.active = false;
    return true;
}

} // namespace ts2::hook
