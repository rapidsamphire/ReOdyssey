
// reodyssey - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "input/reodyssey_mnk_input.h"
#include "render/video.h"

#include <rex/rex_app.h>

class ReodysseyApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<ReodysseyApp>(new ReodysseyApp(ctx, "reodyssey",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.input_factory = reodyssey::CreateInputSystem;
    // Disable the rex emulated GPU; the native Plume renderer owns presentation.
    // rex sets config.graphics just before this hook, so resetting here wins.
    config.graphics.reset();
  }

  // Window exists by now (created in SetupPresentation); stand up the native
  // renderer on its HWND before the guest starts issuing D3D calls.
  void OnPreLaunchModule() override {
    if (auto* w = window()) {
      Video::Init(w->GetNativeWindowHandle(), 1280, 720);
    }
  }

  void OnShutdown() override { Video::Shutdown(); }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (paths.game_data_root.empty()) {
      paths.game_data_root = paths.config_path.parent_path() / "assets";
    }
  }
};
