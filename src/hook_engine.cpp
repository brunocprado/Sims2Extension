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
        char logName[96];
        char tag[24] = {};
        GetEnvironmentVariableA("TS2_LOGTAG", tag, sizeof(tag) - 1);   // lets two test instances log to separate files
        if (tag[0]) _snprintf(logName, sizeof(logName), "ts2se_%s_%u.log", tag, static_cast<unsigned>(GetCurrentProcessId()));
        else        _snprintf(logName, sizeof(logName), "ts2se_%u.log", static_cast<unsigned>(GetCurrentProcessId()));
        g_logFile = fopen(logName, "a");
    }
    if (g_logFile) {
        fprintf(g_logFile, "[%08u] %s\n", static_cast<unsigned>(GetTickCount()), buf);
        fflush(g_logFile);
    }
    OutputDebugStringA(buf);
}

// ---------------------------------------------------------------------------
// Crash logger: a vectored exception handler that records every access violation (and other fatal exception
// codes) with registers, the faulting module offset and a raw stack dump to the plugin log. It never
// handles the exception (EXCEPTION_CONTINUE_SEARCH), so game behaviour is unchanged.
// ---------------------------------------------------------------------------
namespace {
struct ExceptionRecord32 {
    DWORD   code;
    DWORD   flags;
    void*   next;
    void*   address;
    DWORD   numParams;
    uintptr_t params[15];
};
struct ExceptionPointers32 {
    ExceptionRecord32* record;
    uint8_t*           context;   // x86 CONTEXT, accessed by offset (Edi@0x9C ... Eip@0xB8 ... Esp@0xC4)
};
uint32_t g_crashLogged = 0;

// Ring of the last mutating operations the plugin performed (moves, creates, deletes ...). Dumped on a crash so we
// can see what we did to the game right before it fell over.
constexpr uint32_t kTraceN = 64;
char g_trace[kTraceN][112];
volatile long g_traceIdx = 0;

uintptr_t CtxReg(const ExceptionPointers32* e, uint32_t off) { return *reinterpret_cast<const uintptr_t*>(e->context + off); }

LONG __stdcall CrashVeh(ExceptionPointers32* e) {
    if (!e || !e->record || !e->context) return 0;
    DWORD c = e->record->code;
    const bool fatal = c == 0xC0000005u || c == 0xC000001Du || c == 0xC0000094u || c == 0xC0000096u ||
                       c == 0xC00000FDu || c == 0xC0000374u || c == 0xC0000409u || c == 0xC0000025u;
    if (!fatal || g_crashLogged >= 12) return 0;
    ++g_crashLogged;
    HMODULE exe = GetModuleHandleA(nullptr);
    uintptr_t eip = CtxReg(e, 0xB8), esp = CtxReg(e, 0xC4), ebp = CtxReg(e, 0xB4);
    Log("[CRASH] exception 0x%08X at EIP=%08X (exe base %08X, rva %08X) esp=%08X ebp=%08X",
        static_cast<unsigned>(c), static_cast<unsigned>(eip), static_cast<unsigned>(reinterpret_cast<uintptr_t>(exe)),
        static_cast<unsigned>(eip - reinterpret_cast<uintptr_t>(exe)), static_cast<unsigned>(esp), static_cast<unsigned>(ebp));
    if (c == 0xC0000005u && e->record->numParams >= 2) {
        Log("[CRASH]   %s address %08X", e->record->params[0] == 0 ? "READ of" : (e->record->params[0] == 1 ? "WRITE to" : "EXEC of"),
            static_cast<unsigned>(e->record->params[1]));
    }
    // Stack words that look like return addresses into the game image (gives a poor man's backtrace)
    if (esp > 0x10000u && (esp & 3u) == 0 && exe) {
        uintptr_t base = reinterpret_cast<uintptr_t>(exe);
        uint32_t peOff = *reinterpret_cast<const uint32_t*>(base + 0x3C);
        uint32_t imgSize = *reinterpret_cast<const uint32_t*>(base + peOff + 0x18 + 0x38);
        char line[200];
        int n = 0, shown = 0;
        for (uint32_t k = 0; k < 512 && shown < 40; ++k) {
            uint32_t v = *reinterpret_cast<const uint32_t*>(esp + k * 4u);
            if (v > base && v < base + imgSize) {
                n += _snprintf(line + n, sizeof(line) - n, "%08X ", static_cast<unsigned>(v - base + 0xA0000000u - 0x10000u + 0x10000u));
                if (++shown % 6 == 0) { Log("[CRASH]   ret(static): %s", line); n = 0; }
            }
        }
        if (n) Log("[CRASH]   ret(static): %s", line);
    }
    {
        long cur = g_traceIdx;
        for (uint32_t k = 0; k < 24; ++k) {
            long i = (cur - 23 + static_cast<long>(k)) & (kTraceN - 1);
            if (cur - 23 + static_cast<long>(k) < 0 || !g_trace[i][0]) continue;
            Log("[CRASH]   op: %s", g_trace[i]);
        }
    }
    Log("[CRASH]   eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X",
        static_cast<unsigned>(CtxReg(e, 0xB0)), static_cast<unsigned>(CtxReg(e, 0xA4)), static_cast<unsigned>(CtxReg(e, 0xAC)),
        static_cast<unsigned>(CtxReg(e, 0xA8)), static_cast<unsigned>(CtxReg(e, 0xA0)), static_cast<unsigned>(CtxReg(e, 0x9C)));
    if (c != 0xC00000FDu && esp > 0x10000u && (esp & 3u) == 0) {
        // Raw stack words (return addresses show up as values inside the exe / plugin images)
        char line[200];
        for (int row = 0; row < 8; ++row) {
            int n = 0;
            for (int k = 0; k < 8; ++k) {
                uintptr_t addr = esp + static_cast<uintptr_t>(row * 8 + k) * 4u;
                n += _snprintf(line + n, sizeof(line) - n, "%08X ", static_cast<unsigned>(*reinterpret_cast<const uint32_t*>(addr)));
            }
            Log("[CRASH]   stack+%02X: %s", row * 32, line);
        }
    }
    return 0; // EXCEPTION_CONTINUE_SEARCH
}
} // namespace

void Trace(const char* fmt, ...) {
    long i = (_InterlockedIncrement(&g_traceIdx)) & (kTraceN - 1);
    char tmp[112];
    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf(tmp, sizeof(tmp) - 1, fmt, args);
    va_end(args);
    if (n < 0) n = sizeof(tmp) - 1;
    tmp[n] = 0;
    _snprintf(g_trace[i], sizeof(g_trace[i]) - 1, "%u %s", static_cast<unsigned>(GetTickCount()), tmp);
    g_trace[i][sizeof(g_trace[i]) - 1] = 0;
}

void InstallCrashLogger() {
    using FnAdd = void* (__stdcall*)(ULONG, void*);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FnAdd add = k32 ? reinterpret_cast<FnAdd>(GetProcAddress(k32, "AddVectoredExceptionHandler")) : nullptr;
    if (add) add(1, reinterpret_cast<void*>(&CrashVeh));
    Log("[CRASH] logger de excecoes %s", add ? "ativo" : "indisponivel");
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
