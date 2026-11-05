#include "ts2/ui_kit.hpp"
#include "ts2/d3d9_ext.hpp"
#include "ts2/i18n.hpp"

// ----------------------------------------------------------------------------
// GDI imports (only used once per font to bake a glyph atlas)
// ----------------------------------------------------------------------------
extern "C" {
struct TS2_BITMAPINFOHEADER {
    DWORD biSize; LONG biWidth; LONG biHeight; WORD biPlanes; WORD biBitCount; DWORD biCompression;
    DWORD biSizeImage; LONG biXPelsPerMeter; LONG biYPelsPerMeter; DWORD biClrUsed; DWORD biClrImportant;
};
struct TS2_TEXTMETRICW {
    LONG tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading, tmAveCharWidth, tmMaxCharWidth,
         tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
    wchar_t tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    BYTE tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
    BYTE pad[16];
};
struct TS2_SIZE { LONG cx, cy; };

__declspec(dllimport) HDC   WINAPI CreateCompatibleDC(HDC);
__declspec(dllimport) BOOL  WINAPI DeleteDC(HDC);
__declspec(dllimport) void* WINAPI CreateDIBSection(HDC, const TS2_BITMAPINFOHEADER*, UINT, void**, HANDLE, DWORD);
__declspec(dllimport) void* WINAPI SelectObject(HDC, void*);
__declspec(dllimport) BOOL  WINAPI DeleteObject(void*);
__declspec(dllimport) void* WINAPI CreateFontW(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, const wchar_t*);
__declspec(dllimport) DWORD WINAPI SetTextColor(HDC, DWORD);
__declspec(dllimport) int   WINAPI SetBkMode(HDC, int);
__declspec(dllimport) BOOL  WINAPI TextOutW(HDC, int, int, const wchar_t*, int);
__declspec(dllimport) BOOL  WINAPI GetTextExtentPoint32W(HDC, const wchar_t*, int, TS2_SIZE*);
__declspec(dllimport) BOOL  WINAPI GetTextMetricsW(HDC, TS2_TEXTMETRICW*);
__declspec(dllimport) BOOL  WINAPI GdiFlush();
}

