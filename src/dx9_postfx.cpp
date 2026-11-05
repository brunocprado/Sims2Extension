#include "ts2/dx9_postfx.hpp"
#include "ts2/hook_engine.hpp"
#include "ts2/ts2se_engine.hpp"
#include "ts2/postfx_shaders_gen.hpp"

namespace ts2::fx {

namespace {

using d3d9::IDirect3DDevice9;
using d3d9::D3DMATRIX;

// ============================================================================
// Settings / config file
// ============================================================================
static Settings g_set;
static Status   g_status = {};

static bool ParseBool(const char* v) {
    return v && (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y' || _stricmp(v, "on") == 0);
}

// Applies "key value"; returns false for unknown keys.
static bool ApplyKV(const char* key, const char* val) {
    struct F { const char* n; float* p; };
    struct B { const char* n; bool* p; };
    const B bools[] = {
        {"enabled", &g_set.enabled}, {"ao", &g_set.ao}, {"aoblur", &g_set.aoBlur},
        {"contactshadows", &g_set.contactShadows}, {"sunauto", &g_set.sunAuto},
        {"fxaa", &g_set.fxaa}, {"bloom", &g_set.bloom}, {"probe", &g_set.probe},
        {"fog", &g_set.fog}, {"shafts", &g_set.shafts}, {"ssr", &g_set.ssr},
        {"tonemap", &g_set.tonemap}, {"timegrade", &g_set.timeGrade},
    };
    const F floats[] = {
        {"aoradius", &g_set.aoRadius}, {"aointensity", &g_set.aoIntensity}, {"aopower", &g_set.aoPower},
        {"shadowstrength", &g_set.shadowStrength}, {"shadowlength", &g_set.shadowLength},
        {"sunazimuth", &g_set.sunAzimuth}, {"sunelevation", &g_set.sunElevation},
        {"bloomstrength", &g_set.bloomStrength}, {"bloomthreshold", &g_set.bloomThreshold},
        {"vignette", &g_set.vignette}, {"saturation", &g_set.saturation}, {"contrast", &g_set.contrast},
        {"exposure", &g_set.exposure}, {"sunyaw", &g_set.sunYaw},
        {"fogdensity", &g_set.fogDensity}, {"fogstart", &g_set.fogStart}, {"fogmax", &g_set.fogMax}, {"fogscatter", &g_set.fogScatter},
        {"shaftstrength", &g_set.shaftStrength}, {"shaftthreshold", &g_set.shaftThreshold},
        {"ssrstrength", &g_set.ssrStrength}, {"ssrf0", &g_set.ssrF0}, {"ssrdistance", &g_set.ssrDistance}, {"ssrthickness", &g_set.ssrThickness},
        {"tonemapstrength", &g_set.tonemapStrength}, {"tonemapexposure", &g_set.tonemapExposure}, {"gradestrength", &g_set.gradeStrength},
    };
    for (const auto& b : bools) if (_stricmp(key, b.n) == 0) { *b.p = ParseBool(val); return true; }
    for (const auto& f : floats) if (_stricmp(key, f.n) == 0) { *f.p = static_cast<float>(strtod(val, nullptr)); return true; }
    if (_stricmp(key, "debugview") == 0) { g_set.debugView = static_cast<int>(strtol(val, nullptr, 10)); return true; }
    return false;
}

static const char* kConfigName = "ts2fx.cfg";
static DWORD g_cfgStamp = 0;
static bool  g_dirty = false;          // settings changed from the UI, waiting to be written
static uint32_t g_dirtyFrame = 0;

static bool ReadWholeFile(const char* path, char* buf, size_t cap, DWORD* outLen) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, static_cast<DWORD>(cap - 1), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    buf[got] = 0;
    if (outLen) *outLen = got;
    return true;
}

static void ParseConfigText(char* text) {
    char* p = text;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') ++p;
        if (*p) *p++ = 0;
        while (*line == ' ' || *line == '\t' || *line == '\r') ++line;
        if (!*line || *line == '#' || *line == ';' || *line == '/') continue;
        char* eq = line;
        while (*eq && *eq != '=' && *eq != ' ' && *eq != '\t') ++eq;
        if (!*eq) continue;
        char* val = eq;
        *eq = 0;
        ++val;
        while (*val == ' ' || *val == '\t' || *val == '=') ++val;
        char* end = val + strlen(val);
        while (end > val && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (!ApplyKV(line, val)) ts2::hook::Log("[FX] ts2fx.cfg: chave desconhecida '%s'", line);
    }
}

// ============================================================================
// Device state tracking
// ============================================================================
static void*  g_vt[128]      = {};   // original IDirect3DDevice9 vtable entries (pre-hook)
static bool   g_hooked       = false;
static IDirect3DDevice9* g_dev = nullptr;
static uint32_t g_frame      = 0;

template <typename Fn> static Fn Orig(int slot) { return reinterpret_cast<Fn>(g_vt[slot]); }

struct TrackedState {
    DWORD rs[256];
    D3DMATRIX world, view, proj;
    bool  haveProj;
    void* rt0;
    void* ds;
    void* vs;
    void* ps;
    DWORD fvf;
    void* decl;
    float vsc[256][4];           // shadow copy of the vertex shader float constants
    bool  vscSeen[256];
};
static TrackedState g_ts = {};

// ============================================================================
// Probe (one frame of D3D9 calls -> ts2fx_probe_<pid>.log)
// ============================================================================
static bool     g_probeOn    = false;
static char*    g_probeBuf   = nullptr;
static size_t   g_probeLen   = 0;
static constexpr size_t kProbeCap = 6u << 20;
static uint32_t g_probeDraws = 0;
static uint32_t g_probeVscDumps = 0;
static uint32_t g_probeVscDumpsBig = 0;

static void Pl(const char* fmt, ...) {
    if (!g_probeOn || !g_probeBuf) return;
    if (g_probeLen + 600 > kProbeCap) return;
    va_list a;
    va_start(a, fmt);
    int n = _vsnprintf(g_probeBuf + g_probeLen, 560, fmt, a);
    va_end(a);
    if (n < 0 || n > 560) n = 560;
    g_probeBuf[g_probeLen + n] = '\n';
    g_probeLen += n + 1;
}

static const char* FmtName(DWORD f, char* tmp) {
    switch (f) {
        case 21: return "A8R8G8B8"; case 22: return "X8R8G8B8"; case 23: return "R5G6B5";
        case 75: return "D24S8"; case 77: return "D24X8"; case 80: return "D16"; case 71: return "D32";
        case 111: return "R16F"; case 112: return "G16R16F";
        case 113: return "A16B16G16R16F"; case 114: return "R32F"; case 116: return "A32B32G32R32F";
        case 0x5A544E49: return "INTZ";
    }
    _snprintf(tmp, 16, "fmt%u", f);
    return tmp;
}

static void SurfStr(void* s, char* out, size_t cap) {
    if (!s) { _snprintf(out, cap, "null"); return; }
    d3d9::D3DSURFACE_DESC d = {};
    char t[16];
    if (d3d9::SurfaceGetDesc(s, &d) >= 0)
        _snprintf(out, cap, "%p %ux%u %s ms=%u usage=%u", s, d.Width, d.Height, FmtName(d.Format, t), d.MultiSampleType, d.Usage);
    else
        _snprintf(out, cap, "%p (desc?)", s);
}

static void ProbeFlush() {
    if (!g_probeBuf || g_probeLen == 0) return;
    char name[64];
    _snprintf(name, sizeof(name), "ts2fx_probe_%u.log", static_cast<unsigned>(GetCurrentProcessId()));
    HANDLE h = CreateFileA(name, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, g_probeBuf, static_cast<DWORD>(g_probeLen), &w, nullptr);
        CloseHandle(h);
        ts2::hook::Log("[FX] probe escrita: %s (%u bytes, %u draws)", name, static_cast<unsigned>(g_probeLen), g_probeDraws);
    }
    g_probeLen = 0;
}

// ============================================================================
// GPU resources
// ============================================================================
struct Tex {
    void* tex  = nullptr;
    void* surf = nullptr;
    UINT  w = 0, h = 0;
    DWORD fmt = 0;
};

struct Resources {
    UINT  w = 0, h = 0;
    DWORD bbFormat = 0;
    DWORD aoFormat = 0;
    Tex   intz, scene, ao[2], tmp[2], bloom[2], shaft[2];
    void* psAtmos = nullptr;
    void* psShafts = nullptr;
    void* vs = nullptr;
    void* psAO = nullptr;
    void* psBlur = nullptr;
    void* psComposite = nullptr;
    void* psFxaa = nullptr;
    void* psBright = nullptr;
    void* psBloomBlur = nullptr;
    void* psFinal = nullptr;
    void* decl = nullptr;
    void* stateBlock = nullptr;
    bool  ready = false;
    bool  failed = false;
    DWORD retryTick = 0;
};
static Resources g_r;
static void*     g_mainRT   = nullptr;   // current back buffer surface (identity compare only)
static void*     g_gameDS   = nullptr;   // the game's own D24S8 depth surface (replaced by INTZ)
static UINT      g_bbW = 0, g_bbH = 0;
static DWORD     g_bbFmt = 0;
static bool      g_intzSupported = false;
static bool      g_capsChecked = false;

static void FreeTex(Tex& t) {
    if (t.surf) d3d9::ComRelease(t.surf);
    if (t.tex)  d3d9::ComRelease(t.tex);
    t = Tex();
}

static bool MakeTex(Tex& t, UINT w, UINT h, DWORD usage, DWORD fmt) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, void*);
    void* tex = nullptr;
    HRESULT hr = Orig<Fn>(d3d9::slot::CreateTexture)(g_dev, w, h, 1, usage, fmt, d3d9::POOL_DEFAULT, &tex, nullptr);
    if (hr < 0 || !tex) {
        ts2::hook::Log("[FX] CreateTexture %ux%u fmt=%u usage=%u falhou (0x%08X)", w, h, fmt, usage, static_cast<unsigned>(hr));
        return false;
    }
    void* surf = nullptr;
    if (d3d9::TextureGetSurfaceLevel(tex, 0, &surf) < 0 || !surf) {
        d3d9::ComRelease(tex);
        return false;
    }
    t.tex = tex; t.surf = surf; t.w = w; t.h = h; t.fmt = fmt;
    return true;
}

