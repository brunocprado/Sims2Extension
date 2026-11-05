#include "ts2/dx9_overlay.hpp"
#include "ts2/addresses.hpp"
#include "ts2/hook_engine.hpp"
#include "ts2/ts2se_engine.hpp"
#include "ts2/coop_net.hpp"
#include "ts2/engine_qol.hpp"
#include "ts2/dx9_postfx.hpp"

#include "ts2/ui_kit.hpp"
#include "ts2/i18n.hpp"
#include "ts2/ui_tabs.hpp"
namespace ts2::ui {

namespace {

// ============================================================================
// Drawing now lives in ui_kit (anti-aliased font, rounded shapes, widgets). These thin shims keep the
// tab code below readable; `dev` arguments are kept for source compatibility and ignored.
// ============================================================================
namespace kit = ts2::ui::kit;
using kit::g_lay;
using Dev = d3d9::IDirect3DDevice9;

static constexpr float kRowH = kit::kRowH;
static constexpr float kGap  = kit::kGap;
static constexpr DWORD kColText   = kit::kText;
static constexpr DWORD kColDim    = kit::kDim;
static constexpr DWORD kColAccent = kit::kAccent;
static constexpr DWORD kColGood   = kit::kGood;
static constexpr DWORD kColWarn   = kit::kWarn;
static constexpr DWORD kColBad    = kit::kBad;

// Old code expressed text size as a 5x7 pixel-font scale; map it to the closest real font.
static kit::Font FontForScale(float s) { return s >= 1.45f ? kit::F_BOLD : (s < 1.28f ? kit::F_SMALL : kit::F_BODY); }
static void DrawText5x7(Dev*, float x, float y, const char* text, DWORD color, float scale = 1.3f) {
    kit::Text(FontForScale(scale), x, y, text, color);
}

// ============================================================================
// UI State & Input Handling
// ============================================================================
static bool    g_visible         = true;
static int     g_activeTab       = 0; // 0=Sim, 1=Construir, 2=Jogo, 3=Graficos, 4=Co-op, 5=Avancado
static float   g_mouseX          = 0.0f;
static float   g_mouseY          = 0.0f;
static bool    g_mouseDown       = false;
static bool    g_mouseClicked    = false;
static int     g_focusedInputId  = 0; // 0=none, 1=CoopIP, 2=CheatCmd, 3=CatalogFilter
static char    g_coopIpInput[32] = "127.0.0.1";
static char    g_cmdInput[128]   = "ts2se.help";
static char    g_lastCmdOut[256] = "Digite um comando ou cheat e clique em [Executar].";
static HWND    g_gameHwnd        = nullptr;
static WNDPROC g_origWndProc     = nullptr;
static DWORD   g_lastToggleTick  = 0;
static DWORD   g_lastF5Tick      = 0;
static DWORD   g_lastF9Tick      = 0;
static DWORD   g_lastF11Tick     = 0;
static DWORD   g_lastClickTick   = 0;
static bool    g_prevLButtonDown = false;
static uint32_t g_lastRenderedFrame = 0;
static uint32_t g_frameCounter      = 0;

// Overlay window geometry (also used to swallow mouse clicks so they never reach the game)
static constexpr float kWinX = 16.0f, kWinY = 16.0f, kWinW = 680.0f, kWinH = 620.0f;

static bool Button(Dev*, float x, float y, float w, float h, const char* label, bool active = false) {
    return kit::Button(x, y, w, h, label, active);
}

static void AppendCharToInput(char* buf, size_t maxLen, char ch) {
    size_t len = strlen(buf);
    if (ch == '\b') {
        if (len > 0) buf[len - 1] = '\0';
    } else if (ch >= 32 && ch <= 126 && len + 1 < maxLen) {
        buf[len]     = ch;
        buf[len + 1] = '\0';
    }
}

static LRESULT CALLBACK HookedWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        DWORD now = GetTickCount();
        if ((wParam == VK_F10 || wParam == VK_INSERT) && (now - g_lastToggleTick > 220)) {
            g_lastToggleTick = now;
            g_visible = !g_visible;
            return 0;
        }
        if (wParam == VK_F5 && (now - g_lastF5Tick > 350)) {
            g_lastF5Tick = now;
            ts2::qol::TriggerSmartSaveNow();
            return 0;
        }
        if (wParam == VK_F9 && (now - g_lastF9Tick > 350)) {
            g_lastF9Tick = now;
            ts2::coop::ForceResyncNow();
            return 0;
        }
        if (wParam == VK_F11 && (now - g_lastF11Tick > 350)) {
            g_lastF11Tick = now;
            ts2::qol::ToggleBorderlessFullscreen(hWnd);
            return 0;
        }
        if (g_visible && wParam == VK_RETURN && g_focusedInputId == 2) {
            ts2::se::ExecuteCommand(g_cmdInput, g_lastCmdOut, sizeof(g_lastCmdOut));
            return 0;
        }
    }

    if (g_visible) {
        if (msg == WM_MOUSEMOVE) {
            g_mouseX = static_cast<float>(static_cast<int16_t>(lParam & 0xFFFF));
            g_mouseY = static_cast<float>(static_cast<int16_t>((lParam >> 16) & 0xFFFF));
        } else if (msg == WM_LBUTTONDOWN) {
            g_mouseX = static_cast<float>(static_cast<int16_t>(lParam & 0xFFFF));
            g_mouseY = static_cast<float>(static_cast<int16_t>((lParam >> 16) & 0xFFFF));
            g_mouseDown = true;
            DWORD now = GetTickCount();
            if (now - g_lastClickTick > 140) {
                g_lastClickTick   = now;
                g_mouseClicked    = true;
                g_prevLButtonDown = true;
            }
            // Block clicks inside our overlay window from hitting the game
            if (g_mouseX >= kWinX && g_mouseX <= kWinX + kWinW && g_mouseY >= kWinY && g_mouseY <= kWinY + kWinH) {
                return 0;
            }
        } else if (msg == WM_LBUTTONUP) {
            g_mouseDown = false;
        } else if (msg == WM_CHAR && g_focusedInputId != 0) {
            char ch = static_cast<char>(wParam);
            if (g_focusedInputId == 1) {
                AppendCharToInput(g_coopIpInput, sizeof(g_coopIpInput), ch);
            } else if (g_focusedInputId == 2) {
                AppendCharToInput(g_cmdInput, sizeof(g_cmdInput), ch);
            } else if (g_focusedInputId == 3) {
                AppendCharToInput(ts2::qol::GetConfig().catalogSearchFilter,
                                  sizeof(ts2::qol::GetConfig().catalogSearchFilter), ch);
            }
            return 0;
        }
    }

