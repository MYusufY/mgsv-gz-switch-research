// Metal Gear Solid V: Ground Zeroes: the ReXGlue application object (paths, GPU plugin choice, Switch settings).
// Modelled on sc5p2-nx's app_sc5p2/src/sc5p2_app.h. The emulated Xenos GPU is the default; with
// sa2_renderer_native = true the native renderer (src/me) replaces it.

#pragma once

#include <filesystem>
#include <memory>
#include <string_view>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#if defined(MGSV_BUILTIN_CONFIG)
#include "mgsv_builtin_config.h"  // generated from nx/mgsv_switch_defaults.toml and nx/mgsv_switch.toml (CMake)
#endif

#include "cpu/sa2_enhancements.h"
#include "me/me_native_system.h"
#include "me/sa2_render_mode.h"
#include "mgsv_frame_dump.h"
#include "sc5p2_capture.h"
#include "sc5p2_test_input.h"

class MgsvApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MgsvApp>(new MgsvApp(ctx, "mgsv", PPCImageConfig));
  }

  // The Switch build's fixed settings, applied before the SD card's mgsv.toml (which may override any of them).
  std::string_view BuiltInConfig() const override {
#if defined(MGSV_BUILTIN_CONFIG)
    return kMgsvBuiltInConfig;
#else
    return {};
#endif
  }
  // Without an mgsv.toml next to the NRO, the player's default one is written there.
  std::string_view DefaultConfigFile() const override {
#if defined(MGSV_BUILTIN_CONFIG)
    return kMgsvPlayerConfig;
#else
    return {};
#endif
  }

  void OnPostInitLogging() override {
    // The runtime defaults to no GPU plugin: use ReXGlue's emulated Xenos GPU unless one was chosen explicitly.
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
    // sa2-nx's switchable enhancements (the renderer reads their keys): resolved before any guest thread exists.
    sa2::enh::Resolve();
  }

  // Input: the SDK's default drivers, plus the scripted test pad when sc5p2_test_buttons(_file) is set.
  // With sa2_renderer_native = true the graphics system is the native renderer (src/me) and the emulated Xenos GPU
  // plugin is not loaded (ReXApp only loads it when config.graphics is empty).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.input_factory = sc5p2::test_input::Create;
    sa2::enh::ApplyGpuBoost();
    if (me::native::Enabled()) {
      config.graphics = me::native::CreateGraphicsSystem();
      REXLOG_INFO("graphics: native renderer (PM4 ring)");
    }
  }

  void OnPostSetup() override {
    sa2::enh::StartReport();
    sa2::render::RegisterKeys();
    auto presenter = [this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    };
    sc5p2::capture::Start(presenter);
#if !defined(__SWITCH__)
    // PC test builds: frame dumps of the guest output (MGSV_FRAME_DUMP_SECONDS / MGSV_FRAME_DIR), to compare builds.
    frame_dump_.Start(presenter);
#endif
  }

  void OnShutdown() override {
    sc5p2::capture::Stop();
#if !defined(__SWITCH__)
    frame_dump_.Stop();
#endif
  }

  // Without --game_data_root, use a game_root/ folder next to the executable (on the Switch:
  // sdmc:/switch/mgsv-nx/game_root), or the workspace's extracted/ disc when run from a build tree.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) {
      return;
    }
    for (auto candidate = folder; !candidate.empty(); candidate = candidate.parent_path()) {
      for (const auto& sub : {std::filesystem::path("game_root"), std::filesystem::path("extracted")}) {
        auto pack = candidate / sub;
        pack += ".nxpack";
        if (std::filesystem::is_regular_file(candidate / sub / "default.xex", ec) ||
            std::filesystem::is_regular_file(pack, ec)) {
          paths.game_data_root = candidate / sub;
          return;
        }
      }
      if (candidate == candidate.parent_path()) {
        break;
      }
    }
  }

 private:
#if !defined(__SWITCH__)
  mgsv::FrameDump frame_dump_;
#endif
};
