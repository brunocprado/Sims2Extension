#include "ts2/ui_tabs.hpp"
#include "ts2/dx9_postfx.hpp"
#include "ts2/d3d9_ext.hpp"

namespace ts2::ui {

void RenderGraphicsTab(float x, float y, float w) {
    using namespace kit;
    auto& S = ts2::fx::GetSettings();
    const auto& st = ts2::fx::GetStatus();
    static int activePreset = -1;
    bool changed = false;

    LayBegin(x, y, w);

    LaySection("PRESETS", st.message, st.pipelineOk ? kGood : kWarn);
    static const char* kPresets[4] = {"Original", "Suave", "Equilibrado", "Cinematográfico"};
    for (int i = 0; i < 4; ++i) {
        if (LayBtn(i, 5, kPresets[i], activePreset == i)) { ts2::fx::ApplyPreset(i); activePreset = i; }
    }
    if (LayBtn(4, 5, S.enabled ? "Efeitos ligados" : "Efeitos desligados", S.enabled)) {
        S.enabled = !S.enabled;
        activePreset = -1;
        ts2::fx::NotifyChanged();
    }
    LayNext();

    LaySection("EFEITOS", "salvo automaticamente em ts2fx.cfg", kDim);
    changed |= FxRow("Oclusão (AO)",   &S.ao,             &S.aoIntensity,     0.0f, 2.0f,   0.85f);
    changed |= FxRow("Sombras do sol", &S.contactShadows, &S.shadowStrength,  0.0f, 1.0f,   0.40f);
    changed |= FxRow("Névoa",          &S.fog,            &S.fogDensity,      0.0f, 0.02f,  0.0040f, 4);
    changed |= FxRow("Raios de luz",   &S.shafts,         &S.shaftStrength,   0.0f, 1.5f,   0.45f);
    changed |= FxRow("Reflexos (SSR)", &S.ssr,            &S.ssrStrength,     0.0f, 1.5f,   0.60f);
    changed |= FxRow("Tone mapping",   &S.tonemap,        &S.tonemapStrength, 0.0f, 1.0f,   0.60f);
    changed |= FxRow("Cor por hora",   &S.timeGrade,      &S.gradeStrength,   0.0f, 1.0f,   0.60f);
    changed |= FxRow("Bloom",          &S.bloom,          &S.bloomStrength,   0.0f, 1.0f,   0.32f);
    changed |= FxRow("Anti-aliasing",  &S.fxaa,           nullptr,            0.0f, 0.0f,   0.0f);

    g_lay.y += 2.0f;
    LayText("Dica: se o sol ou os raios parecerem vir do lado errado, use  fx.set sunyaw 90  (ou 180, 270) no console.", kDim, F_SMALL);
    if (st.lateZ > 0) {
        char dbg[120];
        _snprintf(dbg, sizeof(dbg), "Diag: %u draws 3D depois do pos-processo (%u escrevem profundidade)",
                  static_cast<unsigned>(st.lateZ), static_cast<unsigned>(st.lateZWrite));
        LayText(dbg, kWarn, F_SMALL);
    }

    if (changed) {
        activePreset = -1;
        ts2::fx::NotifyChanged();
    }
}

} // namespace ts2::ui