namespace ts2::ui::kit {

Layout g_lay;

namespace {

// ============================================================================
// Glyph atlas
// ============================================================================
constexpr int kGlyphs = 195;

static uint32_t g_glyphCp[kGlyphs];
static bool     g_glyphTableReady = false;

static void InitGlyphTable() {
    if (g_glyphTableReady) return;
    g_glyphTableReady = true;
    int n = 0;
    for (uint32_t c = 32; c <= 126; ++c) g_glyphCp[n++] = c;
    for (uint32_t c = 160; c <= 255; ++c) g_glyphCp[n++] = c;
    g_glyphCp[n++] = 0x2212;   // minus
    g_glyphCp[n++] = 0x2022;   // bullet
    g_glyphCp[n++] = 0x2013;   // en dash
    g_glyphCp[n++] = 0x2026;   // ellipsis
}

static int GlyphIndex(uint32_t cp) {
    if (cp >= 32 && cp <= 126) return static_cast<int>(cp - 32);
    if (cp >= 160 && cp <= 255) return static_cast<int>(95 + (cp - 160));
    switch (cp) {
        case 0x2212: return 191;
        case 0x2022: return 192;
        case 0x2013: return 193;
        case 0x2026: return 194;
    }
    return '?' - 32;
}

static uint32_t NextCp(const char*& p) {
    const unsigned char c = static_cast<unsigned char>(*p++);
    if (c < 0x80) return c;
    const unsigned char c1 = static_cast<unsigned char>(p[0]);
    if ((c & 0xE0) == 0xC0 && (c1 & 0xC0) == 0x80) {
        ++p;
        return (static_cast<uint32_t>(c & 0x1F) << 6) | (c1 & 0x3F);
    }
    const unsigned char c2 = c1 ? static_cast<unsigned char>(p[1]) : 0;
    if ((c & 0xF0) == 0xE0 && (c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
        p += 2;
        return (static_cast<uint32_t>(c & 0x0F) << 12) | (static_cast<uint32_t>(c1 & 0x3F) << 6) | (c2 & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && (c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 && p[2] && (static_cast<unsigned char>(p[2]) & 0xC0) == 0x80) {
        const unsigned char c3 = static_cast<unsigned char>(p[2]);
        p += 3;
        return (static_cast<uint32_t>(c & 0x07) << 18) | (static_cast<uint32_t>(c1 & 0x3F) << 12) |
               (static_cast<uint32_t>(c2 & 0x3F) << 6) | (c3 & 0x3F);
    }
    return c >= 0xA0 ? c : '?';    // stray Latin-1 byte
}

struct Atlas {
    bool  built  = false;
    bool  failed = false;
    void* tex    = nullptr;
    int   texW = 0, texH = 0;
    float height = 0.0f;       // text height in pixels
    float cellH  = 0.0f;
    float adv[kGlyphs];
    uint16_t gx[kGlyphs], gy[kGlyphs], gw[kGlyphs];
    float whiteU = 0.0f, whiteV = 0.0f;
};

static Atlas  g_atlas[F_COUNT];
static Dev*   g_dev = nullptr;
static Dev*   g_atlasDev = nullptr;
static Font   g_font = F_BODY;
static Input  g_in;
static int    g_activeSlider = 0;

static void DebugLog(const char* s) { OutputDebugStringA(s); }

static bool BuildAtlas(Dev* dev, Font f, Atlas& a) {
    static const struct { int px; int weight; } spec[F_COUNT] = {{12, 400}, {14, 400}, {14, 600}, {18, 600}};
    InitGlyphTable();
    a.failed = true;

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) { DebugLog("[ui_kit] CreateCompatibleDC falhou\n"); return false; }
    // DEFAULT_CHARSET=1, OUT_TT_PRECIS=4, CLIP_DEFAULT_PRECIS=0, ANTIALIASED_QUALITY=4, DEFAULT_PITCH|FF_SWISS=0x20
    void* font = CreateFontW(-spec[f].px, 0, 0, 0, spec[f].weight, 0, 0, 0, 1, 4, 0, 4, 0x20, L"Segoe UI");
    if (!font) { DeleteDC(dc); DebugLog("[ui_kit] CreateFontW falhou\n"); return false; }
    void* oldFont = SelectObject(dc, font);

    TS2_TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    const int cellH = static_cast<int>(tm.tmHeight) + 2;
    a.height = static_cast<float>(tm.tmHeight);
    a.cellH  = static_cast<float>(cellH);

    // pass 1: measure + pack
    const int texW = 512;
    int cx = 0, cy = 4;
    for (int i = 0; i < kGlyphs; ++i) {
        wchar_t ch = static_cast<wchar_t>(g_glyphCp[i]);
        TS2_SIZE sz = {};
        GetTextExtentPoint32W(dc, &ch, 1, &sz);
        const int cw = static_cast<int>(sz.cx) + 2;
        if (cx + cw > texW) { cx = 0; cy += cellH; }
        a.gx[i] = static_cast<uint16_t>(cx);
        a.gy[i] = static_cast<uint16_t>(cy);
        a.gw[i] = static_cast<uint16_t>(cw);
        a.adv[i] = static_cast<float>(sz.cx);
        cx += cw;
    }
    int texH = 64;
    while (texH < cy + cellH) texH <<= 1;
    a.texW = texW; a.texH = texH;

    // pass 2: rasterise into a top-down 32 bit DIB
    TS2_BITMAPINFOHEADER bmi = {};
    bmi.biSize = sizeof(bmi); bmi.biWidth = texW; bmi.biHeight = -texH; bmi.biPlanes = 1; bmi.biBitCount = 32;
    void* bits = nullptr;
    void* dib = CreateDIBSection(dc, &bmi, 0, &bits, nullptr, 0);
    if (!dib || !bits) {
        SelectObject(dc, oldFont); DeleteObject(font); DeleteDC(dc);
        DebugLog("[ui_kit] CreateDIBSection falhou\n");
        return false;
    }
    void* oldBmp = SelectObject(dc, dib);
    memset(bits, 0, static_cast<size_t>(texW) * texH * 4);
    SetBkMode(dc, 1 /*TRANSPARENT*/);
    SetTextColor(dc, 0x00FFFFFF);
    for (int i = 0; i < kGlyphs; ++i) {
        wchar_t ch = static_cast<wchar_t>(g_glyphCp[i]);
        TextOutW(dc, a.gx[i] + 1, a.gy[i] + 1, &ch, 1);
    }
    GdiFlush();

    // texture (managed pool: survives device Reset)
    using FnCreateTex = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, void*);
    void* tex = nullptr;
    HRESULT hr = reinterpret_cast<FnCreateTex>(dev->vtable[d3d9::slot::CreateTexture])(dev, texW, texH, 1, 0, 21 /*A8R8G8B8*/, 1 /*MANAGED*/, &tex, nullptr);
    bool ok = hr >= 0 && tex;
    if (ok) {
        struct LockedRect { INT pitch; void* bits; } lr = {};
        auto** vt = static_cast<d3d9::IUnknownLike*>(tex)->vtable;
        using FnLock = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, LockedRect*, const RECT*, DWORD);
        using FnUnlock = HRESULT(STDMETHODCALLTYPE*)(void*, UINT);
        if (reinterpret_cast<FnLock>(vt[19])(tex, 0, &lr, nullptr, 0) >= 0 && lr.bits) {
            // coverage -> alpha with a slight boost so light text on dark panels does not look thin
            uint8_t lut[256];
            for (int i = 0; i < 256; ++i) {
                float v = static_cast<float>(i) / 255.0f;
                v = v * (2.0f - v);                       // gentle ease towards 1
                if (i > 0 && v < 0.06f) v = 0.06f;
                int o = static_cast<int>(v * 255.0f + 0.5f);
                lut[i] = static_cast<uint8_t>(o > 255 ? 255 : o);
            }
            const uint8_t* src = static_cast<const uint8_t*>(bits);
            for (int y = 0; y < texH; ++y) {
                uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.bits) + static_cast<size_t>(y) * lr.pitch);
                const uint8_t* row = src + static_cast<size_t>(y) * texW * 4;
                for (int x = 0; x < texW; ++x) {
                    const uint8_t cov = row[x * 4 + 1];   // grey: any channel
                    dst[x] = (static_cast<uint32_t>(lut[cov]) << 24) | 0x00FFFFFFu;
                }
            }
            // solid white block used by every non-text shape
            for (int y = 0; y < 4; ++y) {
                uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.bits) + static_cast<size_t>(y) * lr.pitch);
                for (int x = 0; x < 4; ++x) dst[x] = 0xFFFFFFFFu;
            }
            reinterpret_cast<FnUnlock>(vt[20])(tex, 0);
        } else {
            ok = false;
        }
    }
    SelectObject(dc, oldBmp);
    SelectObject(dc, oldFont);
    DeleteObject(dib);
    DeleteObject(font);
    DeleteDC(dc);
    if (!ok) {
        if (tex) d3d9::ComRelease(tex);
        DebugLog("[ui_kit] falha ao criar a textura da fonte\n");
        return false;
    }
    a.tex = tex;
    a.whiteU = 2.0f / static_cast<float>(texW);
    a.whiteV = 2.0f / static_cast<float>(texH);
    a.built = true;
    a.failed = false;
    return true;
}

