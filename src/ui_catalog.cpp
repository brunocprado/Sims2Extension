#include "ts2/ui_catalog.hpp"
#include "ts2/catalog.hpp"
#include "ts2/coop_net.hpp"
#include "ts2/i18n.hpp"

namespace ts2::ui::catalog_ui {

namespace kit = ts2::ui::kit;
namespace cat = ts2::catalog;

namespace {

struct Geom { float x, y, w, h; };

Geom WindowGeom(float scrW, float scrH) {
    Geom g;
    g.w = scrW - 32.0f; if (g.w > 1080.0f) g.w = 1080.0f; if (g.w < 780.0f) g.w = scrW - 8.0f;
    g.h = scrH - 32.0f; if (g.h > 700.0f) g.h = 700.0f; if (g.h < 480.0f) g.h = scrH - 8.0f;
    g.x = (scrW - g.w) * 0.5f;
    g.y = (scrH - g.h) * 0.5f;
    return g;
}

// ---- state ---------------------------------------------------------------------------------------------------
bool     g_open = false;
bool     g_focus = false;
char     g_search[64] = "";
int      g_cat = 0;
int      g_src = cat::SRC_ALL;
int      g_scrollRow = 0;
int      g_wheel = 0;
int      g_selected = -1;        // index into the catalog (not into the result list)
int      g_rot = 0;              // 0..7
int      g_offX = 2, g_offY = 0;
uint32_t g_lastClickItem = 0xFFFFFFFFu;
DWORD    g_lastClickTick = 0;
bool     g_dragBar = false;

// result cache
uint32_t* g_res = nullptr;
uint32_t  g_resCap = 0, g_resN = 0;
uint32_t  g_catCounts[24] = {};
char      g_keySearch[64] = "\x01";
int       g_keyCat = -1, g_keySrc = -1, g_keyLang = -1;
uint32_t  g_keyCount = 0;

int Utf8Len(unsigned char c) { return c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1))); }

void Refresh() {
    const int lang = static_cast<int>(ts2::i18n::Get());
    if (g_keyCat == g_cat && g_keySrc == g_src && g_keyLang == lang && g_keyCount == cat::Count() && strcmp(g_keySearch, g_search) == 0) return;
    g_keyCat = g_cat; g_keySrc = g_src; g_keyLang = lang; g_keyCount = cat::Count();
    strncpy(g_keySearch, g_search, sizeof(g_keySearch) - 1);
    g_keySearch[sizeof(g_keySearch) - 1] = '\0';
    if (g_resCap < cat::Count()) {
        free(g_res);
        g_resCap = cat::Count() + 256;
        g_res = static_cast<uint32_t*>(malloc(sizeof(uint32_t) * g_resCap));
        if (!g_res) g_resCap = 0;
    }
    g_resN = g_res ? cat::Query(g_search, g_cat, g_src, g_res, g_resCap) : 0;
    if (g_resN > g_resCap) g_resN = g_resCap;
    for (int c = 0; c < 24; ++c) g_catCounts[c] = (c < cat::CategoryCount()) ? cat::Query(g_search, c, g_src, nullptr, 0) : 0;
    g_scrollRow = 0;
}

// Splits `s` in at most `maxLines` lines no wider than `maxW`, breaking at spaces when possible. Fills starts / lens.
int Wrap(kit::Font f, const char* s, float maxW, int maxLines, int* st, int* ln) {
    int pos = 0, seg = 0;
    while (s[pos] && seg < maxLines) {
        int p = pos, lastSpace = -1;
        float w = 0.0f;
        while (s[p]) {
            const int cl = Utf8Len(static_cast<unsigned char>(s[p]));
            char tmp[5] = {};
            for (int k = 0; k < cl && s[p + k]; ++k) tmp[k] = s[p + k];
            const float cw = kit::TextWidth(f, tmp);
            if (w + cw > maxW && p > pos) break;
            if (s[p] == ' ') lastSpace = p;
            w += cw;
            p += cl;
        }
        int end = p;
        if (s[p] && lastSpace > pos) end = lastSpace;
        st[seg] = pos; ln[seg] = end - pos; ++seg;
        pos = end;
        while (s[pos] == ' ') ++pos;
    }
    return seg;
}

