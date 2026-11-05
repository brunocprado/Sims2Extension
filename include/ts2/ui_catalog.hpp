#pragma once
#include "ts2/ui_kit.hpp"

namespace ts2::ui::catalog_ui {

// Full-screen-ish build catalog: categories, search, thumbnail grid, details and placement controls (F7).
bool IsOpen();
void Open();
void Close();
void Toggle();

// Draws the window; call between kit::BeginFrame and kit::EndFrame.
void Draw(d3d9::IDirect3DDevice9* dev, float scrW, float scrH);

// Input routing from the window procedure
bool HitTest(float x, float y, float scrW, float scrH);   // inside the window (clicks must not reach the game)
bool SearchFocused();
void OnChar(unsigned ch);                                  // typed character (WM_CHAR), only used while the search box has focus
void OnWheel(int notches);                                 // > 0 = scroll up
bool OnEscape();                                           // true if consumed (closes the window)

} // namespace ts2::ui::catalog_ui
