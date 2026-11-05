// Standalone preview of the in-game panel: a tiny D3D9 window that renders the real ui_kit / Graphics tab.
// Run under Wine (see tools/uipreview/preview.sh). Scenario comes from /tmp/uipreview.ctl, re-read every frame:
//     tab=3 mx=300 my=200 down=0 click=0 exit=0
#include "ts2/ui_kit.hpp"
#include "ts2/ui_tabs.hpp"
#include "ts2/d3d9_ext.hpp"
#include "ts2/dx9_postfx.hpp"

extern "C" {
struct TS2_WNDCLASSA {
    UINT style; WNDPROC lpfnWndProc; int cbClsExtra; int cbWndExtra; HINSTANCE hInstance;
    void* hIcon; void* hCursor; void* hbrBackground; const char* lpszMenuName; const char* lpszClassName;
};
struct TS2_MSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; LONG ptx, pty; };
__declspec(dllimport) WORD  WINAPI RegisterClassA(const TS2_WNDCLASSA*);
__declspec(dllimport) HWND  WINAPI CreateWindowExA(DWORD, const char*, const char*, DWORD, int, int, int, int, HWND, void*, HINSTANCE, void*);
__declspec(dllimport) BOOL  WINAPI ShowWindow(HWND, int);
__declspec(dllimport) BOOL  WINAPI PeekMessageA(TS2_MSG*, HWND, UINT, UINT, UINT);
__declspec(dllimport) BOOL  WINAPI TranslateMessage(const TS2_MSG*);
__declspec(dllimport) LRESULT WINAPI DispatchMessageA(const TS2_MSG*);
__declspec(dllimport) LRESULT WINAPI DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
__declspec(dllimport) BOOL  WINAPI AdjustWindowRect(RECT*, DWORD, BOOL);
__declspec(dllimport) void  WINAPI ExitProcess(UINT);
}

// ---- stubs for the post-fx API used by the Graphics tab -------------------------------------------------
namespace ts2::fx {
static Settings g_s;
static Status   g_st = {true, true, true, true, 120, "ativo (1280x800)"};
Settings& GetSettings() { return g_s; }
const Status& GetStatus() { return g_st; }
void ApplyPreset(int id) { g_s = Settings(); if (id == 0) g_s.enabled = false; if (id == 1) g_s.aoIntensity = 0.5f; if (id == 3) g_s.aoIntensity = 1.15f; }
void NotifyChanged() {}
}

extern "C" int _fltused = 0;
namespace kit = ts2::ui::kit;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static int ParseInt(const char* s, const char* key, int def) {
    const size_t kl = strlen(key);
    for (const char* p = s; *p; ++p) {
        if ((p == s || p[-1] == ' ' || p[-1] == '\n') && strncmp(p, key, kl) == 0 && p[kl] == '=') return static_cast<int>(strtol(p + kl + 1, nullptr, 10));
    }
    return def;
}