// Draws wrapped text; the last allowed line is truncated with an ellipsis when the text does not fit.
void DrawWrapped(kit::Font f, float x, float y, float maxW, int maxLines, const char* s, DWORD col, bool center, float lineH) {
    int st[6], ln[6];
    if (maxLines > 6) maxLines = 6;
    const int n = Wrap(f, s, maxW, maxLines, st, ln);
    for (int i = 0; i < n; ++i) {
        char buf[200];
        int len = ln[i] < 190 ? ln[i] : 190;
        memcpy(buf, s + st[i], len);
        buf[len] = '\0';
        const bool last = (i == n - 1);
        const bool more = last && s[st[i] + ln[i]] != '\0';
        if (more) {
            // keep the rest of the text on the final line so that TextFit adds the ellipsis
            int rest = static_cast<int>(strlen(s + st[i]));
            if (rest > 190) rest = 190;
            memcpy(buf, s + st[i], rest);
            buf[rest] = '\0';
            kit::TextFit(f, x, y + i * lineH, maxW, buf, col);
        } else if (center) {
            kit::TextCenter(f, x + maxW * 0.5f, y + i * lineH, buf, col);
        } else {
            kit::Text(f, x, y + i * lineH, buf, col);
        }
    }
}

void Placeholder(float x, float y, float s, int category) {
    static const DWORD tints[14] = {
        kit::Col(120, 130, 150), kit::Col(214, 150, 110), kit::Col(170, 130, 220), kit::Col(200, 170, 110), kit::Col(120, 190, 200), kit::Col(110, 160, 230),
        kit::Col(110, 200, 220), kit::Col(240, 210, 110), kit::Col(220, 130, 160), kit::Col(150, 210, 130), kit::Col(110, 200, 140), kit::Col(230, 170, 120),
        kit::Col(160, 160, 170), kit::Col(140, 150, 170)};
    const DWORD t = tints[(category >= 0 && category < 14) ? category : 0];
    kit::RoundRect(x, y, s, s, 8.0f, kit::WithAlpha(t, 0.20f), kit::WithAlpha(t, 0.08f));
    kit::Diamond(x + s * 0.5f, y + s * 0.5f, s * 0.16f, kit::WithAlpha(t, 0.65f));
}

void DrawThumb(d3d9::IDirect3DDevice9* dev, const cat::Item& it, float x, float y, float s) {
    kit::RoundRect(x, y, s, s, 8.0f, kit::Col(20, 26, 42, 255), kit::Col(10, 14, 26, 255));   // the icons are rendered over black
    void* tex = cat::Thumb(dev, it.thumb);
    if (tex) kit::Image(tex, x + 2.0f, y + 2.0f, s - 4.0f, s - 4.0f);
    else if (it.thumb == cat::kNoThumb) Placeholder(x, y, s, it.cat);
    else Placeholder(x, y, s, it.cat);
}

void Price(char* out, size_t cap, uint32_t p) { _snprintf(out, cap, "$%u", static_cast<unsigned>(p)); }

} // namespace

bool IsOpen() { return g_open; }
void Open() {
    g_open = true;
    g_focus = true;
    cat::Load();
}
void Close() { g_open = false; g_focus = false; g_dragBar = false; }
void Toggle() { if (g_open) Close(); else Open(); }
bool SearchFocused() { return g_open && g_focus; }
bool HitTest(float x, float y, float scrW, float scrH) {
    if (!g_open) return false;
    const Geom g = WindowGeom(scrW, scrH);
    return x >= g.x && x <= g.x + g.w && y >= g.y && y <= g.y + g.h;
}
bool OnEscape() {
    if (!g_open) return false;
    if (g_focus && g_search[0]) { g_search[0] = '\0'; return true; }
    Close();
    return true;
}
void OnWheel(int notches) { g_wheel += notches; }

void OnChar(unsigned ch) {
    size_t len = strlen(g_search);
    if (ch == '\b') {
        while (len > 0 && (static_cast<unsigned char>(g_search[len - 1]) & 0xC0) == 0x80) g_search[--len] = '\0';
        if (len > 0) g_search[--len] = '\0';
    } else if (ch >= 32 && ch < 127) {
        if (len + 1 < sizeof(g_search)) { g_search[len] = static_cast<char>(ch); g_search[len + 1] = '\0'; }
    } else if (ch >= 0xA0 && ch <= 0xFF) {
        if (len + 2 < sizeof(g_search)) {
            g_search[len]     = static_cast<char>(0xC0 | (ch >> 6));
            g_search[len + 1] = static_cast<char>(0x80 | (ch & 0x3F));
            g_search[len + 2] = '\0';
        }
    } else if (ch == 13 || ch == 27) {
        g_focus = false;
    }
}