static void* GetD3D() {
    void* d3d = nullptr;
    using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, void**);
    if (Orig<Fn>(d3d9::slot::GetDirect3D)(g_dev, &d3d) < 0) return nullptr;
    return d3d;
}

static bool CheckFormat(void* d3d, DWORD usage, DWORD fmt) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, DWORD, DWORD, DWORD, DWORD, DWORD);
    auto* u = static_cast<d3d9::IUnknownLike*>(d3d);
    DWORD adapterFmt = g_bbFmt ? g_bbFmt : d3d9::FMT_X8R8G8B8;
    return reinterpret_cast<Fn>(u->vtable[d3d9::d3dslot::CheckDeviceFormat])(d3d, 0, 1 /*HAL*/, adapterFmt, usage, d3d9::RTYPE_TEXTURE, fmt) >= 0;
}

static void ReleaseResources() {
    FreeTex(g_r.intz); FreeTex(g_r.scene);
    for (auto& t : g_r.ao) FreeTex(t);
    for (auto& t : g_r.tmp) FreeTex(t);
    for (auto& t : g_r.bloom) FreeTex(t);
    for (auto& t : g_r.shaft) FreeTex(t);
    if (g_r.stateBlock) { d3d9::ComRelease(g_r.stateBlock); g_r.stateBlock = nullptr; }
    g_r.ready = false;
    g_status.depthOk = false;
    g_status.pipelineOk = false;
}

static void* MakeShader(int slot, const uint32_t* code) {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, const uint32_t*, void**);
    void* s = nullptr;
    if (Orig<Fn>(slot)(g_dev, code, &s) < 0) return nullptr;
    return s;
}

static void QueryBackbuffer() {
    using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, UINT, DWORD, void**);
    void* bb = nullptr;
    if (Orig<Fn>(d3d9::slot::GetBackBuffer)(g_dev, 0, 0, 0, &bb) >= 0 && bb) {
        d3d9::D3DSURFACE_DESC d = {};
        if (d3d9::SurfaceGetDesc(bb, &d) >= 0) { g_bbW = d.Width; g_bbH = d.Height; g_bbFmt = d.Format; }
        g_mainRT = bb;
        d3d9::ComRelease(bb);
    }
}

// Creates every GPU object the pipeline needs. Safe to call repeatedly.
static bool EnsureResources() {
    if (g_r.ready) return true;
    if (g_r.failed && GetTickCount() - g_r.retryTick < 4000) return false;
    g_r.failed = false;
    QueryBackbuffer();
    if (!g_bbW || !g_bbH) return false;

    void* d3d = GetD3D();
    if (!d3d) { g_r.failed = true; g_r.retryTick = GetTickCount(); return false; }
    g_intzSupported = CheckFormat(d3d, d3d9::USAGE_DEPTHSTENCIL, d3d9::FMT_INTZ);
    const bool rtF16 = CheckFormat(d3d, d3d9::USAGE_RENDERTARGET, d3d9::FMT_A16B16G16R16F);
    const bool rtF32 = CheckFormat(d3d, d3d9::USAGE_RENDERTARGET, d3d9::FMT_A32B32G32R32F);
    d3d9::ComRelease(d3d);
    if (!g_capsChecked) {
        g_capsChecked = true;
        ts2::hook::Log("[FX] caps: INTZ=%d RT_A16F=%d RT_A32F=%d backbuffer=%ux%u fmt=%u", g_intzSupported, rtF16, rtF32, g_bbW, g_bbH, g_bbFmt);
    }
    if (!g_intzSupported || (!rtF16 && !rtF32)) {
        _snprintf(g_status.message, sizeof(g_status.message), "GPU/driver sem INTZ ou RT float");
        g_r.failed = true; g_r.retryTick = GetTickCount() + 600000;
        return false;
    }

    g_r.w = g_bbW; g_r.h = g_bbH; g_r.bbFormat = g_bbFmt;
    g_r.aoFormat = rtF16 ? d3d9::FMT_A16B16G16R16F : d3d9::FMT_A32B32G32R32F;
    bool ok = MakeTex(g_r.intz, g_bbW, g_bbH, d3d9::USAGE_DEPTHSTENCIL, d3d9::FMT_INTZ);
    ok = ok && MakeTex(g_r.scene, g_bbW, g_bbH, d3d9::USAGE_RENDERTARGET, g_bbFmt);
    for (int i = 0; i < 2 && ok; ++i) ok = MakeTex(g_r.ao[i], g_bbW, g_bbH, d3d9::USAGE_RENDERTARGET, g_r.aoFormat);
    for (int i = 0; i < 2 && ok; ++i) ok = MakeTex(g_r.tmp[i], g_bbW, g_bbH, d3d9::USAGE_RENDERTARGET, g_bbFmt);
    const UINT bw = (g_bbW + 3) / 4, bh = (g_bbH + 3) / 4;
    for (int i = 0; i < 2 && ok; ++i) ok = MakeTex(g_r.bloom[i], bw, bh, d3d9::USAGE_RENDERTARGET, d3d9::FMT_A8R8G8B8);
    for (int i = 0; i < 2 && ok; ++i) ok = MakeTex(g_r.shaft[i], bw, bh, d3d9::USAGE_RENDERTARGET, d3d9::FMT_A8R8G8B8);

    if (ok) {
        g_r.vs          = MakeShader(d3d9::slot::CreateVertexShader, kShader_VS_Fullscreen);
        g_r.psAO        = MakeShader(d3d9::slot::CreatePixelShader, kShader_PS_AO);
        g_r.psBlur      = MakeShader(d3d9::slot::CreatePixelShader, kShader_PS_Blur);
        g_r.psComposite = MakeShader(d3d9::slot::CreatePixelShader, kShader_PS_Composite);
        g_r.psFxaa      = MakeShader(d3d9::slot::CreatePixelShader, kShader_PS_FXAA);
        g_r.psBright    = MakeShader(d3d9::slot::CreatePixelShader, kShader_PS_BloomBright);
        g_r.psBloomBlur = MakeShader(d3d9::slot::CreatePixelShader, kShader_PS_BloomBlur);
        g_r.psFinal     = MakeShader(d3d9::slot::CreatePixelShader, kShader_PS_Final);
        g_r.psAtmos     = MakeShader(d3d9::slot::CreatePixelShader, kShader_PS_Atmos);
        g_r.psShafts    = MakeShader(d3d9::slot::CreatePixelShader, kShader_PS_Shafts);
        ok = g_r.vs && g_r.psAO && g_r.psBlur && g_r.psComposite && g_r.psFxaa && g_r.psBright && g_r.psBloomBlur && g_r.psFinal &&
             g_r.psAtmos && g_r.psShafts;
        if (!ok) ts2::hook::Log("[FX] falha ao criar shaders SM3 (vs=%p ao=%p blur=%p comp=%p fxaa=%p bright=%p bb=%p fin=%p atmos=%p shafts=%p)",
                                g_r.vs, g_r.psAO, g_r.psBlur, g_r.psComposite, g_r.psFxaa, g_r.psBright, g_r.psBloomBlur, g_r.psFinal,
                                g_r.psAtmos, g_r.psShafts);
    }
    if (ok && !g_r.decl) {
        const d3d9::D3DVERTEXELEMENT9 elems[] = {
            {0, 0,  d3d9::DECLTYPE_FLOAT4, 0, d3d9::DECLUSAGE_POSITION, 0},
            {0, 16, d3d9::DECLTYPE_FLOAT2, 0, d3d9::DECLUSAGE_TEXCOORD, 0},
            {0xFF, 0, 17 /*UNUSED*/, 0, 0, 0},
        };
        using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, const d3d9::D3DVERTEXELEMENT9*, void**);
        ok = Orig<Fn>(d3d9::slot::CreateVertexDeclaration)(g_dev, elems, &g_r.decl) >= 0 && g_r.decl;
    }
    if (ok) {
        using Fn = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void**);
        ok = Orig<Fn>(d3d9::slot::CreateStateBlock)(g_dev, 1 /*D3DSBT_ALL*/, &g_r.stateBlock) >= 0 && g_r.stateBlock;
    }
    if (!ok) {
        ReleaseResources();
        g_r.failed = true;
        g_r.retryTick = GetTickCount();
        _snprintf(g_status.message, sizeof(g_status.message), "falha ao criar recursos do post-fx");
        return false;
    }
    g_r.ready = true;
    g_status.depthOk = true;
    g_status.pipelineOk = true;
    _snprintf(g_status.message, sizeof(g_status.message), "ativo (%ux%u)", g_bbW, g_bbH);
    ts2::hook::Log("[FX] recursos prontos: %ux%u, AO fmt=%u, shaders OK", g_bbW, g_bbH, g_r.aoFormat);
    return true;
}