static void MockSimTab(float x, float y, float w) {
    kit::LayBegin(x, y, w);
    kit::LaySection("TEMPO", "12:30");
    if (kit::LayBtn(0, 4, "Pausar")) {}
    kit::LayBtn(1, 4, "Velocidade 1x", true);
    kit::LayBtn(2, 4, "Velocidade 2x");
    kit::LayBtn(3, 4, "Velocidade 3x");
    kit::LayNext();
    kit::LayBtn(0, 4, "Manhã (07h)"); kit::LayBtn(1, 4, "Meio-dia (12h)"); kit::LayBtn(2, 4, "Tarde (18h)"); kit::LayBtn(3, 4, "Noite (00h)");
    kit::LayNext();
    kit::LaySection("DINHEIRO DA FAMÍLIA", "$20000", kit::kWarn);
    kit::LayBtn(0, 4, "+ $1.000"); kit::LayBtn(1, 4, "+ $50.000"); kit::LayBtn(2, 4, "+ $250.000"); kit::LayBtn(3, 4, "- $10.000");
    kit::LayNext();
    kit::LaySection("NECESSIDADES");
    static const char* names[8] = {"Fome", "Conforto", "Higiene", "Banheiro", "Energia", "Diversão", "Social", "Ambiente"};
    static const float vals[8] = {0.9f, 0.55f, 0.2f, 0.7f, 0.4f, 0.8f, 0.6f, 0.95f};
    const float half = (kit::g_lay.w - 20.0f) / 2.0f;
    for (int i = 0; i < 8; ++i) {
        const float cx = kit::g_lay.x + (i % 2) * (half + 20.0f), cy = kit::g_lay.y + (i / 2) * 28.0f;
        const float norm = vals[i];
        kit::Text(kit::F_BODY, cx, cy + 3.0f, names[i], kit::kText);
        const float bx = cx + 86.0f, bw = half - 86.0f - 52.0f;
        kit::RoundRect(bx, cy + 5.0f, bw, 10.0f, 5.0f, kit::Col(14, 18, 30, 230), kit::Col(30, 38, 58, 230));
        const DWORD t = (norm > 0.6f) ? kit::Col(96, 232, 150) : (norm > 0.3f) ? kit::Col(255, 205, 90) : kit::Col(255, 120, 108);
        const DWORD b = (norm > 0.6f) ? kit::Col(48, 184, 110) : (norm > 0.3f) ? kit::Col(224, 160, 40) : kit::Col(222, 76, 66);
        kit::RoundRect(bx, cy + 5.0f, bw * norm, 10.0f, 5.0f, t, b);
        kit::Button(cx + half - 46.0f, cy, 46.0f, 22.0f, "MAX", false, kit::F_SMALL);
    }
}

