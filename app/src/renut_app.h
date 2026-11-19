#pragma once
#include "profiler/banjo_pc_profiler.h"
#include "banjo_pgo.h"
#include <rex/filesystem.h>
#include <rex/platform.h>
#include <rex/rex_app.h>
#include "renut_engine/overlays/fps_overlay_dialog.h"
#include "renut_engine/renut_logging.h"
#include "renut_engine/overlays/renut_logging_overlay.h"
#if !REX_PLATFORM_SWITCH
#include "renut_engine/overlays/path_setup_wizard.h"
#include "renut_engine/overlays/mnk_controls_dialog.h"
#endif
#include "renut_engine/mnk_controls.h"
#include "renut_engine/Timer.h"
#include "renut_engine/Fps.h"
#include "renut_engine/hooks.h"
// katamari-nx: N&B's title update is not part of this port (Beautiful Katamari has no title update in the user's
// files). The DLC installer is (2026-10-31, the user's 16 DLC packages): reNut upstream's, game-agnostic.
#include "renut_engine/dlc_install.h"
#include "profiler/profiler.h"
#include "profiler/profiler_overlay.h"
#include "game_menu.h"  // blur-nx (2026-11-18): the game menu (A+B+X+Y+L+R, F8)
#include "me/me_native_system.h"
// banjo-nx (2026-10-16): Renut's Direct3D-level renderer, the boot choice banjo_renderer = "renut"
// (app/src/renut_engine/gpu, tools/renut_gpu_import.py, docs/renut-renderer-port.md).
#if BANJO_RENUT_RENDERER
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/gpu/banjo_renut_capture.h"
#endif
#include "cpu/banjo_enhancements.h"
#if defined(KATAMARI_BUILTIN_CONFIG)
#include "katamari_builtin_config.h"  // generated from app/katamari_switch_defaults.toml and katamari_switch.toml (CMake)
#endif
namespace katamari::stage_diag {
void Start();  // src/cpu/katamari_stage_diag.cpp
}
namespace blur::small_alloc {
void Enable();  // blur-nx (2026-11-17, #220; renut-ultimate port 151): src/blur_small_alloc.cpp, after setup
}
#include "native/nfsmw_nativo_captura.h"
#if !REX_PLATFORM_SWITCH
#include "profiler/nfsmw_perfil_pc.h"
#endif
#include <rex/ui/window.h>
//#include <rex/discord_rpc.h>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>

REXCVAR_DECLARE(bool, banjo_show_fps);  // banjo-nx (2026-10-21): profiler/profiler.cpp

REXCVAR_DECLARE(bool, banjo_mesa_disk_cache_off);
REXCVAR_DECLARE(bool, masseffect_nvk_cache_per_stage);
REXCVAR_DECLARE(bool, banjo_gpu_boost);
#if BANJO_RENUT_RENDERER
REXCVAR_DECLARE(bool, banjo_renut_linked_stages);  // renut_engine/gpu/banjo_renut_tuning.cpp
#endif
REXCVAR_DECLARE(bool, banjo_gpu_zcull_greater);
#if REX_PLATFORM_SWITCH
extern "C" {
void RexSwitchApmRequestGpuMhz(int mhz);  // sdk/src/ui/switch_apm.cpp
void RexSwitchApmApply(void);
}
#endif

class RenutApp : public rex::ReXApp {
public:
    RenutApp(rex::ui::WindowedAppContext& ctx, std::string_view name, rex::PPCImageInfo info)
        : rex::ReXApp(ctx, name, info), app_name_(name) {}

    static std::unique_ptr<rex::ui::WindowedApp> Create(
        rex::ui::WindowedAppContext& ctx) {
        // katamari-nx: banjo-nx's name, so its config is renut.toml (the built-in config's build: OnConfigurePaths).
        return std::unique_ptr<RenutApp>(new RenutApp(ctx, "renut", PPCImageConfig));
    }

    // katamari-nx (2026-11-02, sa2-nx's design): the Switch build's fixed settings, compiled in and applied before
    // katamari.toml, which holds only the player's options (a key added there still overrides these: test runs).
    std::string_view BuiltInConfig() const override {
#if defined(KATAMARI_BUILTIN_CONFIG)
        return kKatamariBuiltInConfig;
#else
        return {};
#endif
    }
    // Without a katamari.toml next to the NRO, the player's default one is written there.
    std::string_view DefaultConfigFile() const override {
#if defined(KATAMARI_BUILTIN_CONFIG)
        return kKatamariPlayerConfig;
#else
        return {};
#endif
    }