    return g_origWndProc ? CallWindowProcA(g_origWndProc, hWnd, msg, wParam, lParam) : 0;
}

static void EnsureWndProcHooked(HWND hWnd) {
    if (!hWnd || g_gameHwnd == hWnd) return;
    g_gameHwnd = hWnd;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongA(hWnd, GWL_WNDPROC, reinterpret_cast<LONG>(&HookedWndProc))
    );
    char titleBuf[128];
    _snprintf(titleBuf, sizeof(titleBuf),
              "The Sims 2 EP9 [TS2SE + Co-op Lockstep + QoL | PID %u | F10 = Overlay]",
              static_cast<unsigned>(GetCurrentProcessId()));
    SetWindowTextA(hWnd, titleBuf);
    ts2::hook::Log("[DX9Overlay] Subclassed game HWND 0x%08X (orig WndProc=0x%08X)",
                   reinterpret_cast<unsigned>(hWnd), reinterpret_cast<unsigned>(g_origWndProc));
}

// ============================================================================
// Layout shims (the stack layout itself is in ui_kit)
// ============================================================================
static void LayBegin(float x, float y, float w) { kit::LayBegin(x, y, w); }
static void LaySection(Dev*, const char* title, const char* right = nullptr, DWORD rightColor = kColText) { kit::LaySection(title, right, rightColor); }
static void LayText(Dev*, const char* text, DWORD color = kColText, float scale = 1.3f) { kit::LayText(text, color, FontForScale(scale)); }
static void LayNext() { kit::LayNext(); }
static bool LayBtn(Dev*, int col, int cols, const char* label, bool active = false, int span = 1) { return kit::LayBtn(col, cols, label, active, span); }

static void RunCmd(const char* cmd) {
    char out[128];
    ts2::se::ExecuteCommand(cmd, out, sizeof(out));
}

// ============================================================================
// Tab 0: Sim (time, money, selected Sim)
// ============================================================================
static void RenderTabSim(d3d9::IDirect3DDevice9* dev, float x, float y, float w) {
    LayBegin(x, y, w);

    // --- Time ---
    uint16_t hr = 0, mn = 0, sub = 0, day = 0;
    ts2::se::GetSimulatorClock(hr, mn, sub, day);
    char clock[32];
    _snprintf(clock, sizeof(clock), "%02u:%02u", hr, mn);
    LaySection(dev, "TEMPO", clock);
    const int16_t speed = ts2::se::GetSimulatorSpeed();
    const bool paused   = ts2::se::GetSimulatorPauseCount() > 0;
    if (LayBtn(dev, 0, 4, paused ? "Pausado" : "Pausar", paused)) ts2::se::SetSimulatorPaused(!paused);
    if (LayBtn(dev, 1, 4, "Velocidade 1x", !paused && speed == 1)) { ts2::se::SetSimulatorPaused(false); ts2::se::SetSimulatorSpeed(1); }
    if (LayBtn(dev, 2, 4, "Velocidade 2x", !paused && speed == 2)) { ts2::se::SetSimulatorPaused(false); ts2::se::SetSimulatorSpeed(2); }
    if (LayBtn(dev, 3, 4, "Velocidade 3x", !paused && speed == 3)) { ts2::se::SetSimulatorPaused(false); ts2::se::SetSimulatorSpeed(3); }
    LayNext();
    if (LayBtn(dev, 0, 4, "Manha (07h)"))    ts2::se::SetSimulatorHour(7);
    if (LayBtn(dev, 1, 4, "Meio-dia (12h)")) ts2::se::SetSimulatorHour(12);
    if (LayBtn(dev, 2, 4, "Tarde (18h)"))    ts2::se::SetSimulatorHour(18);
    if (LayBtn(dev, 3, 4, "Noite (00h)"))    ts2::se::SetSimulatorHour(0);
    LayNext();

    // --- Money ---
    char funds[40];
    _snprintf(funds, sizeof(funds), "$%d", ts2::se::GetFamilyFunds());
    LaySection(dev, "DINHEIRO DA FAMILIA", funds, kColWarn);
    if (LayBtn(dev, 0, 4, "+ $1.000"))   ts2::se::ModifyFamilyFunds(1000);
    if (LayBtn(dev, 1, 4, "+ $50.000"))  ts2::se::ModifyFamilyFunds(50000);
    if (LayBtn(dev, 2, 4, "+ $250.000")) ts2::se::ModifyFamilyFunds(250000);
    if (LayBtn(dev, 3, 4, "- $10.000"))  ts2::se::ModifyFamilyFunds(-10000);
    LayNext();

    // --- Selected Sim ---
    LaySection(dev, "SIM SELECIONADO");
    const bool motLocked = ts2::se::IsMotiveDecayLocked();
    const bool aspLocked = ts2::se::IsAspirationLocked();
    if (LayBtn(dev, 0, 3, "Encher necessidades"))        ts2::se::FillAllActiveSimMotives();
    if (LayBtn(dev, 1, 3, "Congelar necessidades", motLocked)) ts2::se::SetMotiveDecayLocked(!motLocked);
    if (LayBtn(dev, 2, 3, "Congelar aspiracao", aspLocked))    ts2::se::SetAspirationLocked(!aspLocked);
    LayNext();
    if (LayBtn(dev, 0, 4, "Aspiracao Platina"))   ts2::se::SetSelectedSimAspirationLevel(5);
    if (LayBtn(dev, 1, 4, "Aspiracao Ouro"))      ts2::se::SetSelectedSimAspirationLevel(4);
    if (LayBtn(dev, 2, 4, "+50.000 pts aspir."))  ts2::se::AddSelectedSimAspirationPoints(50000);
    if (LayBtn(dev, 3, 4, "Trocar Sim ativo"))    ts2::se::CycleSelectedLotSim();
    LayNext();

    // --- Core needs (read-only bars + MAX) ---
    LaySection(dev, "NECESSIDADES");
    static const int   kNeed[8]      = {7, 1, 8, 9, 5, 6, 14, 15};
    static const char* kNeedName[8]  = {"Fome", "Conforto", "Higiene", "Banheiro", "Energia", "Diversão", "Social", "Ambiente"};
    const float half = (g_lay.w - 20.0f) / 2.0f;
    for (int i = 0; i < 8; ++i) {
        const float cx = g_lay.x + (i % 2) * (half + 20.0f);
        const float cy = g_lay.y + (i / 2) * 28.0f;
        const float val = ts2::se::GetSimMotive(kNeed[i]);
        kit::Text(kit::F_BODY, cx, cy + 3.0f, kNeedName[i], kColText);
        float norm = (val + 100.0f) / 200.0f;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        const float bx = cx + 86.0f, bw = half - 86.0f - 52.0f;
        kit::RoundRect(bx, cy + 5.0f, bw, 10.0f, 5.0f, kit::Col(14, 18, 30, 230), kit::Col(30, 38, 58, 230));
        const DWORD bcTop = (norm > 0.6f) ? kit::Col(96, 232, 150) : (norm > 0.3f) ? kit::Col(255, 205, 90) : kit::Col(255, 120, 108);
        const DWORD bcBot = (norm > 0.6f) ? kit::Col(48, 184, 110) : (norm > 0.3f) ? kit::Col(224, 160, 40) : kit::Col(222, 76, 66);
        if (norm > 0.01f) kit::RoundRect(bx, cy + 5.0f, bw * norm < 10.0f ? 10.0f : bw * norm, 10.0f, 5.0f, bcTop, bcBot);
        if (kit::Button(cx + half - 46.0f, cy, 46.0f, 22.0f, "MAX", false, kit::F_SMALL)) ts2::se::SetSimMotive(kNeed[i], 100.0f);
    }
    g_lay.y += 4 * 28.0f;
}

