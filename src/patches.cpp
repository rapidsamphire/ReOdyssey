// Lost Odyssey patch hooks ported from Xenia Canary.

#include "generated/reodyssey_init.h"

#include <cstdint>

#include <rex/cvar.h>

namespace {

constexpr int32_t kAspectModeOff = 0;
constexpr int32_t kAspectMode21x9 = 1;
constexpr int32_t kAspectMode16x10 = 2;

constexpr float kAspectRatio21x9 = 2.3333f;
constexpr float kAspectRatio16x10 = 1.6f;
constexpr float kAspectFovScale21x9 = 0.01163553f;
constexpr float kAspectFovScale16x10 = 0.00784999f;
constexpr uint32_t kUiViewportHeight21x9 = 0x226;
constexpr uint32_t kColorBufferWidth16x10 = 0x480;

thread_local uint32_t g_partial_debug_menu_saved_r28 = 0;
thread_local bool g_partial_debug_menu_restore_pending = false;

}  // namespace

REXCVAR_DECLARE(int32_t, lo_patch_aspect_ratio_mode);

REXCVAR_DEFINE_BOOL(lo_patch_60_fps, false, "Lost Odyssey/Patches",
                    "Set the presentation interval selector to 60 FPS.");
REXCVAR_DEFINE_BOOL(lo_patch_flickering_characters_fix, false, "Lost Odyssey/Patches",
                    "Use the safer CPU-skinning path to avoid flickering characters.");
REXCVAR_DEFINE_BOOL(lo_patch_disable_occlusion_queries, false, "Lost Odyssey/Patches",
                    "Disable guest occlusion-query checks for stability.");
REXCVAR_DEFINE_BOOL(lo_patch_post_processing_upscaling_fix, false, "Lost Odyssey/Patches",
                    "Zero problematic post-processing parameters for upscaling.");
REXCVAR_DEFINE_BOOL(lo_patch_disable_depth_of_field, false, "Lost Odyssey/Patches",
                    "Disable depth of field.");
REXCVAR_DEFINE_BOOL(lo_patch_disable_motion_blur, false, "Lost Odyssey/Patches",
                    "Disable motion blur.");
REXCVAR_DEFINE_BOOL(lo_patch_anisotropic_filtering_16x, false, "Lost Odyssey/Patches",
                    "Force the guest anisotropic filtering level to 16x.");
REXCVAR_DEFINE_BOOL(lo_patch_disable_dynamic_shadows, false, "Lost Odyssey/Patches",
                    "Disable dynamic shadow rendering paths.");
REXCVAR_DEFINE_BOOL(lo_patch_partial_debug_menu, false, "Lost Odyssey/Patches",
                    "Enable the partial debug menu (LT + RT).");
REXCVAR_DEFINE_INT32(
    lo_patch_aspect_ratio_mode, kAspectModeOff, "Lost Odyssey/Patches",
    "Aspect ratio mode: 0=off, 1=21:9 ultrawide, 2=16:10 Steam Deck.")
    .range(kAspectModeOff, kAspectMode16x10)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

inline bool IsAspectModeEnabled() {
  return REXCVAR_GET(lo_patch_aspect_ratio_mode) != kAspectModeOff;
}

inline bool IsAspectMode21x9() {
  return REXCVAR_GET(lo_patch_aspect_ratio_mode) == kAspectMode21x9;
}

inline bool IsAspectMode16x10() {
  return REXCVAR_GET(lo_patch_aspect_ratio_mode) == kAspectMode16x10;
}

inline void SetFloatRegister(PPCRegister& reg, float value) {
  reg.f64 = static_cast<double>(value);
}

inline float GetAspectRatioValue() {
  return IsAspectMode21x9() ? kAspectRatio21x9 : kAspectRatio16x10;
}

inline float GetAspectFovScaleValue() {
  return IsAspectMode21x9() ? kAspectFovScale21x9 : kAspectFovScale16x10;
}

}  // namespace

void LoPatch60FpsHook(PPCRegister& r10) {
  if (REXCVAR_GET(lo_patch_60_fps)) {
    r10.u64 = 1;
  }
}