    // void OnPostSetup() override {
    //     rex::discord_rpc::Presence rpc;

    //     rpc.details_ = "";
    //     rpc.state_ = "";
    //     rpc.large_image_key_ = "e242d6b6-c34e-47a1-8c2a-5297fe33bce7";
    //     rpc.large_image_text_ = "renut";

    //     //rex::discord_rpc::Start(Application ID, Settings);
    //     rex::discord_rpc::Start("1520303728047951892", rpc);

    //     rex::cvar::LoadConfig("renut.toml");
    // }

    // blur-nx (2026-11-18): the game menu's fonts (the console's own shared font) in the SDK drawer's atlas.
    void OnConfigureFonts(ImFontAtlas* atlas) override { blur::AddGameMenuFonts(atlas); }

    void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
#if BANJO_RENUT_RENDERER
        // katamari-nx (2026-11-01): the app's dialogs draw with the ImGui inside this executable, whose context only
        // Renut's overlay drawer creates; under the SDK's presenter (banjo_renderer = "emulated") they would draw into
        // no context at all. The emulated GPU is a reference image: no overlays.
        if (!banjo::renut_gpu::Active()) return;
#endif
        //drawer->AddDialog(new FpsOverlayDialog(drawer));
        fps_dialog_ = std::make_unique<FpsOverlayDialog>(drawer);
        fps_dialog_->fpsManager = &fpsManager;
        fpsManager.showFPS = REXCVAR_GET(banjo_show_fps);  // banjo-nx (2026-10-21)
        drawer->AddDialog(fps_dialog_.get());
        drawer->AddDialog(new RenuLogOverlayDialog(drawer));
        // CPU/GPU profiler window ported from UnleashedRecomp (F5 / banjo_show_profiler).
        profiler_dialog_ = std::make_unique<ProfilerOverlayDialog>(drawer);
        drawer->AddDialog(profiler_dialog_.get());
        // blur-nx (2026-11-18, the user's request): the game menu. Attached for good (its constructor adds it), it
        // asks for no ImGui frame while closed (WantsFrame), so the drawer builds none for it then.
        game_menu_ = std::make_unique<blur::GameMenuDialog>(drawer, config_path(), [this] { return runtime(); },
                                                            [this] { return immediate_drawer(); });
#if !REX_PLATFORM_SWITCH
        path_wizard_ = new PathSetupWizard(drawer);
        drawer->AddDialog(path_wizard_);

        // Keyboard & mouse: the window exists by the time dialogs are created,
        // so this is where the host-side listener gets attached.
        mnk_dialog_ = std::make_unique<MnkControlsDialog>(drawer);
        drawer->AddDialog(mnk_dialog_.get());
        renut::mnk::AttachWindow(window());
#endif
    }

    // Native renderer: with masseffect_renderer_native = true the graphics system is the app's own
    // (src/me, from masseffect-nx) and the emulated Xenos GPU plugin is not loaded (ReXApp only loads it
    // when config.graphics is empty).
    void OnPreSetup(rex::RuntimeConfig& config) override {
        banjo::ApplyMasterSwitches();
#if REX_PLATFORM_SWITCH
        // Before the Vulkan device exists (the graphics system creates it after this). Mesa's own disk cache
        // (fossilize file on the SD card) duplicates the renderer's pipeline cache file, and on the console its
        // writer thread held the cache lock during slow SD writes while the ring thread, in the middle of a
        // vkCreateGraphicsPipelines, waited on that lock for its lookup (switch stall dumps). Same pipelines.
        if (banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_mesa_disk_cache_off))) {
            setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
        }
        // banjo_gpu_zcull_greater: the driver reads NVK_ZCULL once, at its first render pass.
        if (banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_zcull_greater))) {
            setenv("NVK_ZCULL", "greater", 1);
        }
        // Per-stage shader cache keys in NVK (the patched driver reads it at vkCreateDevice; an unpatched one ignores
        // it): a vertex or pixel shader already compiled for another pipeline is not compiled again. In this driver
        // the linked compile only drops vertex outputs the pixel shader does not read, so the image is the same.
        bool per_stage_cache = banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu,
                                                        REXCVAR_GET(masseffect_nvk_cache_per_stage));