static Atlas* EnsureAtlas(Font f) {
    if (!g_dev) return nullptr;
    if (g_atlasDev != g_dev) {            // new device: old textures belong to the old one
        for (auto& a : g_atlas) a = Atlas();
        g_atlasDev = g_dev;
    }
    Atlas& a = g_atlas[f];
    if (!a.built && !a.failed) BuildAtlas(g_dev, f, a);
    return a.built ? &a : nullptr;
}

// ============================================================================
// Batched geometry
// ============================================================================
struct V { float x, y, z, rhw; DWORD c; float u, v; };
constexpr int kMaxVerts = 49152;
static V   g_v[kMaxVerts];
static int g_n = 0;
static float g_wu = 0, g_wv = 0;      // white texel of the atlas bound to the current batch

static void Flush() {
    if (g_n >= 3 && g_dev) {
        Atlas* a = EnsureAtlas(g_font);
        if (a) {
            g_dev->SetTexture(0, a->tex);
            g_dev->DrawPrimitiveUP(d3d9::D3DPT_TRIANGLELIST, static_cast<UINT>(g_n / 3), g_v, sizeof(V));
        }
    }
    g_n = 0;
}

static void UseFont(Font f) {
    if (f != g_font) { Flush(); g_font = f; }
    Atlas* a = EnsureAtlas(f);
    if (a) { g_wu = a->whiteU; g_wv = a->whiteV; }
}

static inline void Reserve(int n) { if (g_n + n > kMaxVerts) Flush(); }

static inline void Vtx(float x, float y, DWORD c) { g_v[g_n++] = {x - 0.5f, y - 0.5f, 0.0f, 1.0f, c, g_wu, g_wv}; }
static inline void VtxUV(float x, float y, DWORD c, float u, float v) { g_v[g_n++] = {x - 0.5f, y - 0.5f, 0.0f, 1.0f, c, u, v}; }

static inline float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static DWORD Transparent(DWORD c) { return c & 0x00FFFFFFu; }

// ---------------------------------------------------------------------------
// Rounded rectangle paths (clockwise, same point count for every inset of the same radius)
// ---------------------------------------------------------------------------
struct Pt { float x, y; };
constexpr int kMaxSeg = 8;
constexpr int kMaxPath = 4 * (kMaxSeg + 1);

static int SegFor(float r) { return r < 3.0f ? 2 : (r < 6.0f ? 4 : (r < 12.0f ? 6 : 8)); }

static int RRPath(Pt* out, float x, float y, float w, float h, float r, float inset) {
    const int seg = SegFor(r);
    static float tc[kMaxSeg + 1][2];
    static int tcSeg = 0;
    if (tcSeg != seg) {
        tcSeg = seg;
        for (int j = 0; j <= seg; ++j) {
            const double t = 1.5707963267948966 * j / seg;
            tc[j][0] = static_cast<float>(cos(t));
            tc[j][1] = static_cast<float>(sin(t));
        }
    }
    float rr = r - inset;
    const float ix = x + inset, iy = y + inset, iw = w - 2.0f * inset, ih = h - 2.0f * inset;
    if (rr < 0.0f) rr = 0.0f;
    if (rr > iw * 0.5f) rr = iw * 0.5f;
    if (rr > ih * 0.5f) rr = ih * 0.5f;
    if (rr < 0.0f) rr = 0.0f;
    const float cxs[4] = {ix + rr, ix + iw - rr, ix + iw - rr, ix + rr};
    const float cys[4] = {iy + rr, iy + rr, iy + ih - rr, iy + ih - rr};
    int n = 0;
    for (int q = 0; q < 4; ++q) {
        for (int j = 0; j <= seg; ++j) {
            const float c = tc[j][0], s = tc[j][1];
            float dx, dy;
            switch (q) {   // starting angles: TL 180, TR 270, BR 0, BL 90 degrees
                case 0:  dx = -c; dy = -s; break;
                case 1:  dx =  s; dy = -c; break;
                case 2:  dx =  c; dy =  s; break;
                default: dx = -s; dy =  c; break;
            }
            out[n++] = {cxs[q] + dx * rr, cys[q] + dy * rr};
        }
    }
    return n;
}

static DWORD GradAt(DWORD top, DWORD bot, float y0, float h, float y) {
    if (top == bot || h <= 0.0f) return top;
    return Mix(top, bot, Clamp01((y - y0) / h));
}

