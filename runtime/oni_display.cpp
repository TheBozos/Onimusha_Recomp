// Widescreen for a game whose rooms are fixed 4:3 pictures: they cannot show more at the sides, so the picture is
// zoomed to 16:9 (its width filling the window, an eighth cut at the top and at the bottom: BozoEngine's
// setPictureCrop) while the game is in play, at a door or in a movie (the movies are 16:9 with black bars: those go).
// The menus (the status screen, the title, the save screens) stay 4:3: their art spans the whole frame. In the zoom
// the 2D drawn in screen units (Ot_draw's screen-space primitives, Pit_trans's HUD and fades: oni_sprite.cpp) is
// drawn at 3/4 of its size around the frame's centre, so the HUD and the messages stay inside the 16:9 band; what
// covers the whole frame (a fade) keeps its size.
#include "oni_display.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace oni::display
{
    namespace
    {
        std::atomic<bool> g_widescreen{false};
        bool g_zoomed = false;
        std::string g_file;

        std::string trim(std::string t)
        {
            while (!t.empty() && (t.back() == ' ' || t.back() == '\r' || t.back() == '\t'))
                t.pop_back();
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
                t.erase(t.begin());
            return t;
        }
    } // namespace

    bool widescreen() { return g_widescreen.load(std::memory_order_relaxed); }
    void setWidescreen(bool on) { g_widescreen.store(on, std::memory_order_relaxed); }

    void load(const char *userFolder)
    {
        if (userFolder && *userFolder)
        {
            g_file = (std::filesystem::path(userFolder) / "onimusha.ini").string();
            std::ifstream in(g_file);
            for (std::string line; std::getline(in, line);)
            {
                const size_t eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == std::string::npos)
                    continue;
                if (trim(line.substr(0, eq)) == "widescreen")
                    setWidescreen(trim(line.substr(eq + 1)) == "on");
            }
        }
        if (const char *e = std::getenv("ONI_WIDESCREEN"); e && *e)
            setWidescreen(*e != '0');
    }

    void save()
    {
        if (g_file.empty())
            return;
        std::ofstream out(g_file);
        out << "# Onimusha: the port's own options (the F2 Game tab).\n"
               "# widescreen: on (the picture zoomed to 16:9 in play and in the movies, its top and bottom cut: the rooms\n"
               "# are 4:3 pictures) or off (4:3, as the console).\n"
            << "widescreen = " << (widescreen() ? "on" : "off") << "\n";
    }

    bool zoomed() { return g_zoomed; }

    void frame(const unsigned char *ram, bool movieShown)
    {
        const unsigned mode = ram[0x27de30 + 0x44b0];
        g_zoomed = widescreen() && (movieShown || mode == 2 || mode == 3);
    }
} // namespace oni::display