#if BANJO_RENUT_RENDERER
        // banjo_renut_linked_stages (package 26): not with Renut's renderer. Per-stage keys compile each stage without
        // the other (disable_lto), which banjo-nx's translator allows for by pruning the vertex outputs itself; Renut's
        // vertex shaders write every output (zeros for the unused ones), and only the linked compile drops the ones the
        // pixel shader does not read.
        if (per_stage_cache && banjo::renut_gpu::Active() &&
            banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_renut_linked_stages))) {
            per_stage_cache = false;
        }
#endif
        if (per_stage_cache) {
            setenv("NVK_SWITCH_STAGE_CACHE", "1", 1);
        }
        // banjo_gpu_boost (from sa2-nx): the official handheld GPU profile; switch_apm.cpp logs what it did on stderr
        // (rex_stderr.log) and requests nothing when docked.
        if (banjo::EnhancementActive(banjo::EnhancementGroup::kNonCpu, REXCVAR_GET(banjo_gpu_boost))) {
            RexSwitchApmRequestGpuMhz(460);
            RexSwitchApmApply();
        }
#endif
#if BANJO_RENUT_RENDERER
        if (banjo::renut_gpu::Active()) {
            // No SDK graphics system and no GPU plugin: the renderer is the app's (detached mode).
            renut::gpu::ConfigureRuntime(config);
            banjo::profiler::SetRendererInfo("Renut native (Direct3D level, plume)", "");
            return;
        }
#endif
        if (me::native::Enabled()) {
            config.graphics = me::native::CreateGraphicsSystem();
            banjo::profiler::SetRendererInfo("Native Vulkan (PM4 ring)", "");
        }
    }

#if BANJO_RENUT_RENDERER
    // Renut's renderer draws the SDK's ImGui through its own plume drawer, and resizes its swapchain itself.
    std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
        if (banjo::renut_gpu::Active()) return renut::gpu::CreateOverlayDrawer();
        return rex::ReXApp::OnCreateImmediateDrawer();
    }
    void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
        rex::ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
        if (banjo::renut_gpu::Active()) renut::gpu::OnWindowResized();
    }
    // The window exists here (the path wizard is drawn before the game starts): Renut's renderer comes up first.
    void StartRenutRenderer() {
        if (banjo::renut_gpu::Active())
            renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); });
    }
#else
    void StartRenutRenderer() {}
