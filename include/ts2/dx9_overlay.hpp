#pragma once
#include "ts2/win32_abi.hpp"

namespace ts2::ui {

// ============================================================================
// DirectX 9 Immediate-Mode Interactive Overlay for Sims2EP9.exe
// Hooks Direct3DCreate9 (0xA09F5758 / IAT 0xA0F4974C) -> EndScene & Reset
// ============================================================================

bool InitializeDx9Overlay();
void ShutdownDx9Overlay();

bool IsOverlayVisible();
void SetOverlayVisible(bool visible);

} // namespace ts2::ui
