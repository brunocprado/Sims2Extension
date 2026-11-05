#pragma once
#include "ts2/win32_abi.hpp"
#include <stdint.h>

namespace ts2::hook {

// ============================================================================
// x86 32-bit Detour, Patch & IAT Hook Engine
// ============================================================================

struct Detour {
    uintptr_t targetAddr    = 0;
    uintptr_t detourAddr    = 0;
    uint8_t*  trampoline    = nullptr;
    uint8_t   origBytes[16] = {};
    size_t    stolenLen     = 0;
    bool      active        = false;

    template <typename Fn>
    Fn Original() const {
        return reinterpret_cast<Fn>(trampoline);
    }
};

// Safely writes raw bytes to executable/read-only memory using VirtualProtect
bool PatchBytes(uintptr_t address, const void* data, size_t size);

// Fills memory with 0x90 (NOP)
bool NopRange(uintptr_t address, size_t size);

// Replaces a 32-bit pointer in an Import Address Table (IAT) or VTable slot
bool HookPointerSlot(uintptr_t slotAddress, void* newFunction, void** outOriginalFunction);

// Installs an inline 5-byte E9 JMP detour at targetAddr with an executable trampoline
// stolenLen must be >= 5 and aligned to instruction boundaries at targetAddr
bool InstallDetour(Detour& detour, uintptr_t targetAddr, void* hookFn, size_t stolenLen);

// Removes an installed detour
bool RemoveDetour(Detour& detour);

// Writes a formatted message to ts2se.log and OutputDebugStringA
void Log(const char* fmt, ...);

// Logs fatal exceptions (access violations etc.) with registers + stack to the plugin log; never swallows them.
void InstallCrashLogger();
// Records a short line in a ring buffer that is dumped when the game crashes.
void Trace(const char* fmt, ...);

} // namespace ts2::hook