// ============================================================================
// Tab 1: Build / Buy
// ============================================================================
static void RenderTabBuild(d3d9::IDirect3DDevice9* dev, float x, float y, float w) {
    auto& cfg = ts2::qol::GetConfig();
    LayBegin(x, y, w);

    LaySection(dev, "COLOCAR OBJETOS");
    if (LayBtn(dev, 0, 2, "Mover objetos livremente", cfg.moveObjectsEnabled)) {
        cfg.moveObjectsEnabled = !cfg.moveObjectsEnabled;
        RunCmd(cfg.moveObjectsEnabled ? "moveObjects on" : "moveObjects off");
    }
    if (LayBtn(dev, 1, 2, "Rotacao de 45 graus", cfg.rotate45DegreeEnabled)) {
        cfg.rotate45DegreeEnabled = !cfg.rotate45DegreeEnabled;
        RunCmd(cfg.rotate45DegreeEnabled ? "boolProp allow45DegreeAngleOfRotation true"
                                         : "boolProp allow45DegreeAngleOfRotation false");
    }
    LayNext();
    if (LayBtn(dev, 0, 2, "Posicao em 1/4 de tile", cfg.quarterTileEnabled)) {
        cfg.quarterTileEnabled = !cfg.quarterTileEnabled;
        RunCmd(cfg.quarterTileEnabled ? "setQuarterTilePlacement on" : "setQuarterTilePlacement off");
    }
    if (LayBtn(dev, 1, 2, "Fora da grade (sem snap)", !cfg.snapObjectsToGrid)) {
        cfg.snapObjectsToGrid = !cfg.snapObjectsToGrid;
        ts2::se::SetNativeBoolProperty("snapObjectsToGrid", cfg.snapObjectsToGrid);
        RunCmd(cfg.snapObjectsToGrid ? "boolProp snapObjectsToGrid true" : "boolProp snapObjectsToGrid false");
    }
    LayNext();
    if (LayBtn(dev, 0, 2, "Terreno e piso livres", !cfg.constrainFloorElevation)) {
        cfg.constrainFloorElevation = !cfg.constrainFloorElevation;
        ts2::se::SetNativeBoolProperty("constrainFloorElevation", cfg.constrainFloorElevation);
        RunCmd(cfg.constrainFloorElevation ? "boolProp constrainFloorElevation true"
                                           : "boolProp constrainFloorElevation false");
    }
    if (LayBtn(dev, 1, 2, "Construir em dorm/apto", cfg.unlockDormAndAptTools)) {
        cfg.unlockDormAndAptTools = !cfg.unlockDormAndAptTools;
        const bool disabledFlag = !cfg.unlockDormAndAptTools;
        ts2::se::SetNativeBoolProperty("dormSpecificToolsDisabled", disabledFlag);
        ts2::se::SetNativeBoolProperty("aptBaseLotSpecificToolsDisabled", disabledFlag);
        ts2::se::SetNativeBoolProperty("aptSubLotSpecificToolsDisabled", disabledFlag);
    }
    LayNext();

    LaySection(dev, "LIMITES DO LOTE");
    static const int kFloors[4] = {5, 8, 12, 16};
    for (int i = 0; i < 4; ++i) {
        char lbl[24];
        _snprintf(lbl, sizeof(lbl), ts2::i18n::Tr("%d andares"), kFloors[i]);
        if (LayBtn(dev, i, 4, lbl, cfg.highestAllowedLevel == kFloors[i])) {
            cfg.highestAllowedLevel = kFloors[i];
            char cmd[48];
            _snprintf(cmd, sizeof(cmd), "setHighestAllowedLevel %d", kFloors[i]);
            RunCmd(cmd);
        }
    }
    LayNext();
    if (LayBtn(dev, 0, 3, "Telhado 15 graus")) RunCmd("roofSlopeAngle 15");
    if (LayBtn(dev, 1, 3, "Telhado 30 graus")) RunCmd("roofSlopeAngle 30");
    if (LayBtn(dev, 2, 3, "Telhado 45 graus")) RunCmd("roofSlopeAngle 45");
    LayNext();
    if (LayBtn(dev, 0, 3, "Zona residencial"))  RunCmd("changeLotZoning residential");
    if (LayBtn(dev, 1, 3, "Zona comunitaria"))  RunCmd("changeLotZoning community");
    if (LayBtn(dev, 2, 3, "Zona apartamento"))  RunCmd("changeLotZoning apartmentbase");
    LayNext();

    LaySection(dev, "BUSCAR NO CATALOGO");
    char box[96];
    _snprintf(box, sizeof(box), "%s%s", cfg.catalogSearchFilter[0] || g_focusedInputId == 3 ? cfg.catalogSearchFilter : ts2::i18n::Tr("clique e digite..."),
              (g_focusedInputId == 3) ? "_" : "");
    if (LayBtn(dev, 0, 4, box, g_focusedInputId == 3, 3)) g_focusedInputId = 3;
    if (LayBtn(dev, 3, 4, "Limpar")) cfg.catalogSearchFilter[0] = '\0';
    LayNext();
    const ts2::qol::CatalogEntry* matches[5] = {};
    const size_t n = ts2::qol::FilterCatalogEntries(cfg.catalogSearchFilter, matches, 5);
    for (size_t i = 0; i < n; ++i) {
        char row[160];
        _snprintf(row, sizeof(row), "%s  \xE2\x80\xA2  %s  \xE2\x80\xA2  $%d", matches[i]->name, matches[i]->category, matches[i]->price);
        kit::Text(kit::F_SMALL, g_lay.x + 4.0f, g_lay.y + i * 18.0f, row, kColText);
    }
}