void Draw(d3d9::IDirect3DDevice9* dev, float scrW, float scrH) {
    if (!g_open) return;
    const Geom G = WindowGeom(scrW, scrH);
    const kit::Input& in = kit::GetInput();
    kit::SetTranslate(true);
    const bool loaded = cat::Load();

    kit::Rect(0.0f, 0.0f, scrW, scrH, kit::Col(4, 6, 12, 120));   // dim the game behind the window
    kit::WindowFrame(G.x, G.y, G.w, G.h);

    // ---- header ----------------------------------------------------------------------------------------------
    kit::Diamond(G.x + 30.0f, G.y + 29.0f, 7.5f, kit::Col(70, 220, 120));
    kit::Text(kit::F_TITLE, G.x + 50.0f, G.y + 29.0f - kit::FontHeight(kit::F_TITLE) * 0.5f, "Catálogo de construção", kit::Col(246, 250, 255));
    if (kit::Button(G.x + G.w - 16.0f - 30.0f, G.y + 14.0f, 30.0f, 30.0f, "X", false, kit::F_BOLD)) Close();
    if (!g_open) return;
    if (loaded) {
        Refresh();
        char buf[64];
        _snprintf(buf, sizeof(buf), ts2::i18n::Tr("%u objetos"), static_cast<unsigned>(g_resN));
        float pw = 0.0f;
        kit::Pill(G.x + G.w - 16.0f - 30.0f - 10.0f, G.y + 17.0f, buf, kit::kAccent, &pw, true);
    }

    if (!loaded) {
        kit::Text(kit::F_BOLD, G.x + 28.0f, G.y + 90.0f, "Catálogo não encontrado.", kit::kWarn);
        kit::Text(kit::F_BODY, G.x + 28.0f, G.y + 122.0f, "Gere com tools/catalog/build_catalog.py e copie ts2catalog.bin e ts2thumbs.bin", kit::kDim);
        kit::Text(kit::F_BODY, G.x + 28.0f, G.y + 146.0f, "para a pasta do jogo (TSBin).", kit::kDim);
        return;
    }

    // ---- toolbar: search + source chips -------------------------------------------------------------------------
    const float tbY = G.y + 58.0f, tbH = 34.0f;
    const float chipW = 104.0f;
    const float chipsX = G.x + G.w - 16.0f - chipW * 3.0f - 12.0f;
    const float sbX = G.x + 16.0f, sbW = chipsX - sbX - 14.0f;
    {
        const bool hov = kit::Hover(sbX, tbY, sbW, tbH);
        kit::RoundRect(sbX, tbY, sbW, tbH, 9.0f, kit::Col(10, 14, 26, 235), kit::Col(18, 24, 40, 235));
        kit::RoundOutline(sbX, tbY, sbW, tbH, 9.0f, g_focus ? 1.5f : 1.0f, g_focus ? kit::kAccent : (hov ? kit::Col(150, 190, 255, 130) : kit::Col(255, 255, 255, 30)));
        if (in.clicked) g_focus = hov;
        // magnifier glyph made of shapes
        const float mx = sbX + 20.0f, my = tbY + tbH * 0.5f - 1.0f;
        kit::RoundOutline(mx - 6.0f, my - 6.0f, 12.0f, 12.0f, 6.0f, 1.6f, kit::kDim);
        kit::RoundRect(mx + 4.0f, my + 5.0f, 7.0f, 2.2f, 1.0f, kit::kDim);
        const float tx = sbX + 40.0f, ty = tbY + (tbH - kit::FontHeight(kit::F_BODY)) * 0.5f;
        kit::SetTranslate(false);
        if (g_search[0]) {
            char shown[96];
            _snprintf(shown, sizeof(shown), "%s%s", g_search, (g_focus && ((GetTickCount() / 500) & 1)) ? "_" : "");
            kit::TextFit(kit::F_BODY, tx, ty, sbW - 80.0f, shown, kit::kText);
        } else if (g_focus && ((GetTickCount() / 500) & 1)) {
            kit::Text(kit::F_BODY, tx, ty, "_", kit::kText);
        }
        kit::SetTranslate(true);
        if (!g_search[0] && !g_focus) kit::Text(kit::F_BODY, tx, ty, "Buscar por nome...", kit::kDim);
        if (g_search[0] && kit::Button(sbX + sbW - 32.0f, tbY + 6.0f, 24.0f, 22.0f, "x", false, kit::F_SMALL)) { g_search[0] = '\0'; g_focus = true; }
    }
    {
        char modLbl[32];
        _snprintf(modLbl, sizeof(modLbl), ts2::i18n::Tr("Mods (%u)"), static_cast<unsigned>(cat::ModCount()));
        if (kit::Button(chipsX, tbY, chipW, tbH, "Tudo", g_src == cat::SRC_ALL)) g_src = cat::SRC_ALL;
        if (kit::Button(chipsX + chipW + 6.0f, tbY, chipW, tbH, "Jogo base", g_src == cat::SRC_BASE)) g_src = cat::SRC_BASE;
        if (kit::Button(chipsX + (chipW + 6.0f) * 2.0f, tbY, chipW, tbH, modLbl, g_src == cat::SRC_MODS)) g_src = cat::SRC_MODS;
    }

    // ---- body geometry ------------------------------------------------------------------------------------------
    const float bodyY = tbY + tbH + 12.0f;
    const float footH = 34.0f;
    const float bodyH = G.y + G.h - footH - bodyY;
    const float sideW = 188.0f, detW = 266.0f, pad = 16.0f, gapX = 12.0f;
    const float sideX = G.x + pad;
    const float gridX = sideX + sideW + gapX;
    const float detX = G.x + G.w - pad - detW;
    const float gridW = detX - gapX - gridX;

    // ---- categories ---------------------------------------------------------------------------------------------
    {
        kit::RoundRect(sideX, bodyY, sideW, bodyH, 10.0f, kit::Col(8, 12, 22, 150), kit::Col(12, 18, 32, 150));
        const int n = cat::CategoryCount();
        float rowH = (bodyH - 12.0f) / static_cast<float>(n > 0 ? n : 1);
        if (rowH > 32.0f) rowH = 32.0f;
        kit::SetTranslate(false);
        for (int c = 0; c < n; ++c) {
            const float ry = bodyY + 6.0f + c * rowH;
            const bool act = (g_cat == c);
            const bool hov = kit::Hover(sideX + 6.0f, ry, sideW - 12.0f, rowH - 2.0f);
            if (act) {
                kit::RoundRect(sideX + 6.0f, ry, sideW - 12.0f, rowH - 2.0f, 7.0f, kit::Col(70, 156, 250, 235), kit::Col(44, 110, 222, 235));
            } else if (hov) {
                kit::RoundRect(sideX + 6.0f, ry, sideW - 12.0f, rowH - 2.0f, 7.0f, kit::Col(255, 255, 255, 22));
            }
            const float fh = kit::FontHeight(kit::F_BODY);
            const DWORD tc = act ? kit::Col(255, 255, 255) : (g_catCounts[c] ? kit::kText : kit::Col(120, 132, 156));
            kit::TextFit(kit::F_BODY, sideX + 16.0f, ry + (rowH - 2.0f - fh) * 0.5f, sideW - 74.0f, cat::CategoryName(c), tc);
            char cn[16];
            _snprintf(cn, sizeof(cn), "%u", static_cast<unsigned>(g_catCounts[c]));
            kit::TextRight(kit::F_SMALL, sideX + sideW - 16.0f, ry + (rowH - 2.0f - kit::FontHeight(kit::F_SMALL)) * 0.5f, cn, act ? kit::Col(225, 238, 255) : kit::kDim);
            if (hov && in.clicked) { g_cat = c; g_focus = false; }
        }
        kit::SetTranslate(true);
    }

    // ---- grid ---------------------------------------------------------------------------------------------------
    {
        const float cardW = 118.0f, cardH = 142.0f, gap = 10.0f, barW = 10.0f;
        const float usable = gridW - barW - 4.0f;
        int cols = static_cast<int>((usable + gap) / (cardW + gap));
        if (cols < 1) cols = 1;
        const float gridPad = (usable - (cols * cardW + (cols - 1) * gap)) * 0.5f;
        int rowsVis = static_cast<int>((bodyH + gap) / (cardH + gap));
        if (rowsVis < 1) rowsVis = 1;
        const int totalRows = static_cast<int>((g_resN + cols - 1) / cols);
        const int maxScroll = totalRows > rowsVis ? totalRows - rowsVis : 0;

        const bool overGrid = kit::Hover(gridX, bodyY, gridW, bodyH);
        if (g_wheel != 0) { g_scrollRow -= g_wheel; g_wheel = 0; }
        if (g_scrollRow < 0) g_scrollRow = 0;
        if (g_scrollRow > maxScroll) g_scrollRow = maxScroll;

        // scrollbar
        const float trackX = gridX + gridW - barW, trackY = bodyY, trackH = bodyH;
        if (maxScroll > 0) {
            const float thumbH = trackH * static_cast<float>(rowsVis) / static_cast<float>(totalRows);
            const float th = thumbH < 28.0f ? 28.0f : thumbH;
            const bool hovT = kit::Hover(trackX - 4.0f, trackY, barW + 8.0f, trackH);
            if (in.clicked && hovT) g_dragBar = true;
            if (!in.down) g_dragBar = false;
            if (g_dragBar) {
                float t = (in.my - trackY - th * 0.5f) / (trackH - th);
                if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
                g_scrollRow = static_cast<int>(t * maxScroll + 0.5f);
            }
            const float ty = trackY + (trackH - th) * (static_cast<float>(g_scrollRow) / static_cast<float>(maxScroll));
            kit::RoundRect(trackX, trackY, barW, trackH, 5.0f, kit::Col(255, 255, 255, 18));
            kit::RoundRect(trackX, ty, barW, th, 5.0f, (hovT || g_dragBar) ? kit::Col(150, 200, 255, 220) : kit::Col(110, 150, 220, 170));
        }
        (void)overGrid;

        if (g_resN == 0) {
            kit::TextCenter(kit::F_BODY, gridX + gridW * 0.5f, bodyY + bodyH * 0.4f, "Nenhum objeto encontrado.", kit::kDim);
            if (g_src == cat::SRC_MODS && cat::ModCount() == 0) {
                kit::TextCenter(kit::F_SMALL, gridX + gridW * 0.5f, bodyY + bodyH * 0.4f + 28.0f, "Nenhum mod no catálogo. Rode build_catalog.py com a pasta Downloads (--mods).", kit::kDim);
            }
        }

        for (int r = 0; r < rowsVis; ++r) {
            for (int c = 0; c < cols; ++c) {
                const int li = (g_scrollRow + r) * cols + c;
                if (li >= static_cast<int>(g_resN)) break;
                const uint32_t idx = g_res[li];
                const cat::Item& it = cat::At(idx);
                const float cx = gridX + gridPad + c * (cardW + gap), cy = bodyY + r * (cardH + gap);
                const bool sel = (static_cast<int>(idx) == g_selected);
                const bool hov = kit::Hover(cx, cy, cardW, cardH);
                kit::RoundRect(cx, cy, cardW, cardH, 10.0f,
                               sel ? kit::Col(52, 84, 140, 245) : (hov ? kit::Col(48, 60, 88, 240) : kit::Col(34, 42, 64, 236)),
                               sel ? kit::Col(34, 58, 104, 245) : (hov ? kit::Col(36, 46, 70, 240) : kit::Col(24, 30, 48, 236)));
                kit::RoundOutline(cx, cy, cardW, cardH, 10.0f, sel ? 1.8f : 1.0f, sel ? kit::kAccent : (hov ? kit::Col(150, 200, 255, 120) : kit::Col(255, 255, 255, 22)));
                kit::SetTranslate(false);
                DrawThumb(dev, it, cx + 11.0f, cy + 8.0f, 96.0f);
                if (it.flags & cat::kFlagMod) {
                    kit::RoundRect(cx + 11.0f + 96.0f - 34.0f, cy + 12.0f, 32.0f, 15.0f, 7.0f, kit::Col(255, 190, 70, 245), kit::Col(236, 150, 40, 245));
                    kit::TextCenter(kit::F_SMALL, cx + 11.0f + 96.0f - 18.0f, cy + 12.0f + (15.0f - kit::FontHeight(kit::F_SMALL)) * 0.5f, "MOD", kit::Col(40, 24, 0));
                }
                DrawWrapped(kit::F_SMALL, cx + 6.0f, cy + 108.0f - 0.0f, cardW - 12.0f, 2, cat::ItemName(it), kit::kText, true, 14.0f);
                kit::SetTranslate(true);
                if (hov && in.clicked) {
                    // like the native catalog: click the object, then click the lot (the window hides meanwhile)
                    g_selected = static_cast<int>(idx);
                    g_focus = false;
                    ts2::coop::PlaceModeBegin(it.guid);
                    Close();
                }
            }
        }
    }

    // ---- details + placement --------------------------------------------------------------------------------------
    {
        kit::RoundRect(detX, bodyY, detW, bodyH, 10.0f, kit::Col(8, 12, 22, 150), kit::Col(12, 18, 32, 150));
        float y = bodyY + 12.0f;
        const float ix = detX + 14.0f, iw = detW - 28.0f;
        const cat::Item* sel = (g_selected >= 0 && static_cast<uint32_t>(g_selected) < cat::Count()) ? &cat::At(static_cast<uint32_t>(g_selected)) : nullptr;
        if (sel) {
            kit::SetTranslate(false);
            DrawThumb(dev, *sel, detX + (detW - 112.0f) * 0.5f, y, 112.0f);
            y += 122.0f;
            DrawWrapped(kit::F_BOLD, ix, y, iw, 3, cat::ItemName(*sel), kit::kText, true, 18.0f);
            y += 3 * 18.0f + 4.0f;
            char line[160];
            Price(line, sizeof(line), sel->price);
            kit::TextCenter(kit::F_BOLD, detX + detW * 0.5f, y, line, kit::kGood);
            y += 22.0f;
            kit::SetTranslate(true);
            _snprintf(line, sizeof(line), ts2::i18n::Tr("Categoria: %s"), cat::CategoryName(sel->cat));
            kit::SetTranslate(false);
            kit::TextFit(kit::F_SMALL, ix, y, iw, line, kit::kDim);
            y += 16.0f;
            if (sel->flags & cat::kFlagMod) _snprintf(line, sizeof(line), ts2::i18n::Tr("Origem: mod (%s)"), sel->src);
            else _snprintf(line, sizeof(line), "%s", ts2::i18n::Tr("Origem: jogo base"));
            kit::TextFit(kit::F_SMALL, ix, y, iw, line, (sel->flags & cat::kFlagMod) ? kit::kWarn : kit::kDim);
            y += 16.0f;
            _snprintf(line, sizeof(line), "GUID %08X", static_cast<unsigned>(sel->guid));
            kit::Text(kit::F_SMALL, ix, y, line, kit::Col(110, 124, 150));
            kit::SetTranslate(true);
            y += 24.0f;
        } else {
            kit::TextCenter(kit::F_BODY, detX + detW * 0.5f, y + 40.0f, "Selecione um objeto na lista.", kit::kDim);
            y += 150.0f;
        }

        // single action: place the selected object (rotation / offset live on the placed object in the game itself)
        const float by = bodyY + bodyH - 12.0f - 42.0f;
        if (kit::Button(ix, by, iw, 42.0f, "Colocar no lote", sel != nullptr, kit::F_BOLD) && sel) {
            ts2::coop::PlaceModeBegin(sel->guid);
            Close();
        }
        if (ts2::coop::PlacedCount() > 0) {
            char l[64];
            _snprintf(l, sizeof(l), ts2::i18n::Tr("Desfazer último (%u)"), static_cast<unsigned>(ts2::coop::PlacedCount()));
            if (kit::Button(ix, by - 34.0f, iw, 28.0f, l, false, kit::F_SMALL)) ts2::coop::PlaceDeleteLast();
        }
    }

    // ---- footer: status + hints ---------------------------------------------------------------------------------
    {
        const float fy = G.y + G.h - footH + 8.0f;
        kit::Rect(G.x + 20.0f, G.y + G.h - footH, G.w - 40.0f, 1.0f, kit::Col(255, 255, 255, 22));
        const char* st = ts2::coop::PlaceStatus();
        if (st && st[0]) {
            kit::SetTranslate(false);
            kit::TextFit(kit::F_SMALL, G.x + 22.0f, fy, G.w * 0.55f, st, kit::kWarn);
            kit::SetTranslate(true);
        }
        kit::TextRight(kit::F_SMALL, G.x + G.w - 22.0f, fy, "Clique no objeto e depois no lote  •  Roda do mouse rola  •  Esc fecha", kit::kDim);
    }
}

} // namespace ts2::ui::catalog_ui