static void FillFan(const Pt* in, const Pt* edge, int n, DWORD top, DWORD bot, float y0, float h, float cx, float cy) {
    Reserve(n * 9);
    const DWORD cc = GradAt(top, bot, y0, h, cy);
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        Vtx(cx, cy, cc);
        Vtx(in[i].x, in[i].y, GradAt(top, bot, y0, h, in[i].y));
        Vtx(in[j].x, in[j].y, GradAt(top, bot, y0, h, in[j].y));
        // feather
        const DWORD ci = GradAt(top, bot, y0, h, in[i].y), cj = GradAt(top, bot, y0, h, in[j].y);
        Vtx(in[i].x, in[i].y, ci);
        Vtx(edge[i].x, edge[i].y, Transparent(ci));
        Vtx(in[j].x, in[j].y, cj);
        Vtx(in[j].x, in[j].y, cj);
        Vtx(edge[i].x, edge[i].y, Transparent(ci));
        Vtx(edge[j].x, edge[j].y, Transparent(cj));
    }
}

// quad strip between two paths; colours given per path
static void Strip(const Pt* a, DWORD ca, const Pt* b, DWORD cb, int n) {
    Reserve(n * 6);
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        Vtx(a[i].x, a[i].y, ca); Vtx(b[i].x, b[i].y, cb); Vtx(a[j].x, a[j].y, ca);
        Vtx(a[j].x, a[j].y, ca); Vtx(b[i].x, b[i].y, cb); Vtx(b[j].x, b[j].y, cb);
    }
}

static void HRect(float x, float y, float w, float h, DWORD cl, DWORD cr) {
    Reserve(6);
    Vtx(x, y, cl); Vtx(x + w, y, cr); Vtx(x, y + h, cl);
    Vtx(x + w, y, cr); Vtx(x + w, y + h, cr); Vtx(x, y + h, cl);
}

} // namespace

// ============================================================================
// Colour helpers
// ============================================================================
DWORD WithAlpha(DWORD c, float a) {
    float na = static_cast<float>(c >> 24) * a;
    if (na < 0.0f) na = 0.0f;
    if (na > 255.0f) na = 255.0f;
    return (static_cast<DWORD>(na + 0.5f) << 24) | (c & 0x00FFFFFFu);
}

DWORD Mix(DWORD a, DWORD b, float t) {
    t = Clamp01(t);
    DWORD out = 0;
    for (int s = 0; s <= 24; s += 8) {
        const float ca = static_cast<float>((a >> s) & 0xFF), cb = static_cast<float>((b >> s) & 0xFF);
        out |= static_cast<DWORD>(ca + (cb - ca) * t + 0.5f) << s;
    }
    return out;
}

