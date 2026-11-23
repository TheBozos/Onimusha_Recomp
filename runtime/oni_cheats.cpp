// Onimusha's cheats (oni_cheats.h).
//
// Infinite health: the player's object control block (_OCW, sys_sub.c's types) is at 0x287ed0 (Player_main's work);
// its Max_life (+0x6ce) and Life (+0x6d0) are shorts (128 at the start of the game). At each frame's end Life is set
// back to Max_life, so no hit taken over a frame can bring it to zero.
#include "oni_cheats.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace oni::cheats
{
    namespace
    {
        constexpr uint32_t kPlayer = 0x287ed0, kMaxLife = kPlayer + 0x6ce, kLife = kPlayer + 0x6d0;

        bool fromEnvironment(const char *name)
        {
            const char *e = std::getenv(name);
            return e && *e && *e != '0';
        }

        std::atomic<bool> g_health{fromEnvironment("ONI_CHEAT_HEALTH")};
    } // namespace

    bool enabled(Cheat cheat)
    {
        switch (cheat)
        {
        case Cheat::InfiniteHealth: return g_health.load(std::memory_order_relaxed);
        }
        return false;
    }

    void setEnabled(Cheat cheat, bool on)
    {
        switch (cheat)
        {
        case Cheat::InfiniteHealth: g_health.store(on, std::memory_order_relaxed); break;
        }
    }

    void apply(uint8_t *ram)
    {
        if (g_health.load(std::memory_order_relaxed))
        {
            int16_t maxLife, life;
            std::memcpy(&maxLife, ram + kMaxLife, 2);
            std::memcpy(&life, ram + kLife, 2);
            if (maxLife > 0 && life < maxLife) // a player is in play (no block outside the rooms)
                std::memcpy(ram + kLife, &maxLife, 2);
        }
    }
} // namespace oni::cheats
