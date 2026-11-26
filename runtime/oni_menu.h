// The port's pages inside the game's own OPTION screen (runtime/oni_menu.cpp): L1 / R1 turn from Options to Graphics,
// Cheats, Save States and Close Game, drawn like Options (its scroll, orbs, arrows and highlight, the game's font) and
// driven by the pad. The F2 window keeps what needs a keyboard and a mouse (Controls, the Profiler).
#pragma once
#include "bozo/app/video_settings.h"
#include "bozo/port.h"

#include <functional>
#include <string>

namespace oni::menu
{
    // What the pages reach outside the game (the window build, before the game thread starts; without it, as headless,
    // the Graphics and Save States pages say they are not available). Called on the game thread.
    struct Access
    {
        std::function<bozo::app::video::Shared()> videoShared;                 // what is applied, the sizes to offer
        std::function<void(const bozo::app::video::Settings &, bool)> videoPost; // applied at the window's next frame
        std::function<std::string(int)> slotSummary;                            // "Slot 3: saved 2026-11-26 14:02"
        std::function<void(int)> saveSlot, loadSlot;                            // at the frame's end
        int slots = 0;
    };
    void setAccess(Access access);

    void install(bozo::port::Registry &registry);
    // The Close Game page's answer, taken at the frame boundary (the host then stops).
    bool takeQuitRequest();
    // A save state was loaded: back to the Options page (the pages keep nothing in the game's memory).
    void stateLoaded();
} // namespace oni::menu