// ============================================================================
// Frame
// ============================================================================
void BeginFrame(Dev* dev, const Input& in) {
    g_dev = dev;
    g_in = in;
    g_n = 0;
    if (!in.down) g_activeSlider = 0;
    g_font = F_BODY;
    InitGlyphTable();
    Atlas* a = EnsureAtlas(g_font);
    if (a) { g_wu = a->whiteU; g_wv = a->whiteV; }
    if (!dev) return;

    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(d3d9::D3DFVF_XYZRHW | d3d9::D3DFVF_DIFFUSE | 0x100 /*TEX1*/);
    dev->SetRenderState(d3d9::D3DRS_FILLMODE, d3d9::D3DFILL_SOLID);
    dev->SetRenderState(d3d9::D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(d3d9::D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(d3d9::D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(d3d9::D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(d3d9::D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(d3d9::D3DRS_CULLMODE, d3d9::D3DCULL_NONE);
    dev->SetRenderState(d3d9::D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(d3d9::D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(d3d9::D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(d3d9::D3DRS_COLORWRITEENABLE, 0x0Fu);
    dev->SetRenderState(d3d9::RS_SRGBWRITEENABLE, 0);
    dev->SetRenderState(d3d9::RS_SEPARATEALPHABLEND, 0);
    dev->SetRenderState(d3d9::D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(d3d9::D3DRS_SRCBLEND, d3d9::D3DBLEND_SRCALPHA);
    dev->SetRenderState(d3d9::D3DRS_DESTBLEND, d3d9::D3DBLEND_INVSRCALPHA);
    // stage 0: texture * diffuse (colour and alpha); stage 1 off
    dev->SetTextureStageState(0, 1 /*COLOROP*/, 4 /*MODULATE*/);
    dev->SetTextureStageState(0, 2 /*COLORARG1*/, 2 /*TEXTURE*/);
    dev->SetTextureStageState(0, 3 /*COLORARG2*/, 0 /*DIFFUSE*/);
    dev->SetTextureStageState(0, 4 /*ALPHAOP*/, 4 /*MODULATE*/);
    dev->SetTextureStageState(0, 5 /*ALPHAARG1*/, 2 /*TEXTURE*/);
    dev->SetTextureStageState(0, 6 /*ALPHAARG2*/, 0 /*DIFFUSE*/);
    dev->SetTextureStageState(1, 1 /*COLOROP*/, 1 /*DISABLE*/);
    dev->SetTextureStageState(1, 4 /*ALPHAOP*/, 1 /*DISABLE*/);
    using FnSamp = HRESULT(STDMETHODCALLTYPE*)(Dev*, DWORD, DWORD, DWORD);
    auto ss = reinterpret_cast<FnSamp>(dev->vtable[d3d9::slot::SetSamplerState]);
    ss(dev, 0, d3d9::SAMP_ADDRESSU, d3d9::TADDRESS_CLAMP);
    ss(dev, 0, d3d9::SAMP_ADDRESSV, d3d9::TADDRESS_CLAMP);
    ss(dev, 0, d3d9::SAMP_MINFILTER, d3d9::TEXF_LINEAR);
    ss(dev, 0, d3d9::SAMP_MAGFILTER, d3d9::TEXF_LINEAR);
    ss(dev, 0, d3d9::SAMP_MIPFILTER, d3d9::TEXF_NONE);
    ss(dev, 0, d3d9::SAMP_SRGBTEXTURE, 0);
}

void EndFrame() { Flush(); }

void Image(void* tex, float x, float y, float w, float h, DWORD tint) {
    if (!g_dev || !tex) return;
    Flush();
    const float x1 = x + w, y1 = y + h;
    V q[6] = {
        {x - 0.5f, y - 0.5f, 0.0f, 1.0f, tint, 0.0f, 0.0f}, {x1 - 0.5f, y - 0.5f, 0.0f, 1.0f, tint, 1.0f, 0.0f}, {x - 0.5f, y1 - 0.5f, 0.0f, 1.0f, tint, 0.0f, 1.0f},
        {x1 - 0.5f, y - 0.5f, 0.0f, 1.0f, tint, 1.0f, 0.0f}, {x1 - 0.5f, y1 - 0.5f, 0.0f, 1.0f, tint, 1.0f, 1.0f}, {x - 0.5f, y1 - 0.5f, 0.0f, 1.0f, tint, 0.0f, 1.0f},
    };
    g_dev->SetTexture(0, tex);
    g_dev->DrawPrimitiveUP(d3d9::D3DPT_TRIANGLELIST, 2, q, sizeof(V));
}
const Input& GetInput() { return g_in; }

// ============================================================================
// Shapes
// ============================================================================
void Rect(float x, float y, float w, float h, DWORD c) {
    UseFont(g_font);
    Reserve(6);
    Vtx(x, y, c); Vtx(x + w, y, c); Vtx(x, y + h, c);
    Vtx(x + w, y, c); Vtx(x + w, y + h, c); Vtx(x, y + h, c);
}

void RoundRect(float x, float y, float w, float h, float r, DWORD c) { RoundRect(x, y, w, h, r, c, c); }

void RoundRect(float x, float y, float w, float h, float r, DWORD top, DWORD bottom) {
    if (w <= 0.0f || h <= 0.0f) return;
    UseFont(g_font);
    Pt in[kMaxPath], edge[kMaxPath];
    const int n = RRPath(in, x, y, w, h, r, 0.5f);
    RRPath(edge, x, y, w, h, r, -0.5f);
    FillFan(in, edge, n, top, bottom, y, h, x + w * 0.5f, y + h * 0.5f);
}

void RoundOutline(float x, float y, float w, float h, float r, float thick, DWORD c) {
    if (w <= 0.0f || h <= 0.0f) return;
    UseFont(g_font);
    Pt p0[kMaxPath], p1[kMaxPath], p2[kMaxPath], p3[kMaxPath];
    const int n = RRPath(p0, x, y, w, h, r, -0.5f);
    RRPath(p1, x, y, w, h, r, 0.5f);
    RRPath(p2, x, y, w, h, r, thick - 0.5f);
    RRPath(p3, x, y, w, h, r, thick + 0.5f);
    const DWORD t = Transparent(c);
    Strip(p0, t, p1, c, n);
    if (thick > 1.0f) Strip(p1, c, p2, c, n);
    Strip(p2, c, p3, t, n);
}

void Shadow(float x, float y, float w, float h, float r, float blur, DWORD c) {
    UseFont(g_font);
    Pt p0[kMaxPath], p1[kMaxPath], p2[kMaxPath];
    const int n = RRPath(p0, x, y, w, h, r, 0.0f);
    RRPath(p1, x, y, w, h, r, -blur * 0.4f);
    RRPath(p2, x, y, w, h, r, -blur);
    // centre fan
    Reserve(n * 3);
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        Vtx(x + w * 0.5f, y + h * 0.5f, c); Vtx(p0[i].x, p0[i].y, c); Vtx(p0[j].x, p0[j].y, c);
    }
    Strip(p0, c, p1, WithAlpha(c, 0.45f), n);
    Strip(p1, WithAlpha(c, 0.45f), p2, Transparent(c), n);
}

void Diamond(float cx, float cy, float r, DWORD base) {
    UseFont(g_font);
    const float hh = r * 1.45f;
    const DWORD light = Mix(base, Col(255, 255, 255, 255), 0.45f);
    const DWORD mid   = base;
    const DWORD dark  = Mix(base, Col(0, 20, 10, 255), 0.45f);
    const DWORD mid2  = Mix(base, Col(0, 20, 10, 255), 0.2f);
    Reserve(12);
    const float tx = cx, ty = cy - hh, bx = cx, by = cy + hh, lx = cx - r, rx = cx + r;
    Vtx(tx, ty, light); Vtx(lx, cy, mid);  Vtx(cx, cy, mid);
    Vtx(tx, ty, mid);   Vtx(cx, cy, light); Vtx(rx, cy, light);
    Vtx(bx, by, mid2);  Vtx(lx, cy, mid2); Vtx(cx, cy, mid);
    Vtx(bx, by, dark);  Vtx(cx, cy, mid2); Vtx(rx, cy, mid);
}

// ============================================================================
// Text
// ============================================================================
float FontHeight(Font f) {
    Atlas* a = EnsureAtlas(f);
    return a ? a->height : 14.0f;
}

static bool g_translate = true;
void SetTranslate(bool on) { g_translate = on; }

float TextWidth(Font f, const char* s) {
    Atlas* a = EnsureAtlas(f);
    if (!a || !s) return 0.0f;
    if (g_translate) s = i18n::Tr(s);
    float w = 0.0f;
    for (const char* p = s; *p;) {
        const uint32_t cp = NextCp(p);
        if (cp == '\n') continue;
        w += a->adv[GlyphIndex(cp)];
    }
    return w;
}

void Text(Font f, float x, float y, const char* s, DWORD c) {
    if (!s || !*s) return;
    if (g_translate) s = i18n::Tr(s);
    UseFont(f);
    Atlas* a = EnsureAtlas(f);
    if (!a) return;
    float pen = static_cast<float>(static_cast<int>(x + 0.5f));
    const float top = static_cast<float>(static_cast<int>(y + 0.5f)) - 1.0f;
    const float itw = 1.0f / static_cast<float>(a->texW), ith = 1.0f / static_cast<float>(a->texH);
    for (const char* p = s; *p;) {
        const uint32_t cp = NextCp(p);
        if (cp == '\n') continue;
        const int gi = GlyphIndex(cp);
        if (cp != ' ') {
            Reserve(6);
            const float x0 = pen - 1.0f, x1 = x0 + a->gw[gi], y0 = top, y1 = top + a->cellH;
            const float u0 = a->gx[gi] * itw, u1 = (a->gx[gi] + a->gw[gi]) * itw;
            const float v0 = a->gy[gi] * ith, v1 = (a->gy[gi] + a->cellH) * ith;
            VtxUV(x0, y0, c, u0, v0); VtxUV(x1, y0, c, u1, v0); VtxUV(x0, y1, c, u0, v1);
            VtxUV(x1, y0, c, u1, v0); VtxUV(x1, y1, c, u1, v1); VtxUV(x0, y1, c, u0, v1);
        }
        pen += a->adv[gi];
    }
}

void TextRight(Font f, float rightX, float y, const char* s, DWORD c) { Text(f, rightX - TextWidth(f, s), y, s, c); }
void TextCenter(Font f, float cx, float y, const char* s, DWORD c) { Text(f, cx - TextWidth(f, s) * 0.5f, y, s, c); }

void TextFit(Font f, float x, float y, float maxW, const char* s, DWORD c) {
    if (!s) return;
    if (g_translate) s = i18n::Tr(s);
    Atlas* a = EnsureAtlas(f);
    if (!a) return;
    if (TextWidth(f, s) <= maxW) { Text(f, x, y, s, c); return; }
    const float ell = a->adv[GlyphIndex(0x2026)];
    char buf[260];
    size_t good = 0;
    float w = 0.0f;
    for (const char* p = s; *p;) {
        const char* start = p;
        const uint32_t cp = NextCp(p);
        const float aw = a->adv[GlyphIndex(cp)];
        if (w + aw + ell > maxW) break;
        w += aw;
        good = static_cast<size_t>(p - s);
        (void)start;
    }
    if (good > sizeof(buf) - 5) good = sizeof(buf) - 5;
    memcpy(buf, s, good);
    buf[good] = '\xE2'; buf[good + 1] = '\x80'; buf[good + 2] = '\xA6'; buf[good + 3] = 0;
    Text(f, x, y, buf, c);
}

// ============================================================================
// Widgets
// ============================================================================
bool Hover(float x, float y, float w, float h) {
    return g_in.mx >= x && g_in.mx <= x + w && g_in.my >= y && g_in.my <= y + h;
}

bool Button(float x, float y, float w, float h, const char* label, bool active, Font f) {
    const bool hov = Hover(x, y, w, h);
    const bool press = hov && g_in.down;
    DWORD top, bot;
    if (active) {
        top = hov ? Col(86, 172, 255) : Col(70, 156, 250);
        bot = hov ? Col(56, 128, 240) : Col(44, 110, 222);
    } else {
        top = hov ? Col(66, 80, 114, 240) : Col(50, 60, 86, 232);
        bot = hov ? Col(52, 64, 94, 240)  : Col(38, 46, 68, 232);
    }
    if (press) { top = Mix(top, Col(0, 0, 0), 0.15f); bot = Mix(bot, Col(0, 0, 0), 0.15f); }
    RoundRect(x, y, w, h, 8.0f, top, bot);
    if (active) RoundOutline(x, y, w, h, 8.0f, 1.0f, Col(255, 255, 255, 60));
    else        RoundOutline(x, y, w, h, 8.0f, 1.0f, hov ? Col(120, 190, 255, 150) : Col(255, 255, 255, 22));
    const float fh = FontHeight(f);
    const float room = w - 16.0f;
    float tw = TextWidth(f, label);
    if (tw > room) tw = room;
    TextFit(f, x + (w - tw) * 0.5f, y + (h - fh) * 0.5f, room, label, active ? Col(255, 255, 255) : kText);
    return hov && g_in.clicked;
}

bool Toggle(float x, float y, bool on) {
    const float w = 40.0f, h = 22.0f;
    const bool hov = Hover(x, y, w, h);
    DWORD top, bot;
    if (on) { top = hov ? Col(90, 178, 255) : Col(72, 162, 252); bot = hov ? Col(60, 134, 242) : Col(46, 116, 226); }
    else    { top = hov ? Col(84, 96, 126) : Col(66, 76, 102); bot = hov ? Col(70, 82, 110) : Col(54, 62, 86); }
    RoundRect(x, y, w, h, 11.0f, top, bot);
    RoundOutline(x, y, w, h, 11.0f, 1.0f, Col(255, 255, 255, on ? 50 : 24));
    const float kx = on ? x + w - 11.0f : x + 11.0f, ky = y + h * 0.5f;
    RoundRect(kx - 9.5f, ky - 8.5f, 19.0f, 19.0f, 9.5f, Col(0, 0, 0, 70));
    RoundRect(kx - 8.0f, ky - 8.0f, 16.0f, 16.0f, 8.0f, Col(255, 255, 255), Col(214, 224, 242));
    return hov && g_in.clicked;
}

bool Slider(int id, float x, float y, float w, float* v, float lo, float hi) {
    const float cy = y + 11.0f;
    const bool hov = Hover(x - 8.0f, y, w + 16.0f, 22.0f);
    bool changed = false;
    if (hov && g_in.clicked && g_activeSlider == 0) g_activeSlider = id;
    if (g_activeSlider == id && g_in.down) {
        float t = Clamp01((g_in.mx - x) / w);
        float nv = lo + t * (hi - lo);
        if (nv != *v) { *v = nv; changed = true; }
    }
    float t = (hi > lo) ? Clamp01((*v - lo) / (hi - lo)) : 0.0f;
    RoundRect(x, cy - 3.0f, w, 6.0f, 3.0f, Col(20, 26, 42, 230), Col(34, 42, 62, 230));
    RoundRect(x, cy - 3.0f, w * t < 6.0f ? 6.0f : w * t, 6.0f, 3.0f, Col(96, 188, 255), Col(56, 132, 240));
    const float kx = x + w * t;
    const bool act = (g_activeSlider == id) || hov;
    RoundRect(kx - 10.0f, cy - 9.5f, 20.0f, 20.0f, 10.0f, Col(0, 0, 0, 60));
    RoundRect(kx - 8.0f, cy - 8.0f, 16.0f, 16.0f, 8.0f, act ? Col(255, 255, 255) : Col(236, 242, 252),
              act ? Col(214, 228, 250) : Col(196, 208, 230));
    RoundRect(kx - 3.0f, cy - 3.0f, 6.0f, 6.0f, 3.0f, Col(60, 140, 240));
    return changed;
}

void Pill(float x, float y, const char* s, DWORD tint, float* outWidth, bool rightAlignedAtX) {
    const float w = TextWidth(F_BOLD, s) + 22.0f, h = 22.0f;
    if (outWidth) *outWidth = w;
    const float x0 = rightAlignedAtX ? x - w : x;
    RoundRect(x0, y, w, h, 11.0f, WithAlpha(tint, 0.20f), WithAlpha(tint, 0.10f));
    RoundOutline(x0, y, w, h, 11.0f, 1.0f, WithAlpha(tint, 0.55f));
    Text(F_BOLD, x0 + 11.0f, y + (h - FontHeight(F_BOLD)) * 0.5f, s, tint);
}

int TabBar(float x, float y, float w, float h, const char* const* labels, int n, int active) {
    RoundRect(x, y, w, h, h * 0.5f, Col(8, 11, 20, 190), Col(14, 18, 30, 190));
    RoundOutline(x, y, w, h, h * 0.5f, 1.0f, Col(255, 255, 255, 20));
    const float pad = 4.0f;
    const float iw = (w - pad * 2.0f) / n;
    int clicked = -1;
    for (int i = 0; i < n; ++i) {
        const float ix = x + pad + i * iw, iy = y + pad, ih = h - pad * 2.0f;
        const bool hov = Hover(ix, iy, iw, ih);
        if (i == active) {
            RoundRect(ix, iy, iw, ih, ih * 0.5f, Col(84, 170, 255), Col(52, 120, 232));
            RoundOutline(ix, iy, iw, ih, ih * 0.5f, 1.0f, Col(255, 255, 255, 70));
        } else if (hov) {
            RoundRect(ix, iy, iw, ih, ih * 0.5f, Col(255, 255, 255, 26));
        }
        const Font f = (i == active) ? F_BOLD : F_BODY;
        TextCenter(f, ix + iw * 0.5f, iy + (ih - FontHeight(f)) * 0.5f, labels[i], i == active ? Col(255, 255, 255) : (hov ? kText : kDim));
        if (hov && g_in.clicked) clicked = i;
    }
    return clicked;
}

void WindowFrame(float x, float y, float w, float h) {
    Shadow(x + 2.0f, y + 10.0f, w - 4.0f, h - 4.0f, 18.0f, 34.0f, Col(0, 0, 0, 120));
    RoundRect(x, y, w, h, 18.0f, Col(30, 38, 60, 246), Col(13, 17, 29, 246));
    RoundOutline(x, y, w, h, 18.0f, 1.0f, Col(255, 255, 255, 34));
    // subtle sheen along the top
    RoundRect(x + 1.0f, y + 1.0f, w - 2.0f, 54.0f, 17.0f, Col(255, 255, 255, 16), Col(255, 255, 255, 0));
}

// ============================================================================
// Layout
// ============================================================================
void LayBegin(float x, float y, float w) { g_lay.x = x; g_lay.y = y; g_lay.w = w; g_lay.first = true; }

void LaySection(const char* title, const char* right, DWORD rightColor) {
    if (!g_lay.first) g_lay.y += 12.0f;
    g_lay.first = false;
    const float fh = FontHeight(F_BOLD);
    RoundRect(g_lay.x, g_lay.y + 2.0f, 3.0f, fh - 4.0f, 1.5f, Col(120, 205, 255), Col(70, 140, 245));
    Text(F_BOLD, g_lay.x + 10.0f, g_lay.y, title, Col(190, 220, 255));
    if (right && right[0]) TextRight(F_SMALL, g_lay.x + g_lay.w, g_lay.y + (fh - FontHeight(F_SMALL)) * 0.5f, right, rightColor);
    HRect(g_lay.x, g_lay.y + fh + 5.0f, g_lay.w, 1.0f, Col(120, 190, 255, 90), Col(120, 190, 255, 0));
    g_lay.y += fh + 12.0f;
}

void LayText(const char* text, DWORD color, Font f) {
    const float lh = FontHeight(f) + 3.0f;
    char line[300];
    const char* p = text;
    float y = g_lay.y;
    while (*p) {
        size_t n = 0;
        while (p[n] && p[n] != '\n' && n < sizeof(line) - 1) ++n;
        memcpy(line, p, n);
        line[n] = 0;
        TextFit(f, g_lay.x, y, g_lay.w, line, color);
        y += lh;
        p += n;
        if (*p == '\n') ++p;
        else if (n == sizeof(line) - 1) continue;
    }
    g_lay.y = y + 3.0f;
}

void LayNext() { g_lay.y += kRowH + kGap; }

bool LayBtn(int col, int cols, const char* label, bool active, int span) {
    const float cw = (g_lay.w - kGap * (cols - 1)) / cols;
    const float x = g_lay.x + col * (cw + kGap);
    const float w = cw * span + kGap * (span - 1);
    return Button(x, g_lay.y, w, kRowH, label, active);
}

bool FxRow(const char* name, bool* on, float* val, float lo, float hi, float def, int decimals) {
    const float rowH = 34.0f;
    const float x = g_lay.x, y = g_lay.y, w = g_lay.w;
    bool changed = false;
    RoundRect(x, y, w, rowH, 9.0f, Col(255, 255, 255, 16), Col(255, 255, 255, 9));
    if (Toggle(x + 10.0f, y + 6.0f, *on)) { *on = !*on; changed = true; }
    const float fh = FontHeight(F_BODY);
    TextFit(F_BODY, x + 60.0f, y + (rowH - fh) * 0.5f, 168.0f, name, *on ? kText : kDim);
    if (val) {
        int id = 5381;
        for (const char* p = name; *p; ++p) id = id * 33 + static_cast<unsigned char>(*p);
        const float sx = x + 236.0f, sw = w - 236.0f - 160.0f;
        if (Slider(id, sx, y + 6.0f, sw, val, lo, hi)) changed = true;
        char buf[24];
        double shown = static_cast<double>(*val);
        _snprintf(buf, sizeof(buf), decimals >= 4 ? "%.4f" : (decimals == 3 ? "%.3f" : "%.2f"), shown);
        TextRight(F_BODY, x + w - 76.0f, y + (rowH - fh) * 0.5f, buf, *on ? kText : kDim);
        if (Button(x + w - 68.0f, y + 6.0f, 60.0f, 22.0f, "Padrão", false, F_SMALL)) { *val = def; changed = true; }
    }
    g_lay.y += rowH + 4.0f;
    return changed;
}

bool ValRow(const char* name, float* val, float lo, float hi, float def, int decimals) {
    const float rowH = 34.0f;
    const float x = g_lay.x, y = g_lay.y, w = g_lay.w;
    bool changed = false;
    RoundRect(x, y, w, rowH, 9.0f, Col(255, 255, 255, 16), Col(255, 255, 255, 9));
    const float fh = FontHeight(F_BODY);
    TextFit(F_BODY, x + 16.0f, y + (rowH - fh) * 0.5f, 212.0f, name, kText);
    int id = 7919;
    for (const char* p = name; *p; ++p) id = id * 33 + static_cast<unsigned char>(*p);
    const float sx = x + 236.0f, sw = w - 236.0f - 160.0f;
    if (Slider(id, sx, y + 6.0f, sw, val, lo, hi)) changed = true;
    char buf[24];
    _snprintf(buf, sizeof(buf), decimals >= 4 ? "%.4f" : (decimals == 3 ? "%.3f" : "%.2f"), static_cast<double>(*val));
    TextRight(F_BODY, x + w - 76.0f, y + (rowH - fh) * 0.5f, buf, kText);
    if (Button(x + w - 68.0f, y + 6.0f, 60.0f, 22.0f, "Padrão", false, F_SMALL)) { *val = def; changed = true; }
    g_lay.y += rowH + 4.0f;
    return changed;
}

} // namespace ts2::ui::kit