// ============================================================================
// Math helpers
// ============================================================================
struct V3 { float x, y, z; };
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float Len(V3 a) { return static_cast<float>(sqrt(Dot(a, a))); }
static V3 Norm(V3 a) { float l = Len(a); return l > 1e-6f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1}; }
static float Abs(float v) { return v < 0 ? -v : v; }

struct ProjInfo {
    bool  valid;
    float m11, m22, m31, m32;   // ndc.x = x*m11/z + m31 ; ndc.y(up) = y*m22/z + m32
    float m33, m43;             // depth ndc = m33 + m43 / z
    int   depthAxis, rightAxis, upAxis;
};

// Extracts the (axis permuted) perspective terms from the game's projection matrix.
static ProjInfo ExtractProjection(const D3DMATRIX& M) {
    ProjInfo p = {};
    int a = -1;
    float best = 0;
    for (int i = 0; i < 3; ++i) if (Abs(M.m[i][3]) > best) { best = Abs(M.m[i][3]); a = i; }
    if (a < 0 || best < 1e-4f || Abs(M.m[3][3]) > 1e-4f) return p;   // not a perspective matrix
    int hx = -1, vy = -1;
    float bx = 0, by = 0;
    for (int i = 0; i < 3; ++i) {
        if (i == a) continue;
        if (Abs(M.m[i][0]) > bx) { bx = Abs(M.m[i][0]); hx = i; }
        if (Abs(M.m[i][1]) > by) { by = Abs(M.m[i][1]); vy = i; }
    }
    if (hx < 0 || vy < 0 || hx == vy) return p;
    const float w = M.m[a][3];
    p.m11 = M.m[hx][0] / w;
    p.m22 = M.m[vy][1] / w;
    p.m31 = M.m[a][0] / w;
    p.m32 = M.m[a][1] / w;
    p.m33 = M.m[a][2] / w;
    p.m43 = M.m[3][2] / w;
    p.depthAxis = a; p.rightAxis = hx; p.upAxis = vy;
    p.valid = (Abs(p.m11) > 1e-3f && Abs(p.m22) > 1e-3f && Abs(p.m43) > 1e-6f);
    return p;
}

// ----------------------------------------------------------------------------
// Camera basis (world space, Z up), learned from what the game uploads.
//
// Observed in the probe: the game never calls SetTransform(VIEW); the fixed-function WORLD matrix of every
// object already contains the camera, so its 3rd row is the world "up" axis in (permuted) view space
// and is identical for all upright objects. Shader-driven objects get the camera through the first four
// vertex constants (rows of the world->clip matrix): c0 = m11*right + m31*forward, c3 = forward.
// ----------------------------------------------------------------------------
struct CameraBasis { bool valid; V3 r, f, u; };
static CameraBasis g_cam = {};
static V3   g_upViewRaw = {0.0f, -0.24f, 0.97f};   // permuted view space: x right, y depth, z up
static bool g_haveUpView = false;

static void CaptureUpFromWorldMatrix(const D3DMATRIX& m) {
    const V3 row2 = {m.m[2][0], m.m[2][1], m.m[2][2]};
    const float l = Len(row2);
    if (Abs(row2.x) < 0.03f && l > 0.97f && l < 1.03f && row2.z > 0.3f && row2.y < 0.3f) {
        g_upViewRaw = row2;
        g_haveUpView = true;
    }
}

static void TryCaptureCamera() {
    if (!g_ts.haveProj) return;
    const ProjInfo pj = ExtractProjection(g_ts.proj);
    if (!pj.valid) return;
    const float* c0 = g_ts.vsc[0];
    const float* c3 = g_ts.vsc[3];
    V3 f = {c3[0], c3[1], c3[2]};
    const float lf = Len(f);
    if (lf < 0.96f || lf > 1.04f) return;
    f = V3{f.x / lf, f.y / lf, f.z / lf};
    V3 r = {(c0[0] - pj.m31 * c3[0]) / pj.m11, (c0[1] - pj.m31 * c3[1]) / pj.m11, (c0[2] - pj.m31 * c3[2]) / pj.m11};
    const float lr = Len(r);
    if (lr < 0.96f || lr > 1.04f) return;
    r = V3{r.x / lr, r.y / lr, r.z / lr};
    if (Abs(Dot(r, f)) > 0.06f) return;                       // not an orthonormal camera
    if (Abs(f.z) > 0.99f) return;
    if (g_haveUpView && Abs(f.z - g_upViewRaw.y) > 0.08f) return;   // pitch must agree with the fixed function path
    V3 u = {-f.z * f.x, -f.z * f.y, 1.0f - f.z * f.z};      // Z - (Z.f) f
    u = Norm(u);
    g_cam.valid = true; g_cam.r = r; g_cam.f = f; g_cam.u = u;
}

// World up expressed in "uv aligned" view space (+x right, +y down, +z forward).
static V3 WorldUpInView() {
    if (g_cam.valid) return V3{0.0f, -g_cam.u.z, g_cam.f.z};
    return V3{g_upViewRaw.x, -g_upViewRaw.z, g_upViewRaw.y};
}

// Converts a world space direction (Z up) to uv aligned view space; false when the camera is unknown.
static bool WorldDirToView(V3 w, V3& out) {
    if (!g_cam.valid) return false;
    out = V3{Dot(w, g_cam.r), -Dot(w, g_cam.u), Dot(w, g_cam.f)};
    return true;
}

// Time-of-day palette (fog / sky colour, sun colour, grade tint), interpolated between key hours.
struct SkyKey { float hour; float fog[3]; float sun[3]; float tint[3]; };
struct Sky { float fog[3]; float sun[3]; float tint[3]; };
static Sky TimeOfDaySky(float hour) {
    static const SkyKey keys[] = {
        { 0.0f, {0.05f, 0.08f, 0.18f}, {0.40f, 0.50f, 0.90f}, {0.84f, 0.90f, 1.10f}},
        { 5.5f, {0.07f, 0.10f, 0.22f}, {0.45f, 0.50f, 0.90f}, {0.86f, 0.91f, 1.08f}},
        { 6.8f, {0.95f, 0.62f, 0.45f}, {1.00f, 0.65f, 0.40f}, {1.07f, 0.98f, 0.90f}},
        { 9.0f, {0.70f, 0.80f, 0.92f}, {1.00f, 0.92f, 0.80f}, {1.02f, 1.00f, 0.97f}},
        {12.0f, {0.62f, 0.76f, 0.95f}, {1.00f, 0.97f, 0.90f}, {1.00f, 1.00f, 1.00f}},
        {16.0f, {0.72f, 0.80f, 0.90f}, {1.00f, 0.90f, 0.75f}, {1.02f, 1.00f, 0.96f}},
        {18.6f, {0.98f, 0.58f, 0.38f}, {1.00f, 0.55f, 0.30f}, {1.09f, 0.97f, 0.86f}},
        {20.8f, {0.18f, 0.16f, 0.30f}, {0.50f, 0.40f, 0.70f}, {0.90f, 0.92f, 1.06f}},
        {24.0f, {0.05f, 0.08f, 0.18f}, {0.40f, 0.50f, 0.90f}, {0.84f, 0.90f, 1.10f}},
    };
    constexpr int n = sizeof(keys) / sizeof(keys[0]);
    if (hour < 0.0f) hour = 0.0f;
    if (hour > 24.0f) hour = 24.0f;
    int i = 0;
    while (i < n - 2 && hour > keys[i + 1].hour) ++i;
    const SkyKey& a = keys[i];
    const SkyKey& b = keys[i + 1];
    float t = (hour - a.hour) / (b.hour - a.hour);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    t = t * t * (3.0f - 2.0f * t);
    Sky s;
    for (int k = 0; k < 3; ++k) {
        s.fog[k]  = a.fog[k]  + (b.fog[k]  - a.fog[k])  * t;
        s.sun[k]  = a.sun[k]  + (b.sun[k]  - a.sun[k])  * t;
        s.tint[k] = a.tint[k] + (b.tint[k] - a.tint[k]) * t;
    }
    return s;
}

