#include "ts2/catalog.hpp"
#include "ts2/d3d9_ext.hpp"
#include "ts2/i18n.hpp"

namespace ts2::catalog {

namespace {

constexpr const char* kCatalogFile = "ts2catalog.bin";
constexpr const char* kThumbsFile  = "ts2thumbs.bin";
constexpr uint32_t kThumbBytes = 64 * 64 * 2;
constexpr uint32_t kThumbHeader = 16;

#pragma pack(push, 1)
struct FileHeader {
    char     magic[4];
    uint32_t version, count, catCount, recSize, offCats, offRecs, offNames, namesLen;
};
struct FileRec {
    uint32_t guid, group;
    uint16_t price;
    uint8_t  cat, flags;
    uint32_t nameEn, namePt, thumb, src;
};
#pragma pack(pop)
static_assert(sizeof(FileHeader) == 36, "catalog header");
static_assert(sizeof(FileRec) == 28, "catalog record");

struct State {
    bool      tried = false;
    bool      ok = false;
    char      err[96] = "";
    uint8_t*  blob = nullptr;
    Item*     items = nullptr;
    char**    folded = nullptr;      // lower-case, accent-free search text per item
    uint32_t  count = 0, mods = 0;
    int       catCount = 0;
    const char* catEn[24] = {};
    const char* catPt[24] = {};
    HANDLE    thumbs = INVALID_HANDLE_VALUE;
    uint32_t  thumbCount = 0;
};
State g;

void Fail(const char* msg) {
    g.ok = false;
    size_t n = strlen(msg);
    if (n >= sizeof(g.err)) n = sizeof(g.err) - 1;
    memcpy(g.err, msg, n);
    g.err[n] = '\0';
}

bool ReadAll(const char* path, uint8_t** outBuf, uint32_t* outSize) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(h, nullptr);
    if (size == 0xFFFFFFFFu || size < sizeof(FileHeader) || size > (64u << 20)) { CloseHandle(h); return false; }
    uint8_t* buf = static_cast<uint8_t*>(malloc(size + 1));
    if (!buf) { CloseHandle(h); return false; }
    DWORD got = 0;
    const BOOL okRead = ReadFile(h, buf, size, &got, nullptr);
    CloseHandle(h);
    if (!okRead || got != size) { free(buf); return false; }
    buf[size] = 0;
    *outBuf = buf;
    *outSize = size;
    return true;
}

// Lower-case + strip the accents of Latin-1 letters (UTF-8 "C3 xx"); everything else is copied as is.
size_t Fold(const char* s, char* out, size_t cap) {
    static const char kMap[] =   // for C3 80..BF (À..ÿ): the plain ASCII letter, or 0
        "aaaaaaaceeeeiiii" "dnooooo\0ouuuuyts" "aaaaaaaceeeeiiii" "dnooooo\0ouuuuyty";
    size_t n = 0;
    for (; *s && n + 1 < cap; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == 0xC3 && s[1]) {
            const unsigned char d = static_cast<unsigned char>(s[1]);
            if (d >= 0x80 && d <= 0xBF && kMap[d - 0x80]) { out[n++] = kMap[d - 0x80]; ++s; continue; }
        }
        out[n++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : static_cast<char>(c);
    }
    out[n] = '\0';
    return n;
}

void Cleanup() {
    if (g.folded) { for (uint32_t i = 0; i < g.count; ++i) free(g.folded[i]); free(g.folded); g.folded = nullptr; }
    free(g.items); g.items = nullptr;
    free(g.blob);  g.blob = nullptr;
    g.count = 0;
}

// ---- thumbnails ---------------------------------------------------------------------------------------------
constexpr int kCacheN = 224;
struct CacheSlot { uint32_t idx; void* tex; uint32_t used; };
CacheSlot g_cache[kCacheN];
d3d9::IDirect3DDevice9* g_cacheDev = nullptr;
uint32_t g_useClock = 0, g_frameBudget = 0, g_budgetTick = 0;

void* CreateThumbTexture(d3d9::IDirect3DDevice9* dev, uint32_t idx) {
    if (g.thumbs == INVALID_HANDLE_VALUE || idx >= g.thumbCount) return nullptr;
    static uint16_t px[64 * 64];
    const uint32_t off = kThumbHeader + idx * kThumbBytes;
    if (SetFilePointer(g.thumbs, static_cast<LONG>(off), nullptr, 0) == 0xFFFFFFFFu) return nullptr;
    DWORD got = 0;
    if (!ReadFile(g.thumbs, px, kThumbBytes, &got, nullptr) || got != kThumbBytes) return nullptr;

    using FnCreateTex = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, void*);
    void* tex = nullptr;
    if (reinterpret_cast<FnCreateTex>(dev->vtable[d3d9::slot::CreateTexture])(dev, 64, 64, 1, 0, 21 /*A8R8G8B8*/, 1 /*MANAGED*/, &tex, nullptr) < 0 || !tex)
        return nullptr;
    struct LockedRect { INT pitch; void* bits; } lr = {};
    auto** vt = static_cast<d3d9::IUnknownLike*>(tex)->vtable;
    using FnLock = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, LockedRect*, const RECT*, DWORD);
    using FnUnlock = HRESULT(STDMETHODCALLTYPE*)(void*, UINT);
    if (reinterpret_cast<FnLock>(vt[19])(tex, 0, &lr, nullptr, 0) < 0 || !lr.bits) { d3d9::ComRelease(tex); return nullptr; }
    for (int y = 0; y < 64; ++y) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.bits) + static_cast<size_t>(y) * lr.pitch);
        for (int x = 0; x < 64; ++x) {
            const uint32_t v = px[y * 64 + x];
            const uint32_t r = (v >> 11) & 31, gg = (v >> 5) & 63, b = v & 31;
            dst[x] = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((gg << 2) | (gg >> 4)) << 8) | ((b << 3) | (b >> 2));
        }
    }
    reinterpret_cast<FnUnlock>(vt[20])(tex, 0);
    return tex;
}

} // namespace

