// Onimusha's cheats: toggles the player sets in the window's Cheats tab (F2, runtime/app/oni_cheats_ui.cpp) or, for
// runs without a window, from the environment (ONI_CHEAT_HEALTH=1). Applied on the game's thread at each frame's end.
#pragma once

#include <cstdint>

namespace oni::cheats
{
    enum class Cheat
    {
        InfiniteHealth, // Samanosuke's life kept at its maximum
    };
    bool enabled(Cheat cheat);
    void setEnabled(Cheat cheat, bool on);
    // Once a frame, on the game thread (the frame boundary): the enabled cheats' writes to the game's state.
    void apply(uint8_t *ram);
    // The window's Cheats tab (only in the window build).
    void registerMenu();
} // namespace oni::cheats