static float CurrentHour() {
    uint16_t hr = 12, mn = 0, sub = 0, day = 0;
    ts2::se::GetSimulatorClock(hr, mn, sub, day);
    return static_cast<float>(hr) + static_cast<float>(mn) / 60.0f;
}

// Sun direction in "world" space (Z up): returns the vector pointing towards the light.
static V3 SunWorld(float& strength) {
    float az = g_set.sunAzimuth, el = g_set.sunElevation;
    strength = 1.0f;
    if (g_set.sunAuto) {
        uint16_t hr = 12, mn = 0, sub = 0, day = 0;
        ts2::se::GetSimulatorClock(hr, mn, sub, day);
        const float h = static_cast<float>(hr) + static_cast<float>(mn) / 60.0f;
        // 06:00 sunrise (east) ... 12:00 south, 70 deg ... 18:00 sunset (west)
        float t = (h - 6.0f) / 12.0f;
        if (t < 0.0f || t > 1.0f) {                         // night: weak, high "moon"
            float n = (t < 0.0f) ? (h + 6.0f) / 12.0f : (h - 18.0f) / 12.0f;
            az = 90.0f + 180.0f * n;
            el = 38.0f;
            strength = 0.28f;
        } else {
            az = 90.0f + 180.0f * t;
            const float s = static_cast<float>(sin(3.14159265 * t));
            el = 12.0f + 58.0f * s;
            strength = 0.35f + 0.65f * (s < 0.35f ? s / 0.35f : 1.0f);
        }
    }
    az += g_set.sunYaw;
    const double d2r = 3.14159265358979 / 180.0;
    const float ce = static_cast<float>(cos(el * d2r));
    return V3{ce * static_cast<float>(cos(az * d2r)), ce * static_cast<float>(sin(az * d2r)), static_cast<float>(sin(el * d2r))};
}

// ============================================================================
// Pipeline
// ============================================================================
static bool     g_ranThisFrame = false;
static uint32_t g_sceneDraws   = 0;
static constexpr uint32_t kMinSceneDraws = 60;
static bool     g_inFx         = false;

struct QuadVert { float x, y, z, w, u, v; };

using Fn_SetRT      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, void*);
using Fn_SetDS      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, void*);
using Fn_SetRS      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, DWORD);
using Fn_SetTex     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, void*);
using Fn_SetSamp    = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, DWORD, DWORD);
using Fn_SetVS      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, void*);
using Fn_SetPS      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, void*);
using Fn_SetConst   = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
using Fn_SetVp      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const d3d9::D3DVIEWPORT9*);
using Fn_GetVp      = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, d3d9::D3DVIEWPORT9*);
using Fn_SetDecl    = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, void*);
using Fn_DrawUP     = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, UINT, const void*, UINT);
using Fn_Stretch    = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, void*, const RECT*, void*, const RECT*, DWORD);
using Fn_SB         = HRESULT(STDMETHODCALLTYPE*)(void*);

static void P_SetRT(void* surf, UINT w, UINT h) {
    Orig<Fn_SetRT>(d3d9::slot::SetRenderTarget)(g_dev, 0, surf);
    d3d9::D3DVIEWPORT9 vp = {0, 0, w, h, 0.0f, 1.0f};
    Orig<Fn_SetVp>(d3d9::slot::SetViewport)(g_dev, &vp);
}

static void P_Tex(DWORD stage, void* tex, bool linear) {
    Orig<Fn_SetTex>(d3d9::slot::SetTexture)(g_dev, stage, tex);
    auto ss = Orig<Fn_SetSamp>(d3d9::slot::SetSamplerState);
    ss(g_dev, stage, d3d9::SAMP_ADDRESSU, d3d9::TADDRESS_CLAMP);
    ss(g_dev, stage, d3d9::SAMP_ADDRESSV, d3d9::TADDRESS_CLAMP);
    ss(g_dev, stage, d3d9::SAMP_MINFILTER, linear ? d3d9::TEXF_LINEAR : d3d9::TEXF_POINT);
    ss(g_dev, stage, d3d9::SAMP_MAGFILTER, linear ? d3d9::TEXF_LINEAR : d3d9::TEXF_POINT);
    ss(g_dev, stage, d3d9::SAMP_MIPFILTER, d3d9::TEXF_NONE);
    ss(g_dev, stage, d3d9::SAMP_SRGBTEXTURE, 0);
}

static void P_PS(void* ps, const float* consts, UINT nConsts) {
    Orig<Fn_SetPS>(d3d9::slot::SetPixelShader)(g_dev, ps);
    if (nConsts) Orig<Fn_SetConst>(d3d9::slot::SetPixelShaderConstantF)(g_dev, 0, consts, nConsts);
}

static void P_Quad(UINT targetW, UINT targetH) {
    const float texel[4] = {1.0f / targetW, 1.0f / targetH, 0, 0};
    Orig<Fn_SetConst>(d3d9::slot::SetVertexShaderConstantF)(g_dev, 0, texel, 1);
    const QuadVert q[4] = {
        {-1.0f,  1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
        { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f},
    };
    Orig<Fn_DrawUP>(d3d9::slot::DrawPrimitiveUP)(g_dev, d3d9::D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVert));
}

static void P_States() {
    auto rs = Orig<Fn_SetRS>(d3d9::slot::SetRenderState);
    rs(g_dev, d3d9::D3DRS_ZENABLE, 0);
    rs(g_dev, d3d9::D3DRS_ZWRITEENABLE, 0);
    rs(g_dev, d3d9::D3DRS_ALPHABLENDENABLE, 0);
    rs(g_dev, d3d9::RS_SEPARATEALPHABLEND, 0);
    rs(g_dev, d3d9::D3DRS_ALPHATESTENABLE, 0);
    rs(g_dev, d3d9::D3DRS_CULLMODE, d3d9::D3DCULL_NONE);
    rs(g_dev, d3d9::D3DRS_STENCILENABLE, 0);
    rs(g_dev, d3d9::D3DRS_FOGENABLE, 0);
    rs(g_dev, d3d9::D3DRS_LIGHTING, 0);
    rs(g_dev, d3d9::D3DRS_SCISSORTESTENABLE, 0);
    rs(g_dev, d3d9::D3DRS_CLIPPLANEENABLE, 0);
    rs(g_dev, d3d9::D3DRS_COLORWRITEENABLE, 0xF);
    rs(g_dev, d3d9::RS_SRGBWRITEENABLE, 0);
    rs(g_dev, d3d9::D3DRS_FILLMODE, d3d9::D3DFILL_SOLID);
    Orig<Fn_SetDecl>(d3d9::slot::SetVertexDeclaration)(g_dev, g_r.decl);
    Orig<Fn_SetVS>(d3d9::slot::SetVertexShader)(g_dev, g_r.vs);
    for (DWORD s = 0; s < 4; ++s) P_Tex(s, nullptr, false);
}