extern "C" void WINAPI WinMainCRTStartup() {
    const int W = 1100, H = 760;
    HINSTANCE inst = GetModuleHandleA(nullptr);
    TS2_WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = "ts2preview";
    RegisterClassA(&wc);
    RECT r = {0, 0, W, H};
    AdjustWindowRect(&r, 0x00CF0000 /*WS_OVERLAPPEDWINDOW*/, FALSE);
    HWND hwnd = CreateWindowExA(0, "ts2preview", "ts2 ui preview", 0x10CF0000 /*+VISIBLE*/, 0, 0, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    ShowWindow(hwnd, 5);

    HMODULE d3d = LoadLibraryA("d3d9.dll");
    using FnCreate9 = d3d9::IDirect3D9*(WINAPI*)(UINT);
    auto create9 = reinterpret_cast<FnCreate9>(GetProcAddress(d3d, "Direct3DCreate9"));
    d3d9::IDirect3D9* d9 = create9(32);
    d3d9::D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = W; pp.BackBufferHeight = H; pp.BackBufferCount = 1; pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = FALSE; pp.PresentationInterval = 0x80000000u;
    d3d9::IDirect3DDevice9* dev = nullptr;
    using FnCreateDev = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, DWORD, HWND, DWORD, d3d9::D3DPRESENT_PARAMETERS*, d3d9::IDirect3DDevice9**);
    HRESULT hr = reinterpret_cast<FnCreateDev>(d9->vtable[16])(d9, 0, 1, hwnd, 0x20 /*SOFTWARE_VP*/, &pp, &dev);
    if (hr < 0 || !dev) ExitProcess(2);

    using FnClear = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD);
    using FnVoid = HRESULT(STDMETHODCALLTYPE*)(void*);
    using FnPresent = HRESULT(STDMETHODCALLTYPE*)(void*, const void*, const void*, HWND, const void*);

    char ctl[256];
    DWORD frames = 0;
    for (;;) {
        TS2_MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, 1)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        ctl[0] = 0;
        HANDLE f = CreateFileA("/tmp/uipreview.ctl", GENERIC_READ, 3, nullptr, 3, 0x80, nullptr);
        if (f != reinterpret_cast<HANDLE>(-1)) { DWORD got = 0; ReadFile(f, ctl, sizeof(ctl) - 1, &got, nullptr); ctl[got] = 0; CloseHandle(f); }
        if (ParseInt(ctl, "exit", 0)) break;
        const int tab = ParseInt(ctl, "tab", 3);

        reinterpret_cast<FnClear>(dev->vtable[43])(dev, 0, nullptr, 3 /*TARGET|ZBUFFER ignored*/ & 1, 0xFF4E6E3C, 1.0f, 0);
        reinterpret_cast<FnVoid>(dev->vtable[41])(dev);

        kit::Input in;
        in.mx = static_cast<float>(ParseInt(ctl, "mx", -100)); in.my = static_cast<float>(ParseInt(ctl, "my", -100));
        in.down = ParseInt(ctl, "down", 0) != 0; in.clicked = ParseInt(ctl, "click", 0) != 0 && (frames % 2 == 0);
        kit::BeginFrame(dev, in);

        // fake "game" backdrop so the panel translucency can be judged
        for (int i = 0; i < 14; ++i)
            kit::RoundRect(40.0f + i * 70.0f, 560.0f - (i % 5) * 40.0f, 90.0f, 90.0f, 6.0f, kit::Col(210 - i * 6, 190, 140 + i * 5, 255), kit::Col(150, 130, 100, 255));

        const float wx = 16, wy = 16, ww = 680, wh = 620;
        kit::WindowFrame(wx, wy, ww, wh);
        kit::Diamond(wx + 30.0f, wy + 29.0f, 7.5f, kit::Col(70, 220, 120));
        kit::Text(kit::F_TITLE, wx + 50.0f, wy + 29.0f - kit::FontHeight(kit::F_TITLE) * 0.5f, "The Sims 2 Suite", kit::Col(246, 250, 255));
        float px = wx + ww - 18.0f, pw = 0.0f;
        kit::Pill(px, wy + 18.0f, "SOLO", kit::kWarn, &pw, true); px -= pw + 8.0f;
        kit::Pill(px, wy + 18.0f, "12:30", kit::kAccent, &pw, true); px -= pw + 8.0f;
        kit::Pill(px, wy + 18.0f, "$20000", kit::Col(150, 235, 170), &pw, true);
        static const char* const tabs[6] = {"Sim", "Construir", "Jogo", "Gráficos", "Co-op", "Avançado"};
        kit::TabBar(wx + 16.0f, wy + 56.0f, ww - 32.0f, 34.0f, tabs, 6, tab);
        const float cx = wx + 22.0f, cy = wy + 104.0f, cw = ww - 44.0f;
        if (tab == 3) ts2::ui::RenderGraphicsTab(cx, cy, cw);
        else MockSimTab(cx, cy, cw);
        kit::Rect(wx + 20.0f, wy + wh - 32.0f, ww - 40.0f, 1.0f, kit::Col(255, 255, 255, 22));
        kit::Text(kit::F_SMALL, wx + 22.0f, wy + wh - 26.0f, "F10 ocultar    \xE2\x80\xA2    F11 tela cheia    \xE2\x80\xA2    F5 salvar    \xE2\x80\xA2    F9 sincronizar co-op", kit::kDim);

        // hidden-panel badge
        {
            const char* badge = "SOLO   $20000   12:30   \xE2\x80\xA2   F10";
            const float bw = kit::TextWidth(kit::F_BODY, badge) + 52.0f, bh = 30.0f, bx = 720.0f, by = 20.0f;
            kit::Shadow(bx, by + 2.0f, bw, bh, 15.0f, 14.0f, kit::Col(0, 0, 0, 90));
            kit::RoundRect(bx, by, bw, bh, 15.0f, kit::Col(30, 38, 60, 238), kit::Col(14, 18, 30, 238));
            kit::RoundOutline(bx, by, bw, bh, 15.0f, 1.0f, kit::Col(255, 255, 255, 40));
            kit::Diamond(bx + 18.0f, by + 15.0f, 5.5f, kit::Col(70, 220, 120));
            kit::Text(kit::F_BODY, bx + 32.0f, by + (bh - kit::FontHeight(kit::F_BODY)) * 0.5f, badge, kit::kText);
        }
        kit::EndFrame();

        reinterpret_cast<FnVoid>(dev->vtable[42])(dev);
        reinterpret_cast<FnPresent>(dev->vtable[17])(dev, nullptr, nullptr, nullptr, nullptr);
        ++frames;
        Sleep(30);
    }
    ExitProcess(0);
}