// ============================================================================
// Tab 2: Game (window, performance, camera, saving)
// ============================================================================
static void RenderTabGame(d3d9::IDirect3DDevice9* dev, float x, float y, float w) {
    auto& cfg = ts2::qol::GetConfig();
    LayBegin(x, y, w);

    LaySection(dev, "JANELA E DESEMPENHO");
    if (LayBtn(dev, 0, 2, "Tela cheia sem bordas (F11)", cfg.borderlessFullscreen)) ts2::qol::ToggleBorderlessFullscreen(g_gameHwnd);
    if (LayBtn(dev, 1, 2, "Continuar rodando em 2o plano", cfg.backgroundSimEnabled)) ts2::qol::SetBackgroundSimEnabled(!cfg.backgroundSimEnabled);
    LayNext();
    if (LayBtn(dev, 0, 2, "Desbloquear FPS (sem VSync)", !cfg.vsyncEnabled)) {
        cfg.vsyncEnabled = !cfg.vsyncEnabled;
        RunCmd(cfg.vsyncEnabled ? "vsync on" : "vsync off");
    }
    if (LayBtn(dev, 1, 2, "Cheats de teste (Shift+clique)", cfg.testingCheatsEnabled)) {
        cfg.testingCheatsEnabled = !cfg.testingCheatsEnabled;
        RunCmd(cfg.testingCheatsEnabled ? "boolProp testingCheatsEnabled true" : "boolProp testingCheatsEnabled false");
    }
    LayNext();

    LaySection(dev, "CAMERA E VISUAL");
    if (LayBtn(dev, 0, 2, "Esconder diamante do Sim", !cfg.plumbBobVisible)) {
        cfg.plumbBobVisible = !cfg.plumbBobVisible;
        ts2::se::TogglePlumbBobVisibility();
    }
    if (LayBtn(dev, 1, 2, "Esconder baloes de pensamento", !cfg.showHeadlinesEnabled)) {
        cfg.showHeadlinesEnabled = !cfg.showHeadlinesEnabled;
        ts2::se::SetShowHeadlinesEnabled(cfg.showHeadlinesEnabled);
    }
    LayNext();

    char next[40];
    _snprintf(next, sizeof(next), ts2::i18n::Tr(cfg.smartAutosaveEnabled ? "proximo em %u s" : "desligado"), ts2::qol::GetSecondsUntilNextAutosave());
    LaySection(dev, "SALVAMENTO", next, kColDim);
    if (LayBtn(dev, 0, 4, "Autosave", cfg.smartAutosaveEnabled)) cfg.smartAutosaveEnabled = !cfg.smartAutosaveEnabled;
    if (LayBtn(dev, 1, 4, "A cada 5 min", cfg.autosaveIntervalMin == 5))   cfg.autosaveIntervalMin = 5;
    if (LayBtn(dev, 2, 4, "A cada 15 min", cfg.autosaveIntervalMin == 15)) cfg.autosaveIntervalMin = 15;
    if (LayBtn(dev, 3, 4, "Salvar agora (F5)")) ts2::qol::TriggerSmartSaveNow();
    LayNext();

    g_lay.y += 6.0f;
    LayText(dev, "Atalhos:  F10 mostrar/ocultar painel   F11 tela cheia   F5 salvar   F9 sincronizar co-op", kColDim, 1.25f);
}