static void RunPostFx() {
    if (!EnsureResources()) return;
    const Settings& S = g_set;

    const ProjInfo pj = ExtractProjection(g_ts.proj);
    g_status.sceneDetected = g_ts.haveProj && pj.valid;
    if (!g_status.sceneDetected) return;

    g_inFx = true;
    void* rt0 = g_ts.rt0;
    void* ds  = g_ts.ds;
    d3d9::D3DVIEWPORT9 savedVp = {};
    Orig<Fn_GetVp>(d3d9::slot::GetViewport)(g_dev, &savedVp);
    reinterpret_cast<Fn_SB>(static_cast<d3d9::IUnknownLike*>(g_r.stateBlock)->vtable[4])(g_r.stateBlock);   // Capture()

    const UINT W = g_bbW, H = g_bbH;
    const UINT BW = g_r.bloom[0].w, BH = g_r.bloom[0].h;

    // 0. snapshot of the 3D scene
    Orig<Fn_Stretch>(d3d9::slot::StretchRect)(g_dev, rt0, nullptr, g_r.scene.surf, nullptr, d3d9::TEXF_NONE);

    // Common state; the depth buffer must not be bound while it is sampled
    Orig<Fn_SetDS>(d3d9::slot::SetDepthStencilSurface)(g_dev, nullptr);
    P_States();

    // ---------------------------------------------------------------- shared lighting data
    float strength = 1.0f;
    const V3 sunW = SunWorld(strength);
    V3 sunCam = {0.35f, -0.65f, -0.25f};   // fallback (camera basis unknown): from the upper left, towards the viewer
    const bool sunKnown = WorldDirToView(sunW, sunCam);
    const V3 upView = WorldUpInView();
    const float hour = CurrentHour();
    const Sky sky = TimeOfDaySky(hour);
    const float proj[4] = {pj.m11, pj.m22, pj.m31, -pj.m32};
    const float dp[4]   = {pj.m33, pj.m43, 0, 0};

    // ---------------------------------------------------------------- AO + sun shadow
    const bool doAO = S.ao || S.contactShadows;
    void* aoResult = nullptr;
    if (doAO) {
        const float tex[4]  = {1.0f / W, 1.0f / H, static_cast<float>(W), static_cast<float>(H)};
        const float ao[4]   = {S.aoRadius, S.aoIntensity, S.aoPower, S.ao ? 1.0f : 0.0f};
        const float sun[4]  = {sunCam.x, sunCam.y, sunCam.z, S.shadowStrength * strength};
        const float sh[4]   = {S.shadowLength, S.contactShadows ? 1.0f : 0.0f, static_cast<float>(g_frame & 7), 0.45f};
        float c[24];
        memcpy(c + 0, tex, 16); memcpy(c + 4, proj, 16); memcpy(c + 8, dp, 16);
        memcpy(c + 12, ao, 16); memcpy(c + 16, sun, 16); memcpy(c + 20, sh, 16);

        P_SetRT(g_r.ao[0].surf, W, H);
        P_Tex(0, g_r.intz.tex, false);
        P_PS(g_r.psAO, c, 6);
        P_Quad(W, H);

        void* cur = g_r.ao[0].tex;
        if (S.aoBlur) {
            const float cb0[8] = {tex[0], tex[1], tex[2], tex[3], 1, 0, 0.06f, 0};
            P_SetRT(g_r.ao[1].surf, W, H);
            P_Tex(0, g_r.ao[0].tex, false);
            P_PS(g_r.psBlur, cb0, 2);
            P_Quad(W, H);
            const float cb1[8] = {tex[0], tex[1], tex[2], tex[3], 0, 1, 0.06f, 0};
            P_SetRT(g_r.ao[0].surf, W, H);
            P_Tex(0, g_r.ao[1].tex, false);
            P_PS(g_r.psBlur, cb1, 2);
            P_Quad(W, H);
            cur = g_r.ao[0].tex;
        }
        aoResult = cur;
    }

    // ---------------------------------------------------------------- composite
    {
        const float tex[4]  = {1.0f / W, 1.0f / H, 0, 0};
        const float comp[4] = {doAO ? 1.0f : 0.0f, S.shadowStrength, static_cast<float>(S.debugView), 0};
        const float tint[4] = {0.62f, 0.70f, 0.92f, 0};
        float c[12];
        memcpy(c + 0, tex, 16); memcpy(c + 4, comp, 16); memcpy(c + 8, tint, 16);
        P_SetRT(g_r.tmp[0].surf, W, H);
        P_Tex(0, g_r.scene.tex, false);
        P_Tex(1, aoResult ? aoResult : g_r.ao[0].tex, false);
        P_Tex(2, g_r.intz.tex, false);
        P_PS(g_r.psComposite, c, 3);
        P_Quad(W, H);
    }
    Tex* cur = &g_r.tmp[0];
    auto other = [&]() -> Tex* { return cur == &g_r.tmp[0] ? &g_r.tmp[1] : &g_r.tmp[0]; };

    // ---------------------------------------------------------------- atmosphere: reflections + time-of-day fog
    const bool doSSR = S.ssr && S.ssrStrength > 0.001f;
    const bool doFog = S.fog && S.fogDensity > 0.00001f;
    if ((doSSR || doFog) && S.debugView == 0) {
        float c[40] = {};
        const float tex[4]  = {1.0f / W, 1.0f / H, static_cast<float>(W), static_cast<float>(H)};
        const float fogc[4] = {sky.fog[0], sky.fog[1], sky.fog[2], S.fogDensity};
        const float fog2[4] = {S.fogStart, S.fogMax, S.fogScatter, doFog ? 1.0f : 0.0f};
        const float sunv[4] = {sunCam.x, sunCam.y, sunCam.z, strength};
        const float sunc[4] = {sky.sun[0], sky.sun[1], sky.sun[2], 0};
        const float upv[4]  = {upView.x, upView.y, upView.z, 0};
        const float ssr[4]  = {S.ssrStrength, S.ssrF0, S.ssrDistance, S.ssrThickness};
        const float ssr2[4] = {doSSR ? 1.0f : 0.0f, static_cast<float>(g_frame & 7), 0, 0};
        memcpy(c + 0, tex, 16); memcpy(c + 4, proj, 16); memcpy(c + 8, dp, 16); memcpy(c + 12, fogc, 16);
        memcpy(c + 16, fog2, 16); memcpy(c + 20, sunv, 16); memcpy(c + 24, sunc, 16); memcpy(c + 28, upv, 16);
        memcpy(c + 32, ssr, 16); memcpy(c + 36, ssr2, 16);
        Tex* dst = other();
        P_SetRT(dst->surf, W, H);
        P_Tex(0, cur->tex, false);
        P_Tex(1, g_r.intz.tex, false);
        P_PS(g_r.psAtmos, c, 10);
        P_Quad(W, H);
        cur = dst;
    }

    // ---------------------------------------------------------------- FXAA
    if (S.fxaa && S.debugView == 0) {
        const float tex[4] = {1.0f / W, 1.0f / H, static_cast<float>(W), static_cast<float>(H)};
        Tex* dst = other();
        P_SetRT(dst->surf, W, H);
        P_Tex(0, cur->tex, true);
        P_PS(g_r.psFxaa, tex, 1);
        P_Quad(W, H);
        cur = dst;
    }

    // ---------------------------------------------------------------- light shafts (1/4 res)
    const bool doShafts = S.shafts && S.shaftStrength > 0.001f && S.debugView == 0 && sunKnown && strength > 0.05f;
    if (doShafts) {
        // Sun position on screen. A sun behind the camera uses the anti-solar point, where the shafts converge.
        V3 s = sunCam;
        if (s.z < 0.0f) s = V3{-s.x, -s.y, -s.z};
        float su = 0.5f, sv = 0.5f;
        if (s.z > 0.02f) {
            su = (s.x * pj.m11 / s.z + pj.m31) * 0.5f + 0.5f;
            sv = (s.y * pj.m22 / s.z - pj.m32) * 0.5f + 0.5f;
        } else {
            su = 0.5f + s.x * 50.0f; sv = 0.5f + s.y * 50.0f;
        }
        su = su < -2.0f ? -2.0f : (su > 3.0f ? 3.0f : su);
        sv = sv < -2.0f ? -2.0f : (sv > 3.0f ? 3.0f : sv);
        const float facing = Abs(sunCam.z);                         // rays fade when the sun is edge-on
        const float amount = S.shaftStrength * strength * (0.35f + 0.65f * facing);
        const float cs[12] = {1.0f / BW, 1.0f / BH, static_cast<float>(BW), static_cast<float>(BH),
                              su, sv, amount, S.shaftThreshold,
                              sky.sun[0], sky.sun[1], sky.sun[2], 0.955f};
        P_SetRT(g_r.shaft[0].surf, BW, BH);
        P_Tex(0, cur->tex, true);
        P_Tex(1, g_r.intz.tex, false);
        P_PS(g_r.psShafts, cs, 3);
        P_Quad(BW, BH);
        const float bh[12] = {1.0f / BW, 1.0f / BH, 0, 0,  0, 0, 0, 0,  1, 0, 0, 0};
        P_SetRT(g_r.shaft[1].surf, BW, BH);
        P_Tex(0, g_r.shaft[0].tex, true);
        P_PS(g_r.psBloomBlur, bh, 3);
        P_Quad(BW, BH);
        const float bv[12] = {1.0f / BW, 1.0f / BH, 0, 0,  0, 0, 0, 0,  0, 1, 0, 0};
        P_SetRT(g_r.shaft[0].surf, BW, BH);
        P_Tex(0, g_r.shaft[1].tex, true);
        P_PS(g_r.psBloomBlur, bv, 3);
        P_Quad(BW, BH);
    }

    // ---------------------------------------------------------------- bloom
    const bool doBloom = S.bloom && S.bloomStrength > 0.001f && S.debugView == 0;
    if (doBloom) {
        const float cb[12] = {1.0f / W, 1.0f / H, 0, 0,  S.bloomThreshold, 0.25f, 0, 0,  0, 0, 0, 0};
        P_SetRT(g_r.bloom[0].surf, BW, BH);
        P_Tex(0, cur->tex, true);
        P_PS(g_r.psBright, cb, 2);
        P_Quad(BW, BH);
        const float bh[12] = {1.0f / BW, 1.0f / BH, 0, 0,  0, 0, 0, 0,  1, 0, 0, 0};
        P_SetRT(g_r.bloom[1].surf, BW, BH);
        P_Tex(0, g_r.bloom[0].tex, true);
        P_PS(g_r.psBloomBlur, bh, 3);
        P_Quad(BW, BH);
        const float bv[12] = {1.0f / BW, 1.0f / BH, 0, 0,  0, 0, 0, 0,  0, 1, 0, 0};
        P_SetRT(g_r.bloom[0].surf, BW, BH);
        P_Tex(0, g_r.bloom[1].tex, true);
        P_PS(g_r.psBloomBlur, bv, 3);
        P_Quad(BW, BH);
    }

    // ---------------------------------------------------------------- final grade -> back buffer
    {
        const bool grade = (S.debugView == 0);
        const bool doTm = grade && S.tonemap && S.tonemapStrength > 0.001f;
        float tint[3] = {1.0f, 1.0f, 1.0f};
        if (grade && S.timeGrade)
            for (int k = 0; k < 3; ++k) tint[k] = 1.0f + (sky.tint[k] - 1.0f) * S.gradeStrength;
        const float cb[16] = {1.0f / W, 1.0f / H, 0, 0,
                              doBloom ? S.bloomStrength : 0.0f, grade ? S.vignette : 0.0f, grade ? S.saturation : 1.0f, grade ? S.contrast : 1.0f,
                              grade ? S.exposure : 1.0f, doTm ? S.tonemapStrength : 0.0f, S.tonemapExposure, doShafts ? 1.0f : 0.0f,
                              tint[0], tint[1], tint[2], 0};
        P_SetRT(rt0, W, H);
        P_Tex(0, cur->tex, false);
        P_Tex(1, doBloom ? g_r.bloom[0].tex : g_r.scene.tex, true);
        P_Tex(2, doShafts ? g_r.shaft[0].tex : g_r.scene.tex, true);
        P_PS(g_r.psFinal, cb, 4);
        P_Quad(W, H);
    }

    // ---------------------------------------------------------------- restore
    Orig<Fn_SetRT>(d3d9::slot::SetRenderTarget)(g_dev, 0, rt0);
    Orig<Fn_SetDS>(d3d9::slot::SetDepthStencilSurface)(g_dev, ds);
    Orig<Fn_SetVp>(d3d9::slot::SetViewport)(g_dev, &savedVp);
    reinterpret_cast<Fn_SB>(static_cast<d3d9::IUnknownLike*>(g_r.stateBlock)->vtable[5])(g_r.stateBlock);     // Apply()
    g_inFx = false;
    ++g_status.framesProcessed;
}

