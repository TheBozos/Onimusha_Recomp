// The story bot (ONI_BOT=<script>): plays the game from a save state by a script of goals, steering with the D-pad
// against the player's position each frame, for headless runs that go through the story in its own order.
// Script lines (# comments):
//   go X Z [R] [N]    walk to (X, Z) until within R (300), N frames in play at most (1800; negative: then go on)
//   door S R          walk into the current room's door to stage S, room R and press the action button until the
//                     game is in that room (in play; with `enter`, as soon as it is there: an event's question)
//   take [K] [N]      walk into the nearest item AOT or glint (or slot K; -1 the nearest), face it, press the action
//                     button, close the message (N frames, 2400; negative: none there or not reached is no failure)
//   examine [K] [N]   the same with the nearest event / message AOT (Id 4, 5, 12): done when what it starts is over
//   open [K]          walk to the nearest event / item AOT (or slot K), press the action button once it is faced, and
//                     stop as soon as the game leaves play (a puzzle, a question: the next steps answer it)
//   fight [N] [M]     attack the nearest live enemy until none is left (or N frames, 3600, negative: then go on; or
//                     the game in mode M)
//   play [N]          let an event play until the game is in play for a second (CROSS every 40 frames advances its
//                     messages; N frames at most, 12000)
//   press BTN [F]     hold a button (or a chord: R1+TRIANGLE) F frames (6), then release it for as long
//   cursor C R        stage 5 room 21's sliding puzzle: its cursor moved to column C, row R (read back from RAM)
//   slide HOLD DIR [F] hold a button F frames (40) and tap another while it is held (a sliding puzzle's grab-and-move)
//   wait N            nothing for N frames
//   place S R [N]     wait until the game is in stage S, room R (N frames at most, 3600)
//   attack X Z [N]    turn toward (X, Z) and swing N times (4): pots, crates
//   pots [N] [ID]     Oni Spirits: break pots (the objects of id 0x7a), enemies on the ground fought first (but not
//                     enemy id ID), until the holy ground's quota is released or no pot stands (N frames in play,
//                     7200; negative: then go on)
//   cheat NAME on|off health, magic, souls, items or kills switched (the Cheats tab's)
//   piece X Z [N]     walk straight onto (X, Z) until the control passes to the other character (stage 5 room 20)
//   engage on|off     enemies on the way fought (on, the default) or passed by
//   learn on|off      where the way is held, an obstacle is learnt and the way planned around it (off by default)
//   face X Z          turn toward (X, Z) on the spot
//   teleport X Z [Y]  the player moved within the room (a shortcut where the bot cannot find the way)
//   aots              the room's live AOTs (doors, items, events) logged
//   save PATH         a save state (BozoEngine's, at its next safe point)
//   end               the run stops here
// Each step logs "[oni:bot]"; a step that times out logs why and the bot stops (the run goes on idle).
#pragma once

#include <cstddef>
#include <cstdint>

namespace bozo::host
{
    class FreeHost;
}

namespace oni::bot
{
    // A door of a room (tools/oni_warps_gen.py: every Door_aot_set / Door_aot_set_4p of the room files): its AOT
    // rectangle (a four-point door's bounding box) and where it leads.
    struct Door
    {
        uint8_t stage, room;
        int16_t x, z;
        uint16_t w, d;
        uint8_t toStage, toRoom, toCut, key;
    };
    extern const Door kDoors[];
    extern const size_t kDoorCount;

    // Installs the bot as player 1's controller when ONI_BOT is set; false otherwise.
    bool start(bozo::host::FreeHost &host);
    // Once a frame on the game thread (the frame boundary): the bot's decisions for the next frame.
    void frame(uint8_t *ram, uint64_t frameNumber);
    // Whether the script asked the run to end.
    bool finished();
} // namespace oni::bot