// ============================================================================
// Tab 4: Co-op
// ============================================================================
static void RenderTabCoop(d3d9::IDirect3DDevice9* dev, float x, float y, float w) {
    using namespace ts2::coop;
    const auto& st = GetStats();
    LayBegin(x, y, w);
    char line[220];

    using ts2::i18n::Tr;
    const char* role = (st.role == Role::Host)   ? "HOST" :
                       (st.role == Role::Client) ? Tr("CONVIDADO") : "SOLO";
    _snprintf(line, sizeof(line), "%s - %s", role, Tr(st.peerConnected ? "parceiro conectado" : (st.role == Role::Disconnected ? "nao conectado" : "aguardando parceiro...")));
    LaySection(dev, "CONEXAO", line, st.peerConnected ? kColGood : kColWarn);
    char ipBox[48];
    _snprintf(ipBox, sizeof(ipBox), "%s%s", g_coopIpInput, (g_focusedInputId == 1) ? "_" : "");
    if (LayBtn(dev, 0, 4, ipBox, g_focusedInputId == 1)) g_focusedInputId = 1;
    if (LayBtn(dev, 1, 4, "Hospedar", st.role == Role::Host))   StartHost(27025);
    if (LayBtn(dev, 2, 4, "Conectar", st.role == Role::Client)) JoinHost(g_coopIpInput, 27025);
    if (LayBtn(dev, 3, 4, "Desconectar")) Disconnect();
    LayNext();
    if (st.connectMessage[0]) LayText(dev, st.connectMessage, kColDim, 1.25f);

    LayText(dev, "Ao entrar, o convidado espelha o lote do Host (moveis e Sims). O save do convidado nao importa.", kColDim, 1.25f);

    LaySection(dev, "SINCRONIA");
    if (LayBtn(dev, 0, 4, "Sincronizar (F9)")) ForceResyncNow();
    if (LayBtn(dev, 1, 4, st.objSyncEnabled ? "Moveis: ON" : "Moveis: OFF", st.objSyncEnabled)) SetObjectSyncEnabled(!st.objSyncEnabled);
    if (LayBtn(dev, 2, 4, "Espelhar lote")) RequestObjectSnapshot();
    if (LayBtn(dev, 3, 4, "Trocar meu Sim"))  ts2::se::CycleSelectedLotSim();
    LayNext();
    if (LayBtn(dev, 0, 3, "Pausar (ambos)"))  SendSpeedPauseCommand(ts2::se::GetSimulatorSpeed(), true);
    if (LayBtn(dev, 1, 3, "Retomar (ambos)")) SendSpeedPauseCommand(ts2::se::GetSimulatorSpeed(), false);
    if (LayBtn(dev, 2, 3, st.simSyncEnabled ? "Sims & Acoes: ON" : "Sims & Acoes: OFF", st.simSyncEnabled)) SetSimSyncEnabled(!st.simSyncEnabled);
    LayNext();
    if (LayBtn(dev, 0, 3, st.blockPeerAutonomy ? "IA do convidado: silenciada" : "IA do convidado: livre", st.blockPeerAutonomy)) SetBlockPeerAutonomy(!st.blockPeerAutonomy);
    LayNext();
    _snprintf(line, sizeof(line), Tr("Moveis: %u monit. | movidos %u/%u | criados %u | vendidos %u | conflitos %u%s"),
              st.objTracked, st.objSent, st.objApplied, st.objCreated, st.objDeleted, st.objConflicts,
              st.objSyncAvailable ? "" : Tr(" (abra lote)"));
    LayText(dev, line, kColDim, 1.22f);
    _snprintf(line, sizeof(line), Tr("Sims: meu slot %d (NID %d) | parceiro %d (NID %d) | poses %u/%u | motivos %u"),
              st.mySimIdx, static_cast<int>(st.mySimNeighborId),
              st.peerSimIdx, static_cast<int>(st.peerSimNeighborId),
              st.simsSent, st.simsApplied, st.motivesSynced);
    LayText(dev, line, kColDim, 1.22f);
    _snprintf(line, sizeof(line), Tr("Interacoes Pie Menu sincronizadas: %u (ultima: menu=%d alvo=#%u) | BHAV: %u"),
              st.interactionsSynced, st.lastSyncedInteractionIdx,
              static_cast<unsigned>(st.lastSyncedTargetSlot), st.bhavEventsSynced);
    LayText(dev, line, kColDim, 1.22f);
    _snprintf(line, sizeof(line), Tr("Fundos local $%d / host $%d | Tick peer %u | CRC %08X | divergencias: %u"),
              ts2::se::GetFamilyFunds(), st.hostFunds, st.lastPeerTick, st.lastLocalCrc, st.desyncCount);
    LayText(dev, line, kColDim, 1.22f);

    g_lay.y += 2.0f;
    LayText(dev,
            "Host: clique em Hospedar. Convidado: no MENU PRINCIPAL, clique em Conectar.\n"
            "Sincronia completa: Relogio/SubTick, PRNG, Fundos, Sims (pose + motivos + NeighborID),\n"
            "Fila de Interacoes (Pie Menu / Ir Aqui) e Compra/Venda/Movimento de Moveis!",
            kColDim, 1.18f);
}

// ============================================================================
// Tab 4: Advanced (population, SimAntics watchdog, console, log)
// ============================================================================
static void RenderTabAdvanced(d3d9::IDirect3DDevice9* dev, float x, float y, float w) {
    auto& cfg = ts2::qol::GetConfig();
    LayBegin(x, y, w);

    LaySection(dev, "SIMS");
    if (LayBtn(dev, 0, 3, "Premios de carreira")) ts2::se::UnlockAllCareerRewardsOnLot();
    if (LayBtn(dev, 1, 3, "Forcar gemeos"))       ts2::se::TriggerForceTwinsOnSelectedSim();
    if (LayBtn(dev, 2, 3, "Envelhecimento", cfg.agingEnabled)) {
        cfg.agingEnabled = !cfg.agingEnabled;
        RunCmd(cfg.agingEnabled ? "aging on" : "aging off");
    }
    LayNext();

    char t[64];
    _snprintf(t, sizeof(t), ts2::i18n::Tr("ATUAL: %u"), cfg.maxTotalSims);
    LaySection(dev, "LIMITE DE SIMS NO LOTE", t, kColDim);
    static const uint32_t kLimits[4] = {8, 16, 32, 50};
    for (int i = 0; i < 4; ++i) {
        char lbl[24];
        _snprintf(lbl, sizeof(lbl), ts2::i18n::Tr(i == 0 ? "%u (padrao)" : "%u Sims"), kLimits[i]);
        if (LayBtn(dev, i, 4, lbl, cfg.maxTotalSims == kLimits[i])) {
            cfg.maxTotalSims = kLimits[i];
            ts2::qol::ApplyLotAndBuildQoL(cfg.testingCheatsEnabled, cfg.rotate45DegreeEnabled, cfg.moveObjectsEnabled, kLimits[i]);
        }
    }
    LayNext();

    const uint32_t curIter = ts2::qol::GetMaxTreeIterations();
    _snprintf(t, sizeof(t), ts2::i18n::Tr("ATUAL: %u"), curIter);
    LaySection(dev, "LIMITE DO SIMANTICS (EVITA 'TRAVOU')", t, kColDim);
    static const uint32_t kIters[4] = {10000, 25000, 50000, 100000};
    static const char*    kIterLbl[4] = {"10.000", "25.000", "50.000", "100.000"};
    for (int i = 0; i < 4; ++i) {
        if (LayBtn(dev, i, 4, kIterLbl[i], curIter == kIters[i])) ts2::qol::SetMaxTreeIterations(kIters[i]);
    }
    LayNext();

    LaySection(dev, "CONSOLE");
    char cmdBox[140];
    _snprintf(cmdBox, sizeof(cmdBox), "> %s%s", g_cmdInput, (g_focusedInputId == 2) ? "_" : "");
    if (LayBtn(dev, 0, 4, cmdBox, g_focusedInputId == 2, 3)) g_focusedInputId = 2;
    if (LayBtn(dev, 3, 4, "Executar")) ts2::se::ExecuteCommand(g_cmdInput, g_lastCmdOut, sizeof(g_lastCmdOut));
    LayNext();
    LayText(dev, g_lastCmdOut, kColGood, 1.25f);

    LaySection(dev, "LOG");
    const size_t logCount = ts2::se::GetConsoleLogCount();
    const size_t startIdx = (logCount > 5) ? (logCount - 5) : 0;
    for (size_t i = startIdx; i < logCount; ++i) {
        kit::TextFit(kit::F_SMALL, g_lay.x, g_lay.y + (i - startIdx) * 18.0f, g_lay.w, ts2::se::GetConsoleLogLine(i), kColDim);
    }
}