#endif

    void OnPostSetup() override {
        // blur-nx (2026-11-17, #220; renut-ultimate port 151): the small-block arena, blur_small_alloc_mb (32 MB by
        // default, Blur's size; 0 in a game's toml turns it off). Before the DLC install, as Blur has it.
        blur::small_alloc::Enable();
        // katamari-nx (2026-10-31): DLC packages in the "dlc" folder next to the NRO / exe installed before the game
        // starts (reNut upstream's installer, renut_engine/dlc_install.cpp, as banjo-nx does).
        renut::dlc::InstallPackages(runtime());
        // One log line with every enhancement and whether it is active (src/cpu/banjo_enhancements.h).
        banjo::LogEnhancements();
        // katamari-nx (2026-11-01): katamari_diag_game_state (off): the stage fields of the game state, as they change.
        katamari::stage_diag::Start();
        // PC: function-level CPU profile (banjo_pc_profile_s = N seconds per report; 0 = off; from futari-nx).
        banjo::pc_profiler::Start();
        // Switch: per-thread CPU accounting, the optional sampler and the stall watchdog.
        banjo::profiler::Start();
        // PNG captures of the game image for testing (nfsmw_captura_cada_s = N seconds; 0 = off). Taken from
        // the presenter, so no overlay or window can cover them.
#if BANJO_RENUT_RENDERER
        // Renut's renderer has no SDK presenter: its own Present reads the frame back (banjo_renut_capture.h).
        if (banjo::renut_gpu::Active()) {
            nfsmw::captura::Arrancar(nfsmw::captura::Capturador(
                [](rex::ui::RawImage& image) { return renut::gpu::CaptureFrame(image); }));
        } else
#endif
        nfsmw::captura::Arrancar([this]() -> rex::ui::Presenter* {
            const auto* rt = runtime();
            const auto* graphics = rt ? rt->graphics_system() : nullptr;
            return graphics ? graphics->presenter() : nullptr;
        });
#if !REX_PLATFORM_SWITCH
        // Sampling CPU profiler for PC tests (nfsmw_perfil_pc_desde_s; off by default).
        nfsmw::perfil_pc::Arrancar();
#endif
        // The PGO-instrumented build (BANJO_PGO=generate) saves its counts to pgo/ every 2 minutes (nothing otherwise).
        banjo::pgo::Start();
    }

    // The PGO-instrumented build saves its counts when the game loses focus (HOME menu): it may be closed from there.
    void OnWindowFocusChanged(bool focused) override {
        if (!focused) banjo::pgo::DumpNow();
    }

    void OnShutdown() override {
#if BANJO_RENUT_RENDERER
        if (banjo::renut_gpu::Active()) renut::gpu::ShutdownRenderer();
#endif
        banjo::pc_profiler::Stop();
        nfsmw::captura::Parar();
#if !REX_PLATFORM_SWITCH
        nfsmw::perfil_pc::Parar();
#endif
        renut::mnk::DetachWindow();
        banjo::pgo::Stop();
    }

    // A game_root folder next to the executable is used when no path was given.
    // On the Switch that is sdmc:/switch/katamari-nx/game_root (the extracted disc).
    void OnConfigurePaths(rex::PathConfig& paths) override {
#if defined(KATAMARI_BUILTIN_CONFIG)
        // The player's settings file. Not renut.toml: the test builds' full one, left on an SD card, would override
        // every built-in setting.
        paths.config_path = rex::filesystem::GetExecutableFolder() / "katamari.toml";
#endif
        if (paths.game_data_root.empty()) {
            const auto folder = rex::filesystem::GetExecutableFolder();
            std::error_code ec;
            const auto extracted = folder / "game_root";
            if (!folder.empty() && std::filesystem::is_directory(extracted, ec)) {
                paths.game_data_root = extracted;
            }
        }
        renut::dlc::SetUserDataRoot(paths.user_data_root);  // the folder the DLC installer checks for installed ones
    }

#if !REX_PLATFORM_SWITCH
    std::optional<rex::PathConfig> OnFinalizePaths(
        const rex::PathConfig& defaults,
        std::function<void(rex::PathConfig)> resume) override
    {
        StartRenutRenderer();
        // renut-ultimate (port 69, 2026-11-10, blur-nx): with no saved paths, a game folder given on the command line or
        // in the toml (or the game_root next to the executable) that holds default.xex is used as given, with no wizard.
        // Before, a new port's first PC run waited for a click on the path dialog, which looked like a hang right after
        // the swapchain was made. Saved paths still come first, as the wizard takes them (a player's own folders).
        std::error_code given_ec;
        const bool given = !defaults.game_data_root.empty() &&
                           std::filesystem::is_regular_file(defaults.game_data_root / "default.xex", given_ec) &&
                           !PathConfigStore::TryLoad(app_name_);
        if (!path_wizard_ || given) {
            // katamari-nx (2026-11-01): banjo_renderer = "emulated" creates no app dialogs (OnCreateDialogs), so no
            // wizard: the paths as given (the command line / the toml).
            renut::dlc::SetUserDataRoot(defaults.user_data_root);
            return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
        }
        path_wizard_->Init(app_name_, defaults, [resume](rex::PathConfig resolved) {
            renut::dlc::SetUserDataRoot(resolved.user_data_root);
            resume(resolved);
            });
        return std::nullopt;
    }
#else
    std::optional<rex::PathConfig> OnFinalizePaths(
        const rex::PathConfig& defaults,
        std::function<void(rex::PathConfig)> resume) override
    {
        StartRenutRenderer();
        return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
    }
#endif

private:
    std::string      app_name_;
#if !REX_PLATFORM_SWITCH
    PathSetupWizard* path_wizard_ = nullptr;
    std::unique_ptr<MnkControlsDialog> mnk_dialog_;
#endif
    std::unique_ptr<FpsOverlayDialog> fps_dialog_;
    std::unique_ptr<ProfilerOverlayDialog> profiler_dialog_;
    std::unique_ptr<blur::GameMenuDialog> game_menu_;  // blur-nx (2026-11-18)
};