// ============================================================================
// Hooks
// ============================================================================
#define HK(ret, name, ...) static ret STDMETHODCALLTYPE Hk_##name(IDirect3DDevice9* d, ##__VA_ARGS__)

static inline bool IsMainDepth(void* surf) {
    if (!surf || !g_bbW) return false;
    if (surf == g_gameDS) return true;
    d3d9::D3DSURFACE_DESC dd = {};
    if (d3d9::SurfaceGetDesc(surf, &dd) < 0) return false;
    return dd.Width == g_bbW && dd.Height == g_bbH && dd.Format == d3d9::FMT_D24S8 && dd.MultiSampleType == 0;
}

static inline void PreDraw() {
    if (g_inFx || !g_set.enabled || !g_mainRT || g_ts.rt0 != g_mainRT) return;
    if (g_ts.rs[d3d9::D3DRS_ZENABLE]) {
        ++g_sceneDraws;
    } else if (!g_ranThisFrame && g_sceneDraws >= kMinSceneDraws) {
        g_ranThisFrame = true;                  // exactly once per frame, at the 3D -> HUD transition
        RunPostFx();
    }
}

HK(HRESULT, SetRenderState, DWORD st, DWORD v) {
    if (st < 256) g_ts.rs[st] = v;
    if (g_probeOn && (st == 7 || st == 14 || st == 15 || st == 27 || st == 28 || st == 52 || st == 137 || st == 22 || st == 168 || st == 23 || st == 19 || st == 20))
        Pl("  RS %u = 0x%X", st, v);
    return Orig<Fn_SetRS>(d3d9::slot::SetRenderState)(d, st, v);
}

HK(HRESULT, SetTransform, DWORD st, const D3DMATRIX* m) {
    if (m) {
        if (st == d3d9::TS_PROJECTION) { g_ts.proj = *m; g_ts.haveProj = true; }
        else if (st == d3d9::TS_VIEW)  { g_ts.view = *m; }
        else if (st == d3d9::TS_WORLD) { g_ts.world = *m; CaptureUpFromWorldMatrix(*m); }
        if (g_probeOn)
            Pl("  SetTransform %u [%.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f]", st,
               m->m[0][0], m->m[0][1], m->m[0][2], m->m[0][3], m->m[1][0], m->m[1][1], m->m[1][2], m->m[1][3],
               m->m[2][0], m->m[2][1], m->m[2][2], m->m[2][3], m->m[3][0], m->m[3][1], m->m[3][2], m->m[3][3]);
    }
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DMATRIX*)>(d3d9::slot::SetTransform)(d, st, m);
}

HK(HRESULT, SetRenderTarget, DWORD idx, void* surf) {
    if (idx == 0) g_ts.rt0 = surf;
    if (g_probeOn) { char b[160]; SurfStr(surf, b, sizeof(b)); Pl("SetRenderTarget %u %s", idx, b); }
    return Orig<Fn_SetRT>(d3d9::slot::SetRenderTarget)(d, idx, surf);
}

HK(HRESULT, SetDepthStencilSurface, void* surf) {
    void* use = surf;
    if (g_set.enabled && surf && surf != g_r.intz.surf && IsMainDepth(surf)) {
        g_gameDS = surf;
        if (EnsureResources() && g_r.intz.surf) use = g_r.intz.surf;
    }
    g_ts.ds = use;
    if (g_probeOn) { char b[160]; SurfStr(surf, b, sizeof(b)); Pl("SetDepthStencilSurface %s%s", b, use != surf ? "  -> substituido por INTZ" : ""); }
    return Orig<Fn_SetDS>(d3d9::slot::SetDepthStencilSurface)(d, use);
}

HK(HRESULT, Clear, DWORD count, const void* rects, DWORD flags, DWORD color, float z, DWORD stencil) {
    if (g_probeOn) Pl("Clear count=%u flags=0x%X color=0x%08X z=%.3f stencil=%u", count, flags, color, z, stencil);
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const void*, DWORD, DWORD, float, DWORD)>(d3d9::slot::Clear)(d, count, rects, flags, color, z, stencil);
}

HK(HRESULT, BeginScene) {
    Pl("BeginScene");
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*)>(d3d9::slot::BeginScene)(d);
}

HK(HRESULT, SetViewport, const d3d9::D3DVIEWPORT9* vp) {
    if (g_probeOn && vp) Pl("SetViewport %u,%u %ux%u z=%.2f..%.2f", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
    return Orig<Fn_SetVp>(d3d9::slot::SetViewport)(d, vp);
}

HK(HRESULT, SetVertexShader, void* s) {
    g_ts.vs = s;
    if (g_probeOn) Pl("  SetVertexShader %p", s);
    return Orig<Fn_SetVS>(d3d9::slot::SetVertexShader)(d, s);
}
HK(HRESULT, SetPixelShader, void* s) {
    g_ts.ps = s;
    if (g_probeOn) Pl("  SetPixelShader %p", s);
    return Orig<Fn_SetPS>(d3d9::slot::SetPixelShader)(d, s);
}
HK(HRESULT, SetFVF, DWORD fvf) {
    g_ts.fvf = fvf;
    if (g_probeOn) Pl("  SetFVF 0x%X", fvf);
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD)>(d3d9::slot::SetFVF)(d, fvf);
}
HK(HRESULT, SetVertexDeclaration, void* decl) {
    g_ts.decl = decl;
    if (g_probeOn) Pl("  SetVertexDeclaration %p", decl);
    return Orig<Fn_SetDecl>(d3d9::slot::SetVertexDeclaration)(d, decl);
}
HK(HRESULT, SetTexture, DWORD stage, void* tex) {
    if (g_probeOn) Pl("  SetTexture %u %p", stage, tex);
    return Orig<Fn_SetTex>(d3d9::slot::SetTexture)(d, stage, tex);
}
HK(HRESULT, SetVertexShaderConstantF, UINT start, const float* data, UINT count) {
    if (data && !g_inFx) {
        for (UINT i = 0; i < count && start + i < 256; ++i) {
            memcpy(g_ts.vsc[start + i], data + i * 4, 16);
            g_ts.vscSeen[start + i] = true;
        }
        if (start == 0 && count >= 4) TryCaptureCamera();
    }
    if (g_probeOn && data && count) {
        Pl("  VSConst c%u x%u [%.3f %.3f %.3f %.3f]", start, count, data[0], data[1], data[2], data[3]);
        const bool big = count >= 20;
        if ((big && g_probeVscDumpsBig < 6) || (!big && count >= 4 && g_probeVscDumps < 14)) {
            (big ? g_probeVscDumpsBig : g_probeVscDumps)++;
            const UINT n = count > 64 ? 64 : count;
            for (UINT i = 0; i < n; ++i)
                Pl("      c%u: %9.4f %9.4f %9.4f %9.4f", start + i, data[i * 4], data[i * 4 + 1], data[i * 4 + 2], data[i * 4 + 3]);
        }
    }
    return Orig<Fn_SetConst>(d3d9::slot::SetVertexShaderConstantF)(d, start, data, count);
}
HK(HRESULT, SetPixelShaderConstantF, UINT start, const float* data, UINT count) {
    if (g_probeOn && data && count) Pl("  PSConst c%u x%u [%.3f %.3f %.3f %.3f]", start, count, data[0], data[1], data[2], data[3]);
    return Orig<Fn_SetConst>(d3d9::slot::SetPixelShaderConstantF)(d, start, data, count);
}
HK(HRESULT, SetStreamSource, UINT stream, void* vb, UINT off, UINT stride) {
    if (g_probeOn) Pl("  SetStreamSource %u vb=%p off=%u stride=%u", stream, vb, off, stride);
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, void*, UINT, UINT)>(d3d9::slot::SetStreamSource)(d, stream, vb, off, stride);
}
HK(HRESULT, SetIndices, void* ib) {
    if (g_probeOn) Pl("  SetIndices %p", ib);
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, void*)>(d3d9::slot::SetIndices)(d, ib);
}
HK(HRESULT, SetLight, DWORD idx, const d3d9::D3DLIGHT9* l) {
    if (g_probeOn && l)
        Pl("  SetLight %u type=%u diff=(%.2f %.2f %.2f) amb=(%.2f %.2f %.2f) dir=(%.3f %.3f %.3f) pos=(%.1f %.1f %.1f)", idx, l->Type,
           l->Diffuse.r, l->Diffuse.g, l->Diffuse.b, l->Ambient.r, l->Ambient.g, l->Ambient.b,
           l->Direction.x, l->Direction.y, l->Direction.z, l->Position.x, l->Position.y, l->Position.z);
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const d3d9::D3DLIGHT9*)>(d3d9::slot::SetLight)(d, idx, l);
}
HK(HRESULT, StretchRect, void* src, const RECT* sr, void* dst, const RECT* dr, DWORD filter) {
    if (g_probeOn) { char a[140], b[140]; SurfStr(src, a, sizeof(a)); SurfStr(dst, b, sizeof(b)); Pl("StretchRect %s -> %s", a, b); }
    return Orig<Fn_Stretch>(d3d9::slot::StretchRect)(d, src, sr, dst, dr, filter);
}