static void PollOverlayInput(d3d9::IDirect3DDevice9* dev) {
    if (!g_gameHwnd && dev) {
        d3d9::D3DDEVICE_CREATION_PARAMETERS cp = {};
        if (dev->GetCreationParameters(&cp) >= 0 && cp.hFocusWindow) {
            EnsureWndProcHooked(cp.hFocusWindow);
        }
    }

    // Check if our game window is the active foreground window (or in the same process)
    bool isActiveProcess = true;
    HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD fgPid = 0;
        GetWindowThreadProcessId(fg, &fgPid);
        isActiveProcess = (fgPid == GetCurrentProcessId());
    }

    if (isActiveProcess) {
        DWORD now = GetTickCount();
        bool f10Down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        bool insDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if ((f10Down || insDown) && (now - g_lastToggleTick > 220)) {
            g_lastToggleTick = now;
            g_visible = !g_visible;
        }
        bool f5Down = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        if (f5Down && (now - g_lastF5Tick > 350)) {
            g_lastF5Tick = now;
            ts2::qol::TriggerSmartSaveNow();
        }
        bool f9Down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (f9Down && (now - g_lastF9Tick > 350)) {
            g_lastF9Tick = now;
            ts2::coop::ForceResyncNow();
        }
        bool f11Down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (f11Down && (now - g_lastF11Tick > 350)) {
            g_lastF11Tick = now;
            ts2::qol::ToggleBorderlessFullscreen(g_gameHwnd);
        }

        if (g_visible && g_gameHwnd) {
            POINT pt = {};
            if (GetCursorPos(&pt) && ScreenToClient(g_gameHwnd, &pt)) {
                g_mouseX = static_cast<float>(pt.x);
                g_mouseY = static_cast<float>(pt.y);
            }
            bool lmbDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (lmbDown && !g_prevLButtonDown && (now - g_lastClickTick > 140)) {
                g_lastClickTick = now;
                g_mouseClicked  = true;
            }
            g_prevLButtonDown = lmbDown;
            g_mouseDown = lmbDown;
        }
    }
}