bool LoPatchFlickeringCharactersFixHook(PPCRegister& r3) {
  if (!REXCVAR_GET(lo_patch_flickering_characters_fix)) {
    return false;
  }
  r3.u64 = 1;
  return true;
}

void LoPatchDisableOcclusionQueriesRenderHook(PPCRegister& r11) {
  if (REXCVAR_GET(lo_patch_disable_occlusion_queries)) {
    r11.u64 = 1;
  }
}

void LoPatchDisableOcclusionQueriesInitViewsHook(PPCRegister& r10) {
  if (REXCVAR_GET(lo_patch_disable_occlusion_queries)) {
    r10.u64 = 1;
  }
}

void LoPatchPostProcessingUpscalingFix0Hook(PPCRegister& f0) {
  if (REXCVAR_GET(lo_patch_post_processing_upscaling_fix)) {
    SetFloatRegister(f0, 0.0f);
  }
}

void LoPatchPostProcessingUpscalingFix1Hook(PPCRegister& f1) {
  if (REXCVAR_GET(lo_patch_post_processing_upscaling_fix)) {
    SetFloatRegister(f1, 0.0f);
  }
}

bool LoPatchDisableDepthOfFieldHook() {
  return REXCVAR_GET(lo_patch_disable_depth_of_field);
}

bool LoPatchDisableMotionBlurHook() {
  return REXCVAR_GET(lo_patch_disable_motion_blur);
}

void LoPatchAnisotropicFiltering16xHook(PPCRegister& r5) {
  if (REXCVAR_GET(lo_patch_anisotropic_filtering_16x)) {
    r5.u64 = 16;
  }
}

bool LoPatchDisableDynamicShadowsRenderLightsHook() {
  return REXCVAR_GET(lo_patch_disable_dynamic_shadows);
}

bool LoPatchDisableDynamicShadowsProjectedSortHook() {
  return REXCVAR_GET(lo_patch_disable_dynamic_shadows);
}

bool LoPatchDisableDynamicShadowsModulatedHook() {
  return REXCVAR_GET(lo_patch_disable_dynamic_shadows);
}

void LoPatchPartialDebugMenuStoreHook(PPCRegister& r28, PPCRegister& r30) {
  if (!REXCVAR_GET(lo_patch_partial_debug_menu)) {
    g_partial_debug_menu_restore_pending = false;
    return;
  }
  g_partial_debug_menu_saved_r28 = r28.u32;
  g_partial_debug_menu_restore_pending = true;
  r28.u64 = r30.u64;
}

void LoPatchPartialDebugMenuRestoreHook(PPCRegister& r28) {
  if (!g_partial_debug_menu_restore_pending) {
    return;
  }
  r28.u64 = g_partial_debug_menu_saved_r28;
  g_partial_debug_menu_restore_pending = false;
}

void LoPatchPartialDebugMenuFlagHook(PPCRegister& r11) {
  if (REXCVAR_GET(lo_patch_partial_debug_menu)) {
    r11.u64 = 0;
  }
}

void LoPatchAspectRatioConstantHook(PPCRegister& f0) {
  if (IsAspectModeEnabled()) {
    SetFloatRegister(f0, GetAspectRatioValue());
  }
}

void LoPatchAspectRatioColorBufferWidthHook(PPCRegister& r30) {
  if (IsAspectMode16x10()) {
    r30.u64 = kColorBufferWidth16x10;
  }
}

bool LoPatchAspectRatioUiViewportHeightHook(PPCRegister& r3) {
  if (!IsAspectMode21x9()) {
    return false;
  }
  r3.u64 = kUiViewportHeight21x9;
  return true;
}

bool LoPatchAspectRatioSkipConstrainHook() {
  return IsAspectModeEnabled();
}

void LoPatchAspectRatioFovScaleHook(PPCRegister& f0) {
  if (IsAspectModeEnabled()) {
    SetFloatRegister(f0, GetAspectFovScaleValue());
  }
}