bool Load() {
    if (g.tried) return g.ok;
    g.tried = true;
    uint8_t* blob = nullptr;
    uint32_t size = 0;
    if (!ReadAll(kCatalogFile, &blob, &size)) { Fail("ts2catalog.bin nao encontrado"); return false; }
    g.blob = blob;
    const FileHeader* h = reinterpret_cast<const FileHeader*>(blob);
    if (strncmp(h->magic, "TS2C", 4) != 0 || h->version != 1 || h->recSize != sizeof(FileRec) || h->catCount > 24 ||
        h->offCats + h->catCount * 8u > size || h->offRecs + h->count * sizeof(FileRec) > size || h->offNames + h->namesLen > size ||
        h->namesLen == 0) {
        Fail("ts2catalog.bin invalido");
        Cleanup();
        return false;
    }
    char* names = reinterpret_cast<char*>(blob + h->offNames);
    names[h->namesLen - 1] = '\0';   // guarantee termination
    auto nameAt = [&](uint32_t o) -> const char* { return o < h->namesLen ? names + o : names; };

    g.catCount = static_cast<int>(h->catCount);
    const uint32_t* cats = reinterpret_cast<const uint32_t*>(blob + h->offCats);
    for (int i = 0; i < g.catCount; ++i) { g.catEn[i] = nameAt(cats[i * 2]); g.catPt[i] = nameAt(cats[i * 2 + 1]); }

    g.items = static_cast<Item*>(malloc(sizeof(Item) * (h->count ? h->count : 1)));
    g.folded = static_cast<char**>(malloc(sizeof(char*) * (h->count ? h->count : 1)));
    if (!g.items || !g.folded) { Fail("sem memoria"); Cleanup(); return false; }
    const FileRec* recs = reinterpret_cast<const FileRec*>(blob + h->offRecs);
    g.count = 0;
    g.mods = 0;
    uint32_t maxThumb = 0;
    for (uint32_t i = 0; i < h->count; ++i) {
        const FileRec& r = recs[i];
        Item& it = g.items[i];
        it.guid = r.guid; it.group = r.group; it.price = r.price; it.cat = r.cat; it.flags = r.flags; it.thumb = r.thumb;
        it.nameEn = nameAt(r.nameEn);
        it.namePt = r.namePt ? nameAt(r.namePt) : it.nameEn;
        it.src = nameAt(r.src);
        if (r.flags & kFlagMod) ++g.mods;
        if (r.thumb != kNoThumb && r.thumb + 1 > maxThumb) maxThumb = r.thumb + 1;
        char buf[512];
        size_t n = Fold(it.nameEn, buf, sizeof(buf) - 2);
        buf[n++] = ' ';
        n += Fold(it.namePt, buf + n, sizeof(buf) - n - 2);
        if (it.src[0]) { buf[n++] = ' '; n += Fold(it.src, buf + n, sizeof(buf) - n); }
        g.folded[i] = static_cast<char*>(malloc(n + 1));
        if (!g.folded[i]) { g.count = i; Fail("sem memoria"); Cleanup(); return false; }
        memcpy(g.folded[i], buf, n + 1);
        g.count = i + 1;
    }

    g.thumbs = CreateFileA(kThumbsFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g.thumbs != INVALID_HANDLE_VALUE) {
        uint32_t hdr[4] = {};
        DWORD got = 0;
        if (ReadFile(g.thumbs, hdr, sizeof(hdr), &got, nullptr) && got == sizeof(hdr) && hdr[0] == 0x54325354u /*'TS2T'*/ && hdr[3] == 64) {
            g.thumbCount = hdr[2];
        } else {
            CloseHandle(g.thumbs);
            g.thumbs = INVALID_HANDLE_VALUE;
        }
    }
    g.ok = true;
    g.err[0] = '\0';
    return true;
}