static void RenderOverlay(d3d9::IDirect3DDevice9* dev) {
    if (!dev) return;

    // Poll keyboard & mouse fallback (works on both Windows native & Proton/Wine/DXVK)
    PollOverlayInput(dev);

    // Poll network & QoL timers every frame even when overlay is minimized
    ts2::coop::PollNetwork();

    ts2::qol::TickFrame();

    // Capture full D3D9 state block so we never corrupt The Sims 2 or DXVK materials/shaders
    d3d9::IDirect3DStateBlock9* stateBlock = nullptr;
    if (dev->CreateStateBlock(1 /* D3DSBT_ALL */, &stateBlock) >= 0 && stateBlock) {
        stateBlock->Capture();
    }

    // Viewport = whole client area; every other state is set by kit::BeginFrame
    if (g_gameHwnd) {
        RECT rc = {};
        if (GetClientRect(g_gameHwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
            d3d9::D3DVIEWPORT9 vp = {
                0, 0,
                static_cast<DWORD>(rc.right - rc.left),
                static_cast<DWORD>(rc.bottom - rc.top),
                0.0f, 1.0f
            };
            dev->SetViewport(&vp);
        }
    }

    kit::Input in;
    in.mx = g_mouseX; in.my = g_mouseY; in.down = g_mouseDown; in.clicked = g_mouseClicked;
    kit::BeginFrame(dev, in);

    uint16_t hr = 0, mn = 0, sub = 0, day = 0;
    ts2::se::GetSimulatorClock(hr, mn, sub, day);
    const auto& coop = ts2::coop::GetStats();

    auto finish = [&]() {
        kit::EndFrame();
        if (stateBlock) {
            stateBlock->Apply();
            stateBlock->Release();
        }
        g_mouseClicked = false;
    };

    // Compact status badge when the panel is hidden
    if (!g_visible) {
        char badge[96];
        _snprintf(badge, sizeof(badge), "%s   $%d   %02u:%02u   \xE2\x80\xA2   F10", coop.peerConnected ? "CO-OP" : "SOLO",
                  ts2::se::GetFamilyFunds(), hr, mn);
        const float tw = kit::TextWidth(kit::F_BODY, badge);
        const float bw = tw + 52.0f, bh = 30.0f;
        kit::Shadow(12.0f, 14.0f, bw, bh, 15.0f, 14.0f, kit::Col(0, 0, 0, 90));
        kit::RoundRect(12.0f, 12.0f, bw, bh, 15.0f, kit::Col(30, 38, 60, 238), kit::Col(14, 18, 30, 238));
        kit::RoundOutline(12.0f, 12.0f, bw, bh, 15.0f, 1.0f, kit::Col(255, 255, 255, 40));
        kit::Diamond(30.0f, 27.0f, 5.5f, kit::Col(70, 220, 120));
        kit::Text(kit::F_BODY, 44.0f, 12.0f + (bh - kit::FontHeight(kit::F_BODY)) * 0.5f, badge, kit::kText);
        finish();
        return;
    }

    // ---------------------------------------------------------------- main window
    const float wx = kWinX, wy = kWinY, ww = kWinW, wh = kWinH;
    kit::WindowFrame(wx, wy, ww, wh);

    // header: plumbob + title on the left, status pills on the right
    kit::Diamond(wx + 30.0f, wy + 29.0f, 7.5f, kit::Col(70, 220, 120));
    kit::Text(kit::F_TITLE, wx + 50.0f, wy + 29.0f - kit::FontHeight(kit::F_TITLE) * 0.5f, "The Sims 2 Suite", kit::Col(246, 250, 255));
    {
        // language switch (PT | EN), right after the title
        const float lx = wx + 50.0f + kit::TextWidth(kit::F_TITLE, "The Sims 2 Suite") + 14.0f, ly = wy + 18.0f;
        const bool en = ts2::i18n::Get() == ts2::i18n::Lang::EN;
        if (kit::Button(lx, ly, 34.0f, 22.0f, "PT", !en, kit::F_SMALL) && en) ts2::i18n::Set(ts2::i18n::Lang::PT);
        if (kit::Button(lx + 38.0f, ly, 34.0f, 22.0f, "EN", en, kit::F_SMALL) && !en) ts2::i18n::Set(ts2::i18n::Lang::EN);
    }
    {
        char buf[48];
        float px = wx + ww - 18.0f, pw = 0.0f;
        kit::Pill(px, wy + 18.0f, coop.peerConnected ? "CO-OP" : "SOLO", coop.peerConnected ? kColGood : kColWarn, &pw, true);
        px -= pw + 8.0f;
        _snprintf(buf, sizeof(buf), "%02u:%02u", hr, mn);
        kit::Pill(px, wy + 18.0f, buf, kColAccent, &pw, true);
        px -= pw + 8.0f;
        _snprintf(buf, sizeof(buf), "$%d", ts2::se::GetFamilyFunds());
        kit::Pill(px, wy + 18.0f, buf, kit::Col(150, 235, 170), &pw, true);
    }

    // tabs
    static const char* const kTabs[6] = {"Sim", "Construir", "Jogo", "Gráficos", "Co-op", "Avançado"};
    const int clickedTab = kit::TabBar(wx + 16.0f, wy + 56.0f, ww - 32.0f, 34.0f, kTabs, 6, g_activeTab);
    if (clickedTab >= 0 && clickedTab != g_activeTab) {
        g_activeTab = clickedTab;
        g_focusedInputId = 0;
        g_mouseClicked = false; // do not also click whatever sits under the cursor in the new tab
    }

    const float cx = wx + 22.0f, cy = wy + 104.0f, cw = ww - 44.0f;
    switch (g_activeTab) {
        case 0:  RenderTabSim(dev, cx, cy, cw);      break;
        case 1:  RenderTabBuild(dev, cx, cy, cw);    break;
        case 2:  RenderTabGame(dev, cx, cy, cw);     break;
        case 3:  RenderGraphicsTab(cx, cy, cw);      break;
        case 4:  RenderTabCoop(dev, cx, cy, cw);     break;
        default: RenderTabAdvanced(dev, cx, cy, cw); break;
    }

    // footer
    kit::Rect(wx + 20.0f, wy + wh - 32.0f, ww - 40.0f, 1.0f, kit::Col(255, 255, 255, 22));
    kit::Text(kit::F_SMALL, wx + 22.0f, wy + wh - 26.0f,
              "F10 ocultar    •    F11 tela cheia    •    F5 salvar    •    F9 sincronizar co-op", kit::kDim);

    finish();
}


// ============================================================================
// Direct3D 9 VTable Hooks + nGZGraphic4::cDeviceDX Hooks (Windows + Proton/DXVK)
// ============================================================================
using Fn_Direct3DCreate9 = d3d9::IDirect3D9* (WINAPI*)(UINT sdkVersion);
using Fn_CreateDevice    = HRESULT (STDMETHODCALLTYPE*)(
    d3d9::IDirect3D9*, UINT, DWORD, HWND, DWORD, d3d9::D3DPRESENT_PARAMETERS*, d3d9::IDirect3DDevice9**);
using Fn_EndScene        = HRESULT (STDMETHODCALLTYPE*)(d3d9::IDirect3DDevice9*);
using Fn_Reset           = HRESULT (STDMETHODCALLTYPE*)(d3d9::IDirect3DDevice9*, d3d9::D3DPRESENT_PARAMETERS*);
using Fn_cDeviceDX_EndScene      = uint8_t (__thiscall*)(void* thisPtr);
using Fn_cDeviceDX_OnTickPresent = uint8_t (__thiscall*)(void* thisPtr, void* msgPtr);

static Fn_Direct3DCreate9         g_origDirect3DCreate9      = nullptr;
static Fn_CreateDevice            g_origCreateDevice         = nullptr;
static Fn_EndScene                g_origEndScene             = nullptr;
static Fn_Reset                   g_origReset                = nullptr;
static Fn_cDeviceDX_EndScene      g_origCDeviceDXEndScene    = nullptr;
static Fn_cDeviceDX_OnTickPresent g_origCDeviceDXOnTick      = nullptr;
static ts2::hook::Detour          g_hookCDeviceDXOnTick      = {};

static bool g_inMainPresentTick = false;

// Hook on nGZGraphic4::cDeviceDX::EndScene (vtable 0xA0F4D518 slot 30 = 0xA0169920)
// Captures the live IDirect3DDevice9* at [thisPtr + 0x38] even when ijl15.dll was loaded
// dynamically AFTER Direct3DCreate9 / CreateDevice already executed!
static uint8_t __fastcall Hooked_cDeviceDX_EndScene(void* thisPtr, void* /*edx*/) {
    if (thisPtr && g_inMainPresentTick) {
        uintptr_t base = reinterpret_cast<uintptr_t>(thisPtr);
        int32_t sceneDepth = *reinterpret_cast<int32_t*>(base + ts2::addr::kOffset_cDeviceDX_SceneDepth);
        auto* dev = *reinterpret_cast<d3d9::IDirect3DDevice9**>(base + ts2::addr::kOffset_cDeviceDX_D3DDevice);
        // Only draw when sceneDepth == 1 AND we are inside the main screen OnTickPresent pass
        // (prevents drawing the HUD into offscreen Sim Portrait / Thumbnail render targets!)
        if (dev) ts2::fx::AttachDevice(dev);
        if (sceneDepth == 1 && dev) {
            static bool s_loggedDevice = false;
            if (!s_loggedDevice) {
                s_loggedDevice = true;
                ts2::hook::Log("[DX9Overlay] Captured live IDirect3DDevice9* = 0x%08X via cDeviceDX::EndScene (this=0x%08X)",
                               reinterpret_cast<unsigned>(dev), static_cast<unsigned>(base));
            }
            RenderOverlay(dev);
            g_lastRenderedFrame = ++g_frameCounter;
        }
    }
    return g_origCDeviceDXEndScene ? g_origCDeviceDXEndScene(thisPtr) : 1;
}

// Hook on nGZGraphic4::cDeviceDX::OnTickMessage (0xA0169C70) right around the main frame render + Present()
static uint8_t __fastcall Hooked_cDeviceDX_OnTickPresent(void* thisPtr, void* /*edx*/, void* msgPtr) {
    bool isRenderTick = false;
    if (thisPtr && msgPtr) {
        uint32_t msgId = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(msgPtr) + 0x04);
        if (msgId == 3) {
            isRenderTick = true;
            g_inMainPresentTick = true;
        }
    }
    uint8_t res = g_origCDeviceDXOnTick ? g_origCDeviceDXOnTick(thisPtr, msgPtr) : 0;
    if (isRenderTick) {
        g_inMainPresentTick = false;
    }
    return res;
}

