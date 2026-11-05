#include "ts2/ui_tabs.hpp"
#include "ts2/dx9_postfx.hpp"
#include "ts2/d3d9_ext.hpp"
#include "ts2/i18n.hpp"

namespace ts2::ui {

void RenderGraphicsTab(float x, float y, float w) {
    using namespace kit;
    auto& S = ts2::fx::GetSettings();
    const auto& st = ts2::fx::GetStatus();
    static int activePreset = -1;
    static int page = 0;
    bool changed = false;

    LayBegin(x, y, w);

    static const char* kPages[4] = {"Efeitos", "Cor", "Foco (DoF)", "Sombras"};
    for (int i = 0; i < 4; ++i) {
        if (LayBtn(i, 4, kPages[i], page == i)) page = i;
    }
    LayNext();

    if (page == 0) {
        char statusText[96];
        if (strncmp(st.message, "ativo (", 7) == 0) {
            char* mid = nullptr;
            const long sw = strtol(st.message + 7, &mid, 10);
            const long sh = (mid && *mid == 'x') ? strtol(mid + 1, nullptr, 10) : 0;
            _snprintf(statusText, sizeof(statusText), ts2::i18n::Tr("ativo (%ux%u)"), static_cast<unsigned>(sw), static_cast<unsigned>(sh));
        } else {
            _snprintf(statusText, sizeof(statusText), "%s", ts2::i18n::Tr(st.message));
        }
        LaySection("PRESETS", statusText, st.pipelineOk ? kGood : kWarn);
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
        changed |= FxRow("Névoa",          &S.fog,            &S.fogDensity,      0.0f, 0.02f,  0.0040f, 4);
        changed |= FxRow("Raios de luz",   &S.shafts,         &S.shaftStrength,   0.0f, 1.5f,   0.45f);
        changed |= FxRow("Reflexos (SSR)", &S.ssr,            &S.ssrStrength,     0.0f, 1.5f,   0.60f);
        changed |= FxRow("Anti-aliasing",  &S.fxaa,           nullptr,            0.0f, 0.0f,   0.0f);
        changed |= ValRow("Vinheta",       &S.vignette,       0.0f, 0.6f, 0.18f);
        changed |= ValRow("Saturação",     &S.saturation,     0.5f, 1.6f, 1.08f);
        changed |= ValRow("Contraste",     &S.contrast,       0.7f, 1.5f, 1.06f);
        if (st.lateZ > 0) {
            char dbg[120];
            _snprintf(dbg, sizeof(dbg), ts2::i18n::Tr("Diag: %u draws 3D depois do pos-processo (%u escrevem profundidade)"),
                      static_cast<unsigned>(st.lateZ), static_cast<unsigned>(st.lateZWrite));
            LayText(dbg, kWarn, F_SMALL);
        }
    } else if (page == 1) {
        LaySection("COR", "0 = neutro", kDim);
        changed |= FxRow("Tone mapping",   &S.tonemap,        &S.tonemapStrength, 0.0f, 1.0f,   0.60f);
        changed |= FxRow("Cor por hora",   &S.timeGrade,      &S.gradeStrength,   0.0f, 1.0f,   0.60f);
        changed |= FxRow("Bloom",          &S.bloom,          &S.bloomStrength,   0.0f, 1.0f,   0.32f);
        changed |= ValRow("Temperatura (frio/quente)", &S.gradeTemp,     -1.0f, 1.0f, 0.0f);
        changed |= ValRow("Matiz (verde/magenta)",     &S.gradeTint,     -1.0f, 1.0f, 0.0f);
        changed |= ValRow("Pretos (lift)",             &S.gradeLift,     -1.0f, 1.0f, 0.0f);
        changed |= ValRow("Brilhos (gain)",            &S.gradeGain,     -1.0f, 1.0f, 0.0f);
        changed |= ValRow("Tom dividido (frio/quente)", &S.gradeSplit,    0.0f, 1.0f, 0.0f);
        changed |= ValRow("Vibração",                  &S.gradeVibrance,  0.0f, 1.0f, 0.0f);
    } else if (page == 2) {
        LaySection("PROFUNDIDADE DE CAMPO", "desfoca o que está fora de foco", kDim);
        changed |= FxRow("Profundidade de campo", &S.dof,     &S.dofAperture,  0.0f, 1.0f, 0.45f);
        changed |= FxRow("Foco automático (centro)", &S.dofAuto, &S.dofFocus,  2.0f, 80.0f, 18.0f, 1);
        changed |= FxRow("Tilt-shift (miniatura)",   &S.dofTilt, &S.dofTiltBand, 0.04f, 0.45f, 0.20f);
        changed |= ValRow("Raio máximo (px a 1080p)", &S.dofMaxRadius, 2.0f, 30.0f, 12.0f, 1);
        changed |= ValRow("Zona nítida",              &S.dofDeadZone,  0.0f, 0.4f, 0.10f);
        LayText("Foco automático: a distância mira o objeto no centro da tela. Desligado, o controle ao lado da chave é a distância fixa.", kDim, F_SMALL);
        LayText("Tilt-shift: ignora a profundidade e deixa nítida só uma faixa horizontal (o controle é a altura da faixa).", kDim, F_SMALL);
    } else {
        LaySection("SOMBRAS", "calculadas na resolução da tela", kDim);
        changed |= FxRow("Sombras do sol", &S.contactShadows, &S.shadowStrength, 0.0f, 1.0f, 0.40f);
        changed |= ValRow("Suavidade (penumbra)", &S.shadowSoft,   0.0f, 1.0f, 0.55f);
        changed |= ValRow("Alcance",              &S.shadowLength, 0.5f, 5.0f, 1.6f);
        changed |= FxRow("Sombras longas com sol baixo", &S.shadowAdaptive, nullptr, 0.0f, 0.0f, 0.0f);

        LayText("O jogo desenha as próprias sombras como geometria projetada (não usa mapa de sombras), então não há textura de sombra para aumentar.", kDim, F_SMALL);
        LayText("As sombras do sol acima são calculadas na resolução da tela. Suavidade controla a largura da penumbra; Alcance, o comprimento.", kDim, F_SMALL);
    }

    if (changed) {
        activePreset = -1;
        ts2::fx::NotifyChanged();
    }
}

} // namespace ts2::ui
