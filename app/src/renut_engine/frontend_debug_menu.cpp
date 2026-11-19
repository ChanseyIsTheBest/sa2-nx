#include <rex/cvar.h>
#include <rex/hook.h>

#include <cstdint>
#include <string>

// Name = "Debug Main Menu"
REXCVAR_DEFINE_STRING(debug_main_menu, "main", "Nuts&Bolts/Debug", "Opens the unused blueprint-style debug main menu in place of the retail one.")
.allowed({ "main", "debug" });

// Scene ids -- index both off_82E5E038 (base) and the descriptor array at 0x82D90EC8.
// Title update 3 (from reNut upstream ab60251): the TU's table added scenes before these, the base's 68 and 70.
static constexpr uint32_t kSceneMainMenu = 76;   // XuiScene_BanjoX_Frontend_MainMenu
static constexpr uint32_t kSceneStartMenu = 78;  // XuiScene_BanjoX_Frontend_StartMenu

REX_EXTERN(__imp__sub_823B7B50);  // original scene open

REX_HOOK_RAW(sub_823B7B50) {
	if (ctx.r4.u32 == kSceneStartMenu && REXCVAR_GET(debug_main_menu) == "debug") {
		// Full 64-bit write, same as the `li r4, N` this replaces.
		ctx.r4.u64 = kSceneMainMenu;
	}

	__imp__sub_823B7B50(ctx, base);
}