static HRESULT STDMETHODCALLTYPE HookedEndScene(d3d9::IDirect3DDevice9* pDevice) {
    if (pDevice && g_lastRenderedFrame != g_frameCounter) {
        RenderOverlay(pDevice);
        g_lastRenderedFrame = ++g_frameCounter;
    }
    return g_origEndScene ? g_origEndScene(pDevice) : 0;
}

static HRESULT STDMETHODCALLTYPE HookedReset(d3d9::IDirect3DDevice9* pDevice, d3d9::D3DPRESENT_PARAMETERS* pPresentationParameters) {
    return g_origReset ? g_origReset(pDevice, pPresentationParameters) : 0;
}

static HRESULT STDMETHODCALLTYPE HookedCreateDevice(
    d3d9::IDirect3D9* pD3D,
    UINT Adapter,
    DWORD DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    d3d9::D3DPRESENT_PARAMETERS* pPresentationParameters,
    d3d9::IDirect3DDevice9** ppReturnedDeviceInterface)
{
    HRESULT hr = g_origCreateDevice(pD3D, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                    pPresentationParameters, ppReturnedDeviceInterface);
    if (hr >= 0 && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        d3d9::IDirect3DDevice9* dev = *ppReturnedDeviceInterface;
        HWND targetWnd = (pPresentationParameters && pPresentationParameters->hDeviceWindow)
                         ? pPresentationParameters->hDeviceWindow
                         : hFocusWindow;
        EnsureWndProcHooked(targetWnd);
        ts2::fx::AttachDevice(dev);

        uintptr_t vtableAddr = reinterpret_cast<uintptr_t>(dev->vtable);
        ts2::hook::HookPointerSlot(vtableAddr + 16 * sizeof(void*),
                                   reinterpret_cast<void*>(&HookedReset),
                                   reinterpret_cast<void**>(&g_origReset));
        ts2::hook::HookPointerSlot(vtableAddr + 42 * sizeof(void*),
                                   reinterpret_cast<void*>(&HookedEndScene),
                                   reinterpret_cast<void**>(&g_origEndScene));
        ts2::hook::Log("[DX9Overlay] Hooked IDirect3DDevice9 (0x%08X): Reset=0x%08X, EndScene=0x%08X",
                       reinterpret_cast<unsigned>(dev),
                       reinterpret_cast<unsigned>(g_origReset),
                       reinterpret_cast<unsigned>(g_origEndScene));
    }
    return hr;
}

static d3d9::IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion) {
    d3d9::IDirect3D9* pD3D = g_origDirect3DCreate9 ? g_origDirect3DCreate9(sdkVersion) : nullptr;
    if (pD3D && pD3D->vtable) {
        uintptr_t vtableAddr = reinterpret_cast<uintptr_t>(pD3D->vtable);
        ts2::hook::HookPointerSlot(vtableAddr + 16 * sizeof(void*),
                                   reinterpret_cast<void*>(&HookedCreateDevice),
                                   reinterpret_cast<void**>(&g_origCreateDevice));
        ts2::hook::Log("[DX9Overlay] Hooked IDirect3D9::CreateDevice (SDKVersion=%u, pD3D=0x%08X)",
                       sdkVersion, reinterpret_cast<unsigned>(pD3D));
    }
    return pD3D;
}

} // namespace

bool InitializeDx9Overlay() {
    // 1. Hook IAT slot 0xA0F4974C (Direct3DCreate9) in case D3D9 isn't created yet
    uintptr_t iatSlot = ts2::addr::Rebase(ts2::addr::kIAT_Direct3DCreate9);
    bool okIat = ts2::hook::HookPointerSlot(
        iatSlot,
        reinterpret_cast<void*>(&HookedDirect3DCreate9),
        reinterpret_cast<void**>(&g_origDirect3DCreate9)
    );

    // 2. Hook nGZGraphic4::cDeviceDX vtable slot 30 (EndScene @ 0xA0169920)
    // This works immediately even when Sims2EP9.exe loads ijl15.dll AFTER D3D9 device creation!
    uintptr_t cDeviceDxEndSceneSlot = ts2::addr::Rebase(ts2::addr::kVTable_cDeviceDX + 30 * sizeof(void*));
    bool okVtbl = ts2::hook::HookPointerSlot(
        cDeviceDxEndSceneSlot,
        reinterpret_cast<void*>(&Hooked_cDeviceDX_EndScene),
        reinterpret_cast<void**>(&g_origCDeviceDXEndScene)
    );

    // 3. Hook nGZGraphic4::cDeviceDX::OnTickMessage (0xA0169C70, 5-byte prologue: 55 8B EC 6A FF)
    uintptr_t onTickAddr = ts2::addr::Rebase(ts2::addr::kFunc_cDeviceDX_OnTickPresent);
    bool okTick = ts2::hook::InstallDetour(
        g_hookCDeviceDXOnTick,
        onTickAddr,
        reinterpret_cast<void*>(&Hooked_cDeviceDX_OnTickPresent),
        5
    );
    g_origCDeviceDXOnTick = g_hookCDeviceDXOnTick.Original<Fn_cDeviceDX_OnTickPresent>();

    ts2::hook::Log("[DX9Overlay] Installed hooks: IAT_D3D9=%d, cDeviceDX::EndScene(vt30)=%d (orig=0x%08X), cDeviceDX::OnTick=%d",
                   okIat ? 1 : 0,
                   okVtbl ? 1 : 0,
                   reinterpret_cast<unsigned>(g_origCDeviceDXEndScene),
                   okTick ? 1 : 0);
    return okIat && okVtbl;
}

void ShutdownDx9Overlay() {
    if (g_gameHwnd && g_origWndProc) {
        SetWindowLongA(g_gameHwnd, GWL_WNDPROC, reinterpret_cast<LONG>(g_origWndProc));
        g_origWndProc = nullptr;
    }
}

bool IsOverlayVisible() {
    return g_visible;
}

void SetOverlayVisible(bool visible) {
    g_visible = visible;
}

} // namespace ts2::ui
