// Unlocks every bonus: multiplayer maps and modes (Geonosis Academy, Control Zone, Conquest...), bonus extras and
// characters. Profile_IsBonusUnlocked normally tests the save profile's unlock mask; this makes it always true
// without changing the saved profile.

#include "cw_mod.h"
#include "game/GameSymbols.h"

namespace {

bool __fastcall isBonusUnlocked(void* profile, void* edx, unsigned char bonusId) {
    return true;
}

} // namespace

extern "C" __declspec(dllexport) int __cdecl CwModInit(const CwModApi* api) {
    if (api->version != CW_MOD_API_VERSION) {
        return 0;
    }
    void* original = nullptr;
    return api->detour(reinterpret_cast<std::uint32_t>(cw::game::Profile_IsBonusUnlocked), reinterpret_cast<const void*>(&isBonusUnlocked),
        &original, "Profile_IsBonusUnlocked (unlock_all)");
}