static void LogDrawState(const char* what) {
    ++g_probeDraws;
    Pl("%s z=%u zw=%u zf=%u ab=%u at=%u lit=%u st=%u cull=%u cw=0x%X vs=%p ps=%p fvf=0x%X", what, g_ts.rs[7], g_ts.rs[14], g_ts.rs[23],
       g_ts.rs[27], g_ts.rs[15], g_ts.rs[137], g_ts.rs[52], g_ts.rs[22], g_ts.rs[168], g_ts.vs, g_ts.ps, g_ts.fvf);
}

HK(HRESULT, DrawPrimitive, DWORD type, UINT start, UINT count) {
    PreDraw();
    if (g_probeOn) { char b[96]; _snprintf(b, sizeof(b), "DrawPrimitive t=%u start=%u n=%u", type, start, count); LogDrawState(b); }
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, UINT, UINT)>(d3d9::slot::DrawPrimitive)(d, type, start, count);
}
HK(HRESULT, DrawIndexedPrimitive, DWORD type, INT baseVertex, UINT minIdx, UINT numVerts, UINT startIdx, UINT count) {
    PreDraw();
    if (g_probeOn) { char b[128]; _snprintf(b, sizeof(b), "DrawIndexed t=%u base=%d minIdx=%u nv=%u start=%u n=%u", type, baseVertex, minIdx, numVerts, startIdx, count); LogDrawState(b); }
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, INT, UINT, UINT, UINT, UINT)>(d3d9::slot::DrawIndexedPrimitive)(d, type, baseVertex, minIdx, numVerts, startIdx, count);
}
HK(HRESULT, DrawPrimitiveUP, DWORD type, UINT count, const void* data, UINT stride) {
    PreDraw();
    if (g_probeOn) { char b[96]; _snprintf(b, sizeof(b), "DrawPrimitiveUP t=%u n=%u stride=%u", type, count, stride); LogDrawState(b); }
    return Orig<Fn_DrawUP>(d3d9::slot::DrawPrimitiveUP)(d, type, count, data, stride);
}
HK(HRESULT, DrawIndexedPrimitiveUP, DWORD type, UINT minIdx, UINT numVerts, UINT count, const void* idx, DWORD idxFmt, const void* vdata, UINT stride) {
    PreDraw();
    if (g_probeOn) { char b[96]; _snprintf(b, sizeof(b), "DrawIndexedUP t=%u nv=%u n=%u stride=%u", type, numVerts, count, stride); LogDrawState(b); }
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, UINT, UINT, UINT, const void*, DWORD, const void*, UINT)>(d3d9::slot::DrawIndexedPrimitiveUP)(d, type, minIdx, numVerts, count, idx, idxFmt, vdata, stride);
}

HK(HRESULT, Reset, d3d9::D3DPRESENT_PARAMETERS* pp) {
    ts2::hook::Log("[FX] Reset do device: liberando recursos do post-fx");
    ReleaseResources();
    g_gameDS = nullptr; g_ts.ds = nullptr; g_ts.rt0 = nullptr; g_mainRT = nullptr;
    HRESULT hr = Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, d3d9::D3DPRESENT_PARAMETERS*)>(d3d9::slot::Reset)(d, pp);
    g_r.failed = false;
    return hr;
}

HK(HRESULT, Present, const RECT* a, const RECT* b, HWND w, const void* r) {
    if (g_probeOn) {
        Pl("Present (frame %u) fx: ran=%d sceneDraws=%u", g_frame, g_ranThisFrame, g_sceneDraws);
        g_probeOn = false;
        ProbeFlush();
        g_set.probe = false;
    }
    ++g_frame;
    g_ranThisFrame = false;
    g_sceneDraws = 0;
    if (g_set.probe && !g_probeOn) {
        if (!g_probeBuf) g_probeBuf = static_cast<char*>(VirtualAlloc(nullptr, kProbeCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        g_probeLen = 0; g_probeDraws = 0; g_probeVscDumps = 0; g_probeVscDumpsBig = 0;
        g_probeOn = g_probeBuf != nullptr;
        if (g_probeOn) Pl("==== frame %u ====", g_frame);
    }
    if (g_dirty && g_frame - g_dirtyFrame > 90) { g_dirty = false; SaveConfigFile(); }   // ~1.5 s after the last UI change
    else if ((g_frame % 120) == 0) LoadConfigFile();
    return Orig<HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const void*)>(d3d9::slot::Present)(d, a, b, w, r);
}

struct HookDef { int slot; void* fn; };

static void InstallHooks(IDirect3DDevice9* dev) {
    if (g_hooked) return;
    uintptr_t vt = reinterpret_cast<uintptr_t>(dev->vtable);
    for (int i = 0; i < 128; ++i) g_vt[i] = dev->vtable[i];
    const HookDef defs[] = {
        {d3d9::slot::SetRenderState, (void*)&Hk_SetRenderState},
        {d3d9::slot::SetTransform, (void*)&Hk_SetTransform},
        {d3d9::slot::SetRenderTarget, (void*)&Hk_SetRenderTarget},
        {d3d9::slot::SetDepthStencilSurface, (void*)&Hk_SetDepthStencilSurface},
        {d3d9::slot::Clear, (void*)&Hk_Clear},
        {d3d9::slot::BeginScene, (void*)&Hk_BeginScene},
        {d3d9::slot::SetViewport, (void*)&Hk_SetViewport},
        {d3d9::slot::SetVertexShader, (void*)&Hk_SetVertexShader},
        {d3d9::slot::SetPixelShader, (void*)&Hk_SetPixelShader},
        {d3d9::slot::SetFVF, (void*)&Hk_SetFVF},
        {d3d9::slot::SetVertexDeclaration, (void*)&Hk_SetVertexDeclaration},
        {d3d9::slot::SetTexture, (void*)&Hk_SetTexture},
        {d3d9::slot::SetVertexShaderConstantF, (void*)&Hk_SetVertexShaderConstantF},
        {d3d9::slot::SetPixelShaderConstantF, (void*)&Hk_SetPixelShaderConstantF},
        {d3d9::slot::SetStreamSource, (void*)&Hk_SetStreamSource},
        {d3d9::slot::SetIndices, (void*)&Hk_SetIndices},
        {d3d9::slot::SetLight, (void*)&Hk_SetLight},
        {d3d9::slot::StretchRect, (void*)&Hk_StretchRect},
        {d3d9::slot::DrawPrimitive, (void*)&Hk_DrawPrimitive},
        {d3d9::slot::DrawIndexedPrimitive, (void*)&Hk_DrawIndexedPrimitive},
        {d3d9::slot::DrawPrimitiveUP, (void*)&Hk_DrawPrimitiveUP},
        {d3d9::slot::DrawIndexedPrimitiveUP, (void*)&Hk_DrawIndexedPrimitiveUP},
        {d3d9::slot::Reset, (void*)&Hk_Reset},
        {d3d9::slot::Present, (void*)&Hk_Present},
    };
    int ok = 0;
    for (const auto& h : defs) {
        void* dummy = nullptr;
        if (ts2::hook::HookPointerSlot(vt + h.slot * sizeof(void*), h.fn, &dummy)) ++ok;
    }
    g_hooked = true;
    g_status.hooked = true;
    ts2::hook::Log("[FX] %d/%d hooks instalados no IDirect3DDevice9 (vtable=0x%08X)", ok, static_cast<int>(sizeof(defs) / sizeof(defs[0])), static_cast<unsigned>(vt));
}

} // namespace

