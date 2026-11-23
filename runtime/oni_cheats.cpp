// Onimusha's cheats (oni_cheats.h). Every field is the game's own, named by its DWARF (sys_sub.c's types,
// tools/oni_dwarf.py): the game's work block G (struct @anon28, 0x27de30) and the player's object control block.
//
// - Infinite health: the player's _OCW is Player_main's work at 0x287ed0: Max_life (+0x6ce), Life (+0x6d0), shorts.
// - Infinite magic: G.Sv_magic[3] (+0xa03e: the lightning, fire and wind orbs' magic, as pl_att_sh_l / _f / _w spend
//   it) against G.Sv_max_magic (+0x988e).
// - Infinite souls: G.Exp_accumulater (+0x9e84, an int): the souls the HUD counts and the gauntlet's upgrades
//   (kyouka_push / kyouka_pull) spend.
// - Infinite items: G.Item_work[65] (+0x9eac: Id, Num, Type, Status, 4 bytes each; Num 0xff for an item that is not
//   counted): a count that went down is put back, and a counted item whose slot emptied comes back with one.
// - One-hit kills: the enemies are G's list from pEm_top (+0x45bc) to pEm_end (+0x45cc), linked by pNext (+0x30),
//   each a _ENEMY_WORK whose Max_life / Life are where the player's are; an active one's (Be_flg bit 0) life is kept at
//   1, so the next hit takes it to 0 and the game's own damage code makes the death (its souls, its animation).
// - The room warp: the game's own door transition. A door's _IN_DOOR_WORK (18 bytes) is written at 0x1ffd000 (EE
//   memory nothing uses), G.pSce_aot_on (+0x986c) set to it and G.Doordemo_flg (+0x9879) raised, as Sce_Door does when
//   the player opens a door; game_sp_check then switches the game to game_doordemo (G.mode, +0x44b0, to 3), which
//   fades out, takes the stage, room, cut, spawn point and floor from it and loads the room.
#include "oni_cheats.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace oni::cheats
{
    namespace
    {
        constexpr uint32_t kG = 0x27de30;
        constexpr uint32_t kPlayer = 0x287ed0, kMaxLife = kPlayer + 0x6ce, kLife = kPlayer + 0x6d0;
        constexpr uint32_t kMode = kG + 0x44b0;            // game_* by index: 2 game_move (in play), 3 game_doordemo
        constexpr uint32_t kPlace = kG + 0x97b0;           // stage, room, cut (shorts: 0x2875e0)
        constexpr uint32_t kDoorOn = kG + 0x986c;          // pSce_aot_on
        constexpr uint32_t kDoorDemo = kG + 0x9879;        // Doordemo_flg
        constexpr uint32_t kMaxMagic = kG + 0x988e;        // Sv_max_magic
        constexpr uint32_t kMagic = kG + 0xa03e;           // Sv_magic[3]
        constexpr uint32_t kSoulsAt = kG + 0x9e84;         // Exp_accumulater
        constexpr uint32_t kItems = kG + 0x9eac;           // Item_work[65]
        constexpr uint32_t kItemCount = 65;
        constexpr uint32_t kWarpDoor = 0x1ffd000;          // the warp's _IN_DOOR_WORK

        bool fromEnvironment(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e && *e != '0';
        }

        std::atomic<bool> g_on[5] = {fromEnvironment("ONI_CHEAT_HEALTH"), fromEnvironment("ONI_CHEAT_MAGIC"),
                                     fromEnvironment("ONI_CHEAT_SOULS"), fromEnvironment("ONI_CHEAT_ITEMS"),
                                     fromEnvironment("ONI_CHEAT_KILLS")};
        constexpr uint32_t kEnemyTop = kG + 0x45bc, kEnemyEnd = kG + 0x45cc;
        std::atomic<int> g_warp{-1};
        std::atomic<int> g_place[3] = {-1, -1, -1};
        std::mutex g_statusLock;
        std::string g_status;

        // The items as they were at the last frame (infinite items).
        std::array<std::array<uint8_t, 4>, kItemCount> g_items{};
        bool g_itemsKnown = false;

        int16_t read16(const uint8_t *ram, uint32_t a)
        {
            int16_t v;
            std::memcpy(&v, ram + a, 2);
            return v;
        }
        void write16(uint8_t *ram, uint32_t a, int16_t v) { std::memcpy(ram + a, &v, 2); }

        void setStatus(const std::string &s)
        {
            std::lock_guard<std::mutex> lock(g_statusLock);
            g_status = s;
            std::printf("[oni:cheats] %s\n", s.c_str());
        }

        // ONI_WARP=<stage>:<room>: a warp requested at the start (runs without a window).
        int warpFromEnvironment()
        {
            const char *e = std::getenv("ONI_WARP");
            int stage = -1, room = -1;
            if (!e || std::sscanf(e, "%d:%d", &stage, &room) != 2)
                return -1;
            for (size_t i = 0; i < kWarpCount; ++i)
                if (kWarps[i].stage == stage && kWarps[i].room == room)
                    return static_cast<int>(i);
            return -1;
        }
    } // namespace

    bool enabled(Cheat cheat) { return g_on[static_cast<int>(cheat)].load(std::memory_order_relaxed); }
    void setEnabled(Cheat cheat, bool on) { g_on[static_cast<int>(cheat)].store(on, std::memory_order_relaxed); }

    void requestWarp(size_t index)
    {
        if (index >= kWarpCount)
            return;
        g_warp.store(static_cast<int>(index), std::memory_order_relaxed);
        setStatus("warp to stage " + std::to_string(kWarps[index].stage) + ", room " + std::to_string(kWarps[index].room) +
                  ": at the next moment in play");
    }

    std::string warpStatus()
    {
        std::lock_guard<std::mutex> lock(g_statusLock);
        return g_status;
    }

    void place(int &stage, int &room, int &cut)
    {
        stage = g_place[0].load(std::memory_order_relaxed);
        room = g_place[1].load(std::memory_order_relaxed);
        cut = g_place[2].load(std::memory_order_relaxed);
    }

    void apply(uint8_t *ram)
    {
        static const bool envWarp = [] {
            const int w = warpFromEnvironment();
            if (w >= 0)
                requestWarp(static_cast<size_t>(w));
            return true;
        }();
        (void)envWarp;
        for (int k = 0; k < 3; ++k)
            g_place[k].store(read16(ram, kPlace + 2 * k), std::memory_order_relaxed);
        const bool inPlay = ram[kMode] == 2;

        if (enabled(Cheat::InfiniteHealth))
        {
            const int16_t maxLife = read16(ram, kMaxLife);
            if (maxLife > 0 && read16(ram, kLife) < maxLife) // a player is in play (no block outside the rooms)
                write16(ram, kLife, maxLife);
        }
        if (enabled(Cheat::InfiniteMagic))
        {
            const int16_t maxMagic = read16(ram, kMaxMagic);
            if (maxMagic > 0)
                for (int k = 0; k < 3; ++k)
                    if (read16(ram, kMagic + 2 * k) < maxMagic)
                        write16(ram, kMagic + 2 * k, maxMagic);
        }
        if (enabled(Cheat::InfiniteSouls) && inPlay)
        {
            int32_t souls;
            std::memcpy(&souls, ram + kSoulsAt, 4);
            if (souls < kSouls)
                std::memcpy(ram + kSoulsAt, &kSouls, 4);
        }
        // (Not in stage 0, room 0: its zombie fight is scripted, the zombies' dmg_ck refilling their life at each hit until
        // the event that ends it; a life kept at 1 kept that event from coming.)
        if (enabled(Cheat::OneHitKills) && inPlay && !(read16(ram, kPlace) == 0 && read16(ram, kPlace + 2) == 0))
        {
            uint32_t at, end;
            std::memcpy(&at, ram + kEnemyTop, 4);
            std::memcpy(&end, ram + kEnemyEnd, 4);
            at &= 0x1ffffffu;
            end &= 0x1ffffffu;
            for (int guard = 0; at && at + 0x7e0 <= 0x2000000u && guard < 64; ++guard)
            {
                if ((ram[at] & 1u) && read16(ram, at + 0x6d0) > 1)
                    write16(ram, at + 0x6d0, 1);
                if (at == end)
                    break;
                std::memcpy(&at, ram + at + 0x30, 4);
                at &= 0x1ffffffu;
            }
        }
        // Infinite items: against the last frame's inventory.
        if (enabled(Cheat::InfiniteItems) && inPlay)
        {
            for (uint32_t i = 0; i < kItemCount; ++i)
            {
                uint8_t *slot = ram + kItems + 4 * i;
                const auto &was = g_items[i];
                if (!g_itemsKnown || was[0] == 0 || was[1] == 0xff)
                    continue;
                if (slot[0] == was[0] && slot[1] < was[1] && slot[1] != 0xff)
                    slot[1] = was[1]; // used: the count back
                else if (slot[0] == 0 && was[1] == 1)
                    std::memcpy(slot, was.data(), 4); // the last one used: it comes back
            }
        }
        for (uint32_t i = 0; i < kItemCount; ++i)
            std::memcpy(g_items[i].data(), ram + kItems + 4 * i, 4);
        g_itemsKnown = inPlay;

        // The warp: once the game is in play and no door is under way.
        const int warp = g_warp.load(std::memory_order_relaxed);
        if (warp >= 0 && inPlay && ram[kDoorDemo] == 0)
        {
            const Warp &w = kWarps[warp];
            uint8_t door[18] = {};
            std::memcpy(door + 0, &w.x, 2);
            std::memcpy(door + 2, &w.y, 2);
            std::memcpy(door + 4, &w.z, 2);
            std::memcpy(door + 6, &w.dir, 2);
            door[8] = w.stage;
            door[9] = w.room;
            door[10] = w.cut;
            door[11] = static_cast<uint8_t>(w.floor);
            door[12] = w.dtexType;
            door[13] = w.doorType;
            std::memcpy(ram + kWarpDoor, door, sizeof door);
            const uint32_t pointer = kWarpDoor;
            std::memcpy(ram + kDoorOn, &pointer, 4);
            ram[kDoorDemo] = 1;
            g_warp.store(-1, std::memory_order_relaxed);
            setStatus("warping to stage " + std::to_string(w.stage) + ", room " + std::to_string(w.room) + ", cut " +
                      std::to_string(w.cut));
        }
    }
} // namespace oni::cheats
