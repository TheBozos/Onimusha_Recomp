// Onimusha's cheats: toggles the player sets in the window's Cheats tab (F2, runtime/app/oni_cheats_ui.cpp) or, for
// runs without a window, from the environment (ONI_CHEAT_HEALTH=1, ONI_CHEAT_MAGIC=1, ONI_CHEAT_SOULS=1,
// ONI_CHEAT_ITEMS=1, ONI_CHEAT_KILLS=1, ONI_WARP=<stage>:<room>). Applied on the game's thread at each frame's end.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace oni::cheats
{
    enum class Cheat
    {
        InfiniteHealth, // Samanosuke's life kept at its maximum
        InfiniteMagic,  // the three orbs' magic kept at its maximum
        InfiniteSouls,  // the souls to spend on upgrades kept at kSouls
        InfiniteItems,  // counted items (herbs, arrows, bullets...) never go down
        OneHitKills,    // every enemy's life kept at 1: the next hit kills it (the game's own death)
    };
    bool enabled(Cheat cheat);
    void setEnabled(Cheat cheat, bool on);
    constexpr int32_t kSouls = 99999;

    // The room warp's destinations: one door of the game's own per room a door leads into (tools/oni_warps_gen.py,
    // generated from the disc's room files), with the spawn point and the transition the game uses there.
    struct Warp
    {
        uint8_t stage, room, cut;
        int16_t x, y, z, dir;
        int8_t floor;
        uint8_t dtexType, doorType;
        uint8_t fromStage, fromRoom;
    };
    extern const Warp kWarps[];
    extern const size_t kWarpCount;
    // A warp to kWarps[index], made at the next frame the game is in play (its own door transition: the fade, the
    // room's load, the spawn point). What happened last, for the tab.
    void requestWarp(size_t index);
    std::string warpStatus();
    // The stage, room and cut the game is in (-1 before a room is loaded).
    void place(int &stage, int &room, int &cut);

    // Once a frame, on the game thread (the frame boundary): the enabled cheats' writes to the game's state.
    void apply(uint8_t *ram);
    // The window's Cheats tab (only in the window build).
    void registerMenu();
} // namespace oni::cheats
