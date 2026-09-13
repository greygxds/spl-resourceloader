#include "rage/Natives.h"

#include <natives.h> // ScriptHookV SDK

namespace spl::rage
{
bool IsLoadingScreenActive()
{
    return DLC2::GET_IS_LOADING_SCREEN_ACTIVE() != FALSE;
}

bool IsPlayerSwitchInProgress()
{
    return STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS() != FALSE;
}

WorldPosition GetPlayerPosition()
{
    const Vector3 position = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE);
    return WorldPosition{.x = position.x, .y = position.y, .z = position.z};
}
} // namespace spl::rage
