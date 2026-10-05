#pragma once

namespace cw::d3d {

// Live-tunable settings of the Direct3D 11 renderer's frame effects (Device11.cpp). F10 (or CW_FX_TUNER=1) opens a
// window with a slider per setting; changes apply at once, Save writes them to fx_tuning.ini beside the game, which
// is read at startup.
enum FxSetting {
    FxExposureDay,
    FxExposureNight,
    FxContrast,
    FxSaturation,
    FxBloomDay,
    FxBloomNight,
    FxBloomThresholdDay,
    FxBloomThresholdNight,
    FxVignetteDay,
    FxVignetteNight,
    FxShadowDarknessDay,
    FxShadowDarknessNight,
    FxShadowNear,
    FxShadowFar,
    FxOcclusionStrength,
    FxOcclusionRadius,
    FxLightStrengthDay,
    FxLightStrengthNight,
    FxLightReach,
    FxLightBrightness,
    FxEffectGlowDay,
    FxEffectGlowNight,
    FxPlayerGlow,
    FxBounce,
    FxReflections,
    FxReflectionReach,
    FxSettingCount
};

// The current value of a setting.
float fx(FxSetting setting);

// Opens the tuning window, or closes it if it is open.
void toggleFxTuner();

// Opens it at startup when CW_FX_TUNER=1.
void startFxTunerIfRequested();

}  // namespace cw::d3d
