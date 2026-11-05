#pragma once
#include "ts2/win32_abi.hpp"
#include "ts2/d3d9_ext.hpp"

namespace ts2::fx {

// ============================================================================
// Modern lighting / post-processing layer for The Sims 2 (Direct3D 9).
//
//   * Hooks the live IDirect3DDevice9 vtable (works with native D3D9, Wine wined3d and DXVK).
//   * Swaps the game's depth buffer for a sampleable INTZ texture, so the scene depth is available.
//   * After the 3D scene and before the HUD is drawn it runs a screen-space pipeline:
//       SSAO (horizon based) -> bilateral blur -> contact shadows + sun shadow -> composite
//       -> atmosphere (time-of-day fog + screen-space reflections) -> FXAA -> light shafts, bloom
//       -> final grade (filmic tone mapping, time-of-day tint, vignette, saturation, contrast)
//   * Settings are live: overlay tab "Graficos", console commands `fx.*`, and ts2fx.cfg (hot reloaded).
// ============================================================================

struct Settings {
    bool  enabled          = true;

    // Ambient occlusion
    bool  ao               = true;
    float aoRadius         = 1.10f;   // world units (tiles ~ 1.0)
    float aoIntensity      = 0.85f;
    float aoPower          = 1.0f;
    bool  aoBlur           = true;

    // Shadows
    bool  contactShadows   = true;
    float shadowStrength   = 0.40f;
    float shadowLength     = 1.6f;    // world units the screen-space ray travels towards the sun
    bool  sunAuto          = true;    // derive the sun from the in-game clock
    float sunAzimuth       = 215.0f;  // degrees (used when sunAuto == false)
    float sunElevation     = 52.0f;   // degrees

    float sunYaw           = 0.0f;    // degrees added to the sun azimuth (compass calibration)

    // Atmosphere
    bool  fog              = true;    // distance fog tinted by the time of day
    float fogDensity       = 0.0040f; // per world unit beyond fogStart
    float fogStart         = 30.0f;
    float fogMax           = 0.45f;   // opacity cap
    float fogScatter       = 0.45f;   // brighter fog when looking towards the sun
    bool  shafts           = true;    // screen-space light shafts (god rays)
    float shaftStrength    = 0.45f;
    float shaftThreshold   = 0.72f;

    // Screen-space reflections on smooth, upward facing surfaces
    bool  ssr              = true;
    float ssrStrength      = 0.60f;
    float ssrF0            = 0.25f;   // reflectivity at normal incidence (Fresnel base)
    float ssrDistance      = 6.0f;    // world units the reflection ray travels
    float ssrThickness     = 0.45f;

    // Tone mapping / grade
    bool  tonemap          = true;    // filmic (ACES) curve, mid grey preserving
    float tonemapStrength  = 0.60f;
    float tonemapExposure  = 0.50f;
    bool  timeGrade        = true;    // tint by in-game time of day
    float gradeStrength    = 0.60f;

    // Image
    bool  fxaa             = true;
    bool  bloom            = true;
    float bloomStrength    = 0.32f;
    float bloomThreshold   = 0.72f;
    float vignette         = 0.18f;
    float saturation       = 1.08f;
    float contrast         = 1.06f;
    float exposure         = 1.00f;

    // Diagnostics
    int   debugView        = 0;       // 0 = final, 1 = AO only, 2 = depth, 3 = shadow only
    bool  probe            = false;   // log one frame of D3D9 calls to ts2fx_probe_<pid>.log
};

Settings& GetSettings();

struct Status {
    bool     hooked;
    bool     depthOk;        // INTZ depth swapped in
    bool     pipelineOk;     // shaders + render targets ready
    bool     sceneDetected;  // a 3D scene with a perspective projection was seen this frame
    uint32_t framesProcessed;
    uint32_t lateZ;          // Z-tested draws issued after the post-fx ran (last processed frame)
    uint32_t lateZWrite;     // ...of which wrote depth
    char     message[160];
};
const Status& GetStatus();

// Called once the live device is known (idempotent).
void AttachDevice(d3d9::IDirect3DDevice9* dev);

// Frame brackets around the game's main render tick (cDeviceDX::OnTickMessage id 3).
void OnRenderTickBegin(d3d9::IDirect3DDevice9* dev);
void OnRenderTickEnd(d3d9::IDirect3DDevice9* dev);

// Handles `fx.*` console commands. Returns true when the command was recognised.
bool ExecuteCommand(const char* cmd, char* out, size_t outLen);

// Writes a settings file / reloads it (hot reload also runs automatically).
void LoadConfigFile();
void SaveConfigFile();

// UI helpers. Presets: 0 = original look (all effects off), 1 = soft, 2 = balanced (defaults), 3 = cinematic.
void ApplyPreset(int id);
// Call after changing Settings from the UI: the file is saved a moment later (debounced).
void NotifyChanged();

} // namespace ts2::fx
