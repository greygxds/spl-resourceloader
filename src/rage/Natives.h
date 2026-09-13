#pragma once

namespace spl::rage
{
// The few natives the loader itself calls. They live in their own translation unit because
// ScriptHookV's headers bring <windows.h> along, whose macros (GetMessage, GetFileAttributes)
// would rename our own methods in any file that includes them.

/// GET_IS_LOADING_SCREEN_ACTIVE. Only valid on the script thread.
[[nodiscard]] bool IsLoadingScreenActive();

/// IS_PLAYER_SWITCH_IN_PROGRESS. Only valid on the script thread.
[[nodiscard]] bool IsPlayerSwitchInProgress();

struct WorldPosition
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

/// GET_ENTITY_COORDS(PLAYER_PED_ID()). Only valid on the script thread.
[[nodiscard]] WorldPosition GetPlayerPosition();
} // namespace spl::rage