bool Available() { return g.ok; }
const char* LoadError() { return g.err; }
uint32_t Count() { return g.count; }
const Item& At(uint32_t i) { return g.items[i < g.count ? i : 0]; }
uint32_t ModCount() { return g.mods; }
int CategoryCount() { return g.catCount; }

const char* CategoryName(int c) {
    if (c < 0 || c >= g.catCount) return "";
    const bool en = ts2::i18n::Get() == ts2::i18n::Lang::EN;
    const char* s = en ? g.catEn[c] : g.catPt[c];
    return (s && s[0]) ? s : g.catEn[c];
}

const char* ItemName(const Item& it) {
    const bool en = ts2::i18n::Get() == ts2::i18n::Lang::EN;
    const char* s = en ? it.nameEn : it.namePt;
    return (s && s[0]) ? s : it.nameEn;
}

uint32_t Query(const char* text, int cat, int src, uint32_t* out, uint32_t max) {
    if (!g.ok) return 0;
    char q[96];
    Fold(text ? text : "", q, sizeof(q));
    // split into words
    const char* words[8];
    size_t wlen[8];
    int nw = 0;
    for (char* p = q; *p && nw < 8;) {
        while (*p == ' ') ++p;
        if (!*p) break;
        char* e = p;
        while (*e && *e != ' ') ++e;
        words[nw] = p; wlen[nw] = static_cast<size_t>(e - p); ++nw;
        p = e;
        if (*p) { *p = '\0'; ++p; }
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < g.count; ++i) {
        const Item& it = g.items[i];
        if (cat > 0 && it.cat != cat) continue;
        const bool mod = (it.flags & kFlagMod) != 0;
        if (src == SRC_BASE && mod) continue;
        if (src == SRC_MODS && !mod) continue;
        bool match = true;
        for (int w = 0; w < nw && match; ++w) { (void)wlen; if (!strstr(g.folded[i], words[w])) match = false; }
        if (!match) continue;
        if (out && n < max) out[n] = i;
        ++n;
    }
    return n;
}

void* Thumb(d3d9::IDirect3DDevice9* dev, uint32_t thumbIdx) {
    if (!dev || thumbIdx == kNoThumb) return nullptr;
    if (dev != g_cacheDev) { ReleaseTextures(); g_cacheDev = dev; }
    ++g_useClock;
    int freeSlot = -1, oldest = 0;
    for (int i = 0; i < kCacheN; ++i) {
        if (g_cache[i].tex && g_cache[i].idx == thumbIdx) { g_cache[i].used = g_useClock; return g_cache[i].tex; }
        if (!g_cache[i].tex) { if (freeSlot < 0) freeSlot = i; }
        else if (g_cache[i].used < g_cache[oldest].used || !g_cache[oldest].tex) oldest = i;
    }
    // at most a few uploads per rendered frame so that opening the catalog never hitches
    const uint32_t tick = GetTickCount() / 8;
    if (tick != g_budgetTick) { g_budgetTick = tick; g_frameBudget = 0; }
    if (g_frameBudget >= 6) return nullptr;
    ++g_frameBudget;
    void* tex = CreateThumbTexture(dev, thumbIdx);
    if (!tex) return nullptr;
    const int slot = freeSlot >= 0 ? freeSlot : oldest;
    if (g_cache[slot].tex) d3d9::ComRelease(g_cache[slot].tex);
    g_cache[slot] = {thumbIdx, tex, g_useClock};
    return tex;
}

void ReleaseTextures() {
    for (int i = 0; i < kCacheN; ++i) {
        if (g_cache[i].tex) d3d9::ComRelease(g_cache[i].tex);
        g_cache[i] = {};
    }
    g_cacheDev = nullptr;
}

} // namespace ts2::catalog