Settings& GetSettings() { return g_set; }
const Status& GetStatus() { return g_status; }

void LoadConfigFile() {
    static char text[8192];
    DWORD len = 0;
    if (!ReadWholeFile(kConfigName, text, sizeof(text), &len)) return;
    DWORD stamp = len;
    for (DWORD i = 0; i < len; ++i) stamp = stamp * 131u + static_cast<uint8_t>(text[i]);
    if (stamp == g_cfgStamp) return;
    g_cfgStamp = stamp;
    ParseConfigText(text);
    ts2::hook::Log("[FX] ts2fx.cfg carregado");
}

void SaveConfigFile() {
    static char buf[3072];
    int n = 0;
    const size_t cap = sizeof(buf);
    const Settings& s = g_set;
#define WB(k, v) n += _snprintf(buf + n, cap - n, "%s=%d\n", k, (v) ? 1 : 0)
#define WF(k, v) n += _snprintf(buf + n, cap - n, "%s=%.4f\n", k, static_cast<double>(v))
    n += _snprintf(buf + n, cap - n, "# TS2 post-fx - gravado pelo painel Graficos (F10). Pode editar a mao: recarrega sozinho.\n");
    WB("enabled", s.enabled);
    WB("ao", s.ao); WF("aoradius", s.aoRadius); WF("aointensity", s.aoIntensity); WF("aopower", s.aoPower); WB("aoblur", s.aoBlur);
    WB("contactshadows", s.contactShadows); WF("shadowstrength", s.shadowStrength); WF("shadowlength", s.shadowLength);
    WB("sunauto", s.sunAuto); WF("sunazimuth", s.sunAzimuth); WF("sunelevation", s.sunElevation); WF("sunyaw", s.sunYaw);
    WB("fog", s.fog); WF("fogdensity", s.fogDensity); WF("fogstart", s.fogStart); WF("fogmax", s.fogMax); WF("fogscatter", s.fogScatter);
    WB("shafts", s.shafts); WF("shaftstrength", s.shaftStrength); WF("shaftthreshold", s.shaftThreshold);
    WB("ssr", s.ssr); WF("ssrstrength", s.ssrStrength); WF("ssrf0", s.ssrF0); WF("ssrdistance", s.ssrDistance); WF("ssrthickness", s.ssrThickness);
    WB("tonemap", s.tonemap); WF("tonemapstrength", s.tonemapStrength); WF("tonemapexposure", s.tonemapExposure);
    WB("timegrade", s.timeGrade); WF("gradestrength", s.gradeStrength);
    WB("fxaa", s.fxaa); WB("bloom", s.bloom); WF("bloomstrength", s.bloomStrength); WF("bloomthreshold", s.bloomThreshold);
    WF("vignette", s.vignette); WF("saturation", s.saturation); WF("contrast", s.contrast); WF("exposure", s.exposure);
#undef WB
#undef WF
    if (n <= 0 || static_cast<size_t>(n) >= cap) return;
    HANDLE h = CreateFileA(kConfigName, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { ts2::hook::Log("[FX] nao consegui gravar %s", kConfigName); return; }
    DWORD w = 0;
    WriteFile(h, buf, static_cast<DWORD>(n), &w, nullptr);
    CloseHandle(h);
    // the hot reload must not re-apply what we just wrote
    DWORD stamp = static_cast<DWORD>(n);
    for (int i = 0; i < n; ++i) stamp = stamp * 131u + static_cast<uint8_t>(buf[i]);
    g_cfgStamp = stamp;
    ts2::hook::Log("[FX] configuracao gravada em %s", kConfigName);
}

void NotifyChanged() {
    g_dirty = true;
    g_dirtyFrame = g_frame;
}

void ApplyPreset(int id) {
    // keep the "meta" settings (sun calibration, diagnostics) across presets
    const Settings keep = g_set;
    g_set = Settings();
    g_set.sunAuto = keep.sunAuto; g_set.sunAzimuth = keep.sunAzimuth; g_set.sunElevation = keep.sunElevation;
    g_set.sunYaw = keep.sunYaw; g_set.debugView = keep.debugView; g_set.probe = keep.probe;
    Settings& s = g_set;
    switch (id) {
        case 0:   // original look
            s.enabled = false;
            break;
        case 1:   // soft
            s.aoIntensity = 0.50f; s.aoRadius = 0.90f; s.shadowStrength = 0.25f;
            s.fogDensity = 0.0030f; s.fogMax = 0.35f; s.shaftStrength = 0.25f; s.ssrStrength = 0.35f;
            s.tonemapStrength = 0.35f; s.gradeStrength = 0.40f; s.bloomStrength = 0.15f; s.vignette = 0.10f;
            break;
        case 3:   // cinematic
            s.aoIntensity = 1.15f; s.aoPower = 1.2f; s.shadowStrength = 0.55f;
            s.fogDensity = 0.0060f; s.fogMax = 0.60f; s.fogScatter = 0.70f; s.shaftStrength = 0.75f; s.ssrStrength = 0.95f;
            s.tonemapStrength = 0.85f; s.gradeStrength = 0.90f; s.bloomStrength = 0.35f; s.vignette = 0.26f;
            s.saturation = 1.12f; s.contrast = 1.10f;
            break;
        default:  // balanced = defaults
            break;
    }
    NotifyChanged();
}

void AttachDevice(d3d9::IDirect3DDevice9* dev) {
    if (!dev || g_dev == dev) return;
    g_dev = dev;
    LoadConfigFile();
    InstallHooks(dev);
    QueryBackbuffer();

    // Learn the game's current render target / depth buffer and swap in the INTZ one right away
    void* rt = nullptr;
    if (Orig<HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, void**)>(d3d9::slot::GetRenderTarget)(dev, 0, &rt) >= 0 && rt) {
        g_ts.rt0 = rt;
        d3d9::ComRelease(rt);
    }
    void* ds = nullptr;
    if (Orig<HRESULT(STDMETHODCALLTYPE*)(void*, void**)>(d3d9::slot::GetDepthStencilSurface)(dev, &ds) >= 0 && ds) {
        char s[160];
        SurfStr(ds, s, sizeof(s));
        ts2::hook::Log("[FX] depth buffer do jogo: %s", s);
        g_ts.ds = ds;
        if (g_set.enabled && IsMainDepth(ds)) {
            g_gameDS = ds;
            if (EnsureResources() && g_r.intz.surf) {
                Orig<Fn_SetDS>(d3d9::slot::SetDepthStencilSurface)(dev, g_r.intz.surf);
                g_ts.ds = g_r.intz.surf;
                ts2::hook::Log("[FX] depth buffer substituido por textura INTZ %p", g_r.intz.surf);
            }
        }
        d3d9::ComRelease(ds);
    }
    ts2::hook::Log("[FX] backbuffer %ux%u fmt=%u, rt0=%p mainRT=%p", g_bbW, g_bbH, g_bbFmt, g_ts.rt0, g_mainRT);
}

void OnRenderTickBegin(d3d9::IDirect3DDevice9*) {}
void OnRenderTickEnd(d3d9::IDirect3DDevice9*) {}

bool ExecuteCommand(const char* cmd, char* out, size_t outLen) {
    if (strncmp(cmd, "fx.", 3) != 0) return false;
    cmd += 3;
    if (strncmp(cmd, "set ", 4) == 0) {
        char key[48] = {};
        const char* p = cmd + 4;
        size_t n = 0;
        while (*p && *p != ' ' && n < sizeof(key) - 1) key[n++] = *p++;
        while (*p == ' ') ++p;
        if (ApplyKV(key, p)) _snprintf(out, outLen, "fx: %s = %s", key, p);
        else _snprintf(out, outLen, "fx: chave desconhecida '%s'", key);
        return true;
    }
    if (strncmp(cmd, "preset ", 7) == 0) {
        ApplyPreset(static_cast<int>(strtol(cmd + 7, nullptr, 10)));
        _snprintf(out, outLen, "fx: preset %s aplicado", cmd + 7);
        return true;
    }
    if (_stricmp(cmd, "save") == 0) {
        SaveConfigFile();
        _snprintf(out, outLen, "fx: gravado em ts2fx.cfg");
        return true;
    }
    if (_stricmp(cmd, "probe") == 0) {
        g_set.probe = true;
        _snprintf(out, outLen, "fx: probe armada (proximo frame)");
        return true;
    }
    if (_stricmp(cmd, "status") == 0) {
        _snprintf(out, outLen, "fx: hooked=%d depth=%d pipeline=%d cena=%d frames=%u (%s)", g_status.hooked, g_status.depthOk,
                  g_status.pipelineOk, g_status.sceneDetected, g_status.framesProcessed, g_status.message);
        return true;
    }
    _snprintf(out, outLen, "fx.set <chave> <valor> | fx.probe | fx.status");
    return true;
}

} // namespace ts2::fx
