// The port's display options (runtime/oni_display.cpp): widescreen, a 16:9 zoom of the game's 4:3 picture.
#pragma once

namespace oni::display
{
    // The player's choice (onimusha.ini in the user folder, the F2 Game tab; ONI_WIDESCREEN=1/0 over the file).
    bool widescreen();
    void setWidescreen(bool on);
    // Reads the file and the environment (the window build, before the window opens); saves the choice back.
    void load(const char *userFolder);
    void save();

    // The zoom in effect this frame (the game thread): widescreen on and the game in play, at a door or in a movie.
    // Its 2D (the HUD, the messages) is then drawn at 3/4 of its size around the frame's centre, inside the 16:9 band.
    bool zoomed();
    // At each frame's end: decides the next frame's zoom from the game's mode (G+0x44b0) and the movie.
    void frame(const unsigned char *ram, bool movieShown);

    // The 2D's place in the zoom: (x, y) in the frame's own units around its centre (cx, cy).
    constexpr float kZoom2d = 0.75f;
} // namespace oni::display
