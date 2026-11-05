#pragma once
#include "ts2/win32_abi.hpp"

// ============================================================================
// ui_kit: small immediate-mode toolkit used by the in-game panel (F10).
//
//   * anti-aliased text from a real system font (GDI -> glyph atlas texture)
//   * rounded rectangles with smooth edges, vertical gradients, soft shadows
//   * widgets: button, toggle switch, slider, tab bar, pills
//   * a vertical "stack" layout (sections of equal-width columns)
//
// No dependency on the game or on the rest of the plugin: it only needs a Direct3D 9 device.
// Everything is drawn in one textured, batched DrawPrimitiveUP stream (shapes sample a white texel).
// ============================================================================
namespace ts2::ui::kit {

using Dev = d3d9::IDirect3DDevice9;

enum Font : int { F_SMALL = 0, F_BODY, F_BOLD, F_TITLE, F_COUNT };

struct Input {
    float mx = 0, my = 0;
    bool  down = false;      // left button currently held
    bool  clicked = false;   // left button went down this frame
};

constexpr DWORD Col(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return d3d9::ColorARGB(a, r, g, b); }

// Palette
constexpr DWORD kText    = Col(236, 242, 252);
constexpr DWORD kDim     = Col(142, 158, 186);
constexpr DWORD kAccent  = Col(98, 190, 255);
constexpr DWORD kGood    = Col(96, 226, 150);
constexpr DWORD kWarn    = Col(255, 205, 100);
constexpr DWORD kBad     = Col(255, 128, 118);

constexpr float kRowH = 28.0f;
constexpr float kGap  = 6.0f;

DWORD WithAlpha(DWORD c, float a);               // multiplies the alpha channel
DWORD Mix(DWORD a, DWORD b, float t);

// Frame
void BeginFrame(Dev* dev, const Input& in);      // sets the device state used by the kit
void EndFrame();                                 // flushes the batch
const Input& GetInput();

// Shapes (screen pixels, y down)
void Rect(float x, float y, float w, float h, DWORD c);
void RoundRect(float x, float y, float w, float h, float r, DWORD c);
void RoundRect(float x, float y, float w, float h, float r, DWORD top, DWORD bottom);
void RoundOutline(float x, float y, float w, float h, float r, float thick, DWORD c);
void Shadow(float x, float y, float w, float h, float r, float blur, DWORD c);
void Diamond(float cx, float cy, float r, DWORD base);   // the Sims plumbob
// Draws a whole texture (flushes the batch first; `tint` multiplies the texels).
void Image(void* tex, float x, float y, float w, float h, DWORD tint = 0xFFFFFFFFu);

// Text. (x, y) is the top-left of the line box.
float FontHeight(Font f);
void  SetTranslate(bool on);   // off = draw strings as given (user-typed text)
float TextWidth(Font f, const char* s);
void  Text(Font f, float x, float y, const char* s, DWORD c);
void  TextRight(Font f, float rightX, float y, const char* s, DWORD c);
void  TextCenter(Font f, float cx, float y, const char* s, DWORD c);
// Draws `s` (UTF-8) truncated with an ellipsis so that it fits in `maxW`.
void  TextFit(Font f, float x, float y, float maxW, const char* s, DWORD c);

// Widgets (return true when activated this frame)
bool Hover(float x, float y, float w, float h);
bool Button(float x, float y, float w, float h, const char* label, bool active = false, Font f = F_BODY);
bool Toggle(float x, float y, bool on);                                    // 40 x 22 switch
bool Slider(int id, float x, float y, float w, float* v, float lo, float hi);   // true while the value changes
void Pill(float x, float y, const char* s, DWORD tint, float* outWidth = nullptr, bool rightAlignedAtX = false);
// Segmented tab bar; returns the clicked tab index or -1.
int  TabBar(float x, float y, float w, float h, const char* const* labels, int n, int active);

// Window chrome
void WindowFrame(float x, float y, float w, float h);

// Layout: a vertical stack of sections; every row is a set of equal columns.
struct Layout { float x = 0, y = 0, w = 0; bool first = true; };
extern Layout g_lay;
void LayBegin(float x, float y, float w);
void LaySection(const char* title, const char* right = nullptr, DWORD rightColor = kText);
void LayText(const char* text, DWORD color = kText, Font f = F_BODY);
void LayNext();
bool LayBtn(int col, int cols, const char* label, bool active = false, int span = 1);
// One settings row: [switch] name ---- slider ---- value [reset]. `val` may be null for plain on/off rows.
bool FxRow(const char* name, bool* on, float* val, float lo, float hi, float def, int decimals = 2);
// Same row without the on/off switch (a plain labelled slider with a "Padrão" button).
bool ValRow(const char* name, float* val, float lo, float hi, float def, int decimals = 2);

} // namespace ts2::ui::kit
