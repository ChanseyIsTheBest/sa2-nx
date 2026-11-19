#include <rex/cvar.h>
#include <rex/hook.h>

#include <cstdint>

// Name = "Disable Townsfolk"
REXCVAR_DEFINE_BOOL(disable_townsfolk, false, "Nuts&Bolts/Showdown Town",
	"Stops the wandering Showdown Town townsfolk from spawning. Named characters "
	"(Mumbo, Humba Wumba, ...) are unaffected. Applies as you move around town.");

REX_EXTERN(__imp__sub_82241D38);  // spawn-point blocked test

// Return address of the blocked-test call at 0x821BD97C, inside sub_821BD8F8.
static constexpr uint32_t kCrowdSpawnBlockedTestLr = 0x821BD980;

REX_HOOK_RAW(sub_82241D38) {
	if (REXCVAR_GET(disable_townsfolk) && ctx.lr == kCrowdSpawnBlockedTestLr) {
		ctx.r3.u64 = 1;  // "blocked" -> the spawner skips this point
		return;
	}

	__imp__sub_82241D38(ctx, base);
}
