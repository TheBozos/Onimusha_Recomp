// The port's pages inside the game's own OPTION screen (option.c's Syousai task: the title's OPTION and the pause
// menu's). L1 / R1 turn from the game's Options to Graphics, Cheats, Save States and Close Game and back. A page is
// drawn the way Options draws itself: the screen's backdrop (the scroll with its ten rows and two buttons) stays, and
// the page puts the game's own parts on it (the orbs, the arrows, the highlight: Options' textures) and its text in
// the game's font (the message font at 0x2300, each glyph's width from Prop_tbl, at the 1.5 scale Mess_print uses),
// through the native 2D (oni::sprite::overlay). Nothing is written to the game's memory: the original task runs on
// underneath (its fades, its exit), only its list's input and drawing are replaced while a page of the port's is up.
//
// Syousai(pSw = syousai_work 0x410530) each frame: the routine Rno_0 picks (syou_init, syou_move, syou_exit), then
// sprite_disp(pSw) (the orbs, arrows, sliders: the frame's table) and mess_disp() (the labels: Mess_print, Pit_trans).
// syou_move reads G's pad words: +0x4004 the buttons pressed this frame, +0x4010 the same with the held directions
// repeating; TRIANGLE leaves, UP / DOWN move the cursor (pSw+4: rows 0-9, BACK 10, DEFAULT 11), LEFT / RIGHT change a
// value, CROSS picks; nothing while the screen fades (Get_Fade_Status: 0x27c372 at 1). L1 and R1 are unused there.
#include "oni_menu.h"
#include "oni_cheats.h"
#include "oni_display.h"
#include "oni_render.h"
#include "oni_tex.h"

#include "oni_functions.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace oni::menu
{
    namespace
    {
        namespace video = bozo::app::video;

        constexpr uint32_t kWork = 0x410530;            // syousai_work: Rno_0 +0, Cursor +4
        constexpr uint32_t kTrg = 0x27de30 + 0x4004;    // G: the buttons pressed this frame
        constexpr uint32_t kRep = 0x27de30 + 0x4010;    // ...with held directions repeating
        constexpr uint32_t kFade = 0x27c372;            // Get_Fade_Status's byte: 1 while a fade runs
        constexpr uint32_t kPropTbl = 0x1fe4c0;         // Prop_tbl: per character from ' ', its left and its width
        constexpr uint16_t kUp = 0x1000, kRight = 0x2000, kDown = 0x4000, kLeft = 0x8000;
        constexpr uint16_t kCross = 0x40, kTriangle = 0x10, kL1 = 0x4, kR1 = 0x8;

        // The textures Options shows (its uploads, by TEX0): the font, and its parts' sheet in two palettes.
        constexpr uint64_t kFont = 0x2004700621412300ull;
        constexpr uint64_t kParts = 0x2003580621411a40ull; // the arrows (uv 38-97), the slider's knob
        constexpr uint64_t kOrbs = 0x2003586621411a40ull;  // the red orb (uv 0-18) and the plain one (18-36)
        // Attributes as Options' own: a textured sprite (FST, the texture's alpha test), the text (blended by its
        // alpha), the highlight (untextured, added at A 0x20).
        constexpr uint32_t kPartAttr = 0x0400031eu, kTextAttr = 0x0400a35eu, kBarAttr = 0x0400034eu;
        constexpr uint32_t kBarColour = 0x20804040u;
        // The game's text colours (Moji_color 0x203570), A 0x80.
        constexpr uint32_t kWhite = 0x80808080u, kYellow = 0x80006060u, kGrey = 0x80505050u, kOrange = 0x80004070u;
        // Options' sounds (Snd_secall): the cursor, BACK / leaving, a choice, DEFAULT.
        constexpr uint32_t kSeCursor = 0x1000f, kSeBack = 0x501000f, kSeDecide = 0x401000f, kSeDefault = 0x201000f;
        // The font's button glyphs (the row of symbols after '~'; CROSS 136, TRIANGLE 137): R1, L1.
        constexpr char kGlyphR1 = char(139), kGlyphL1 = char(141);

        // The backdrop's rows (frame units): ten 29 apart from y 82, 28 high, x 88-551; the buttons below.
        constexpr float kRowTop = 82, kRowPitch = 29, kRowLeft = 88, kRowRight = 551;
        constexpr float kButtonTop = 380, kLeftButton[2] = {88, 306}, kRightButton[2] = {332, 551};
        constexpr int kRows = 10, kBack = 10, kAction = 11;

        enum Page { Options, Graphics, Cheats, States, Quit, PageCount };
        const char *const kTitles[PageCount] = {"Options", "Graphics", "Cheats", "Save States", "Close Game"};

        struct Row
        {
            std::string label, value;
            bool selectable = true, arrows = false, dim = false;
            uint32_t valueColour = kWhite;
        };

        enum GraphicsRow { Display, Resolution, RenderScale, AntiAliasing, TextureFilter, FrameRate, Widescreen,
                           GraphicsRows };
        constexpr float kScales[] = {0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
        constexpr int kSamples[] = {0, 2, 4, 8};
        constexpr int kSmoothing[] = {0, 1, 2, 4, 8, 16};

        constexpr cheats::Cheat kCheats[] = {cheats::Cheat::InfiniteHealth, cheats::Cheat::InfiniteMagic,
                                             cheats::Cheat::InfiniteSouls, cheats::Cheat::InfiniteItems,
                                             cheats::Cheat::OneHitKills};
        const char *const kCheatNames[] = {"Infinite Health", "Infinite Magic", "Infinite Souls", "Infinite Items",
                                           "One-Hit Kills"};
        constexpr int kWarpRow = 5, kWarpGo = 6;

        enum StatesRow { Slot, SlotInfo, SaveRow, LoadRow };
        enum QuitRow { QuitQuestion, QuitWarning, QuitCancel = 3, QuitConfirm };

        struct Menu
        {
            int page = Options;
            int cursor[PageCount] = {};
            video::Settings graphics; // the Graphics page's choices until APPLY
            bool applied = false;     // APPLY pressed since the last change
            size_t warp = 0;
            bool warpAsked = false;
            int slot = 0;
            const char *stateDone = ""; // "Saving" / "Loading" after the row's CROSS
        } g_menu;

        Access g_access;
        std::atomic<bool> g_quit{false};

        uint16_t read16(const uint8_t *ram, uint32_t a)
        {
            uint16_t v;
            std::memcpy(&v, ram + a, 2);
            return v;
        }

        // ---- The pages' contents ----------------------------------------------------------------------------------

        bool graphicsOpen() { return g_access.videoShared && g_access.videoPost && g_access.videoShared().open; }

        std::string number(const char *format, int v)
        {
            char text[48];
            std::snprintf(text, sizeof text, format, v);
            return text;
        }

        int nearest(const float *values, int count, float v)
        {
            int best = 0;
            for (int i = 1; i < count; ++i)
                if (std::fabs(values[i] - v) < std::fabs(values[best] - v))
                    best = i;
            return best;
        }
        int indexOf(const int *values, int count, int v)
        {
            for (int i = 0; i < count; ++i)
                if (values[i] == v)
                    return i;
            return 0;
        }

        std::string graphicsValue(int row, const video::Settings &g, const video::Shared &shared)
        {
            switch (row)
            {
            case Display:
                return g.mode == video::WindowMode::Fullscreen ? "Fullscreen"
                       : g.mode == video::WindowMode::Borderless ? "Borderless"
                                                                 : "Window";
            case Resolution:
                if (g.mode == video::WindowMode::Borderless)
                    return number("%d", shared.monitor.first) + "x" + number("%d", shared.monitor.second);
                return number("%d", g.width) + "x" + number("%d", g.height);
            case RenderScale:
                return number("%d%%", static_cast<int>(std::lround(g.renderScale * 100)));
            case AntiAliasing:
                return g.antiAliasing ? number("%dx", g.antiAliasing) : "OFF";
            case TextureFilter:
                return g.textureSmoothing == 0 ? "OFF" : g.textureSmoothing == 1 ? "Smooth" : number("%dx", g.textureSmoothing);
            case FrameRate:
                return g.frameRate == 0 ? "60 fps" : g.frameRate < 0 ? number("%d Hz", shared.monitorRefresh) : number("%d fps", g.frameRate);
            case Widescreen:
                return oni::display::widescreen() ? "ON" : "OFF";
            }
            return "";
        }

        bool sameAs(int row, const video::Settings &a, const video::Settings &b)
        {
            switch (row)
            {
            case Display: return a.mode == b.mode;
            case Resolution: return a.mode == video::WindowMode::Borderless || (a.width == b.width && a.height == b.height);
            case RenderScale: return std::fabs(a.renderScale - b.renderScale) < 0.01f;
            case AntiAliasing: return a.antiAliasing == b.antiAliasing;
            case TextureFilter: return a.textureSmoothing == b.textureSmoothing;
            case FrameRate: return a.frameRate == b.frameRate;
            }
            return true;
        }

        std::vector<Row> rows(int page)
        {
            std::vector<Row> r;
            switch (page)
            {
            case Graphics:
            {
                if (!graphicsOpen())
                {
                    r.push_back({"Not available in this run.", "", false, false, true});
                    break;
                }
                const video::Shared shared = g_access.videoShared();
                const char *labels[GraphicsRows] = {"Display", "Resolution", "Render Scale", "Anti-Aliasing",
                                                    "Texture Filter", "Frame Rate", "Widescreen"};
                for (int i = 0; i < GraphicsRows; ++i)
                {
                    Row row{labels[i], graphicsValue(i, g_menu.graphics, shared), true, true};
                    if (i == Resolution && g_menu.graphics.mode == video::WindowMode::Borderless)
                        row.arrows = false, row.valueColour = kGrey; // the monitor's own size
                    else if (!sameAs(i, g_menu.graphics, shared.applied))
                        row.valueColour = kOrange; // chosen, not applied yet
                    r.push_back(row);
                }
                break;
            }
            case Cheats:
                for (int i = 0; i < 5; ++i)
                    r.push_back({kCheatNames[i], cheats::enabled(kCheats[i]) ? "ON" : "OFF", true, true});
                if (cheats::kWarpCount)
                {
                    const cheats::Warp &w = cheats::kWarps[g_menu.warp % cheats::kWarpCount];
                    r.push_back({"Warp To", number("Room %d", w.stage) + number("-%d", w.room), true, true});
                    r.push_back({"Warp", g_menu.warpAsked ? "Next in play" : "", true, false});
                    r.back().valueColour = kYellow;
                }
                break;
            case States:
            {
                if (!g_access.slotSummary || g_access.slots <= 0)
                {
                    r.push_back({"Not available in this run.", "", false, false, true});
                    break;
                }
                std::string summary = g_access.slotSummary(g_menu.slot);
                const size_t colon = summary.find(": ");
                if (colon != std::string::npos)
                    summary = summary.substr(colon + 2);
                if (!summary.empty())
                    summary[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(summary[0])));
                const bool empty = summary == "Empty";
                r.push_back({"Slot", number("%d", g_menu.slot), true, true});
                r.push_back({summary, "", false, false, true});
                r.push_back({"Save State", g_menu.stateDone[0] == 'S' ? g_menu.stateDone : "", true, false});
                r.back().valueColour = kYellow;
                r.push_back({"Load State", empty ? "" : (g_menu.stateDone[0] == 'L' ? g_menu.stateDone : ""), true, false,
                             empty});
                r.back().valueColour = kYellow;
                break;
            }
            case Quit:
                r.push_back({"Close the game and its window?", "", false});
                r.push_back({"Unsaved progress will be lost.", "", false, false, true});
                r.push_back({"", "", false});
                r.push_back({"Cancel", ""});
                r.push_back({"Close Game", ""});
                break;
            }
            return r;
        }

        const char *actionButton(int page)
        {
            if (page == Graphics && graphicsOpen())
                return g_menu.applied ? "APPLIED" : "APPLY";
            return nullptr;
        }

        // ---- Input -----------------------------------------------------------------------------------------------

        void openPage(int page)
        {
            g_menu.page = page;
            if (page == Graphics && graphicsOpen())
            {
                g_menu.graphics = g_access.videoShared().applied;
                g_menu.applied = false;
            }
            g_menu.warpAsked = false;
            g_menu.stateDone = "";
            int &cursor = g_menu.cursor[page];
            const std::vector<Row> r = rows(page);
            if (page == Quit)
                cursor = QuitCancel; // Cancel first
            else if (cursor >= static_cast<int>(r.size()) || (cursor < kRows && !r[cursor].selectable))
            {
                cursor = kBack;
                for (int i = 0; i < static_cast<int>(r.size()); ++i)
                    if (r[i].selectable)
                    {
                        cursor = i;
                        break;
                    }
            }
        }

        // UP / DOWN through the selectable rows then the buttons, round.
        void moveCursor(int way)
        {
            const std::vector<Row> r = rows(g_menu.page);
            std::vector<int> stops;
            for (int i = 0; i < static_cast<int>(r.size()) && i < kRows; ++i)
                if (r[i].selectable)
                    stops.push_back(i);
            int &cursor = g_menu.cursor[g_menu.page];
            const int at = cursor >= kBack ? static_cast<int>(stops.size())
                                           : static_cast<int>(std::find(stops.begin(), stops.end(), cursor) - stops.begin());
            const int n = static_cast<int>(stops.size()) + 1;
            const int next = ((at + way) % n + n) % n;
            cursor = next == static_cast<int>(stops.size()) ? kBack : stops[next];
        }

        // LEFT / RIGHT on a Graphics row.
        void stepGraphics(int row, int way)
        {
            const video::Shared shared = g_access.videoShared();
            video::Settings &g = g_menu.graphics;
            switch (row)
            {
            case Display:
                g.mode = static_cast<video::WindowMode>((static_cast<int>(g.mode) + 3 + way) % 3);
                break;
            case Resolution:
            {
                const auto &sizes = shared.resolutions;
                if (g.mode == video::WindowMode::Borderless || sizes.empty())
                    return;
                const int n = static_cast<int>(sizes.size());
                int at = -1;
                for (int i = 0; i < n; ++i)
                    if (sizes[i] == std::pair{g.width, g.height})
                        at = i;
                at = at < 0 ? (way > 0 ? 0 : n - 1) : std::clamp(at + way, 0, n - 1);
                g.width = sizes[at].first, g.height = sizes[at].second;
                break;
            }
            case RenderScale:
                g.renderScale = kScales[std::clamp(nearest(kScales, 5, g.renderScale) + way, 0, 4)];
                break;
            case AntiAliasing:
                g.antiAliasing = kSamples[std::clamp(indexOf(kSamples, 4, g.antiAliasing) + way, 0, 3)];
                break;
            case TextureFilter:
                g.textureSmoothing = kSmoothing[std::clamp(indexOf(kSmoothing, 6, g.textureSmoothing) + way, 0, 5)];
                break;
            case FrameRate:
            {
                std::vector<int> rates = {0, -1};
                if (shared.applied.frameRate > 0)
                    rates.push_back(shared.applied.frameRate);
                const int n = static_cast<int>(rates.size());
                int at = 0;
                for (int i = 0; i < n; ++i)
                    if (rates[i] == g.frameRate)
                        at = i;
                g.frameRate = rates[((at + way) % n + n) % n];
                break;
            }
            case Widescreen: // applied at once, kept in onimusha.ini
                oni::display::setWidescreen(!oni::display::widescreen());
                oni::display::save();
                return;
            }
            g_menu.applied = false;
        }

        // A page's buttons; returns the sound to play (0 none).
        uint32_t pageInput(uint16_t trg, uint16_t rep)
        {
            const int page = g_menu.page;
            int &cursor = g_menu.cursor[page];
            if (rep & (kUp | kDown))
            {
                moveCursor((rep & kUp) ? -1 : 1);
                return kSeCursor;
            }
            if (cursor >= kBack)
            {
                if ((rep & (kLeft | kRight)) && actionButton(page))
                {
                    cursor = cursor == kBack ? kAction : kBack;
                    return kSeCursor;
                }
                if (!(trg & kCross))
                    return 0;
                if (cursor == kBack)
                {
                    g_menu.page = Options;
                    return kSeBack;
                }
                // APPLY: the window takes the choices at its next frame and keeps them in video.ini. Borderless keeps
                // the window's size to come back to.
                video::Settings next = g_menu.graphics;
                const video::Shared shared = g_access.videoShared();
                if (next.mode == video::WindowMode::Borderless)
                    next.width = shared.applied.width, next.height = shared.applied.height;
                g_access.videoPost(next, true);
                g_menu.graphics = next;
                g_menu.applied = true;
                return kSeDefault;
            }
            const int way = (rep & kRight) ? 1 : (rep & kLeft) ? -1 : 0;
            switch (page)
            {
            case Graphics:
                if (way && graphicsOpen())
                {
                    stepGraphics(cursor, way);
                    return kSeCursor;
                }
                return 0;
            case Cheats:
                if (cursor < 5 && (way || (trg & kCross)))
                {
                    cheats::setEnabled(kCheats[cursor], !cheats::enabled(kCheats[cursor])); // as Options' ON / OFF rows
                    return kSeCursor;
                }
                if (cursor == kWarpRow && way && cheats::kWarpCount)
                {
                    const size_t n = cheats::kWarpCount;
                    g_menu.warp = (g_menu.warp + n + static_cast<size_t>(way + static_cast<int>(n))) % n;
                    g_menu.warpAsked = false;
                    return kSeCursor;
                }
                if (cursor == kWarpGo && (trg & kCross) && cheats::kWarpCount)
                {
                    cheats::requestWarp(g_menu.warp % cheats::kWarpCount);
                    g_menu.warpAsked = true;
                    return kSeDecide;
                }
                return 0;
            case States:
                if (cursor == Slot && way && g_access.slots > 0)
                {
                    g_menu.slot = (g_menu.slot + g_access.slots + way) % g_access.slots;
                    g_menu.stateDone = "";
                    return kSeCursor;
                }
                if (cursor == SaveRow && (trg & kCross) && g_access.saveSlot)
                {
                    g_access.saveSlot(g_menu.slot);
                    g_menu.stateDone = "Saved";
                    return kSeDecide;
                }
                if (cursor == LoadRow && (trg & kCross) && g_access.loadSlot && !rows(States)[LoadRow].dim)
                {
                    g_menu.stateDone = "Loading";
                    g_access.loadSlot(g_menu.slot);
                    return kSeDecide;
                }
                return 0;
            case Quit:
                if (!(trg & kCross))
                    return 0;
                if (cursor == QuitConfirm)
                {
                    g_quit.store(true, std::memory_order_relaxed);
                    return kSeDecide;
                }
                g_menu.page = Options;
                return kSeBack;
            }
            return 0;
        }

        // ---- Drawing ---------------------------------------------------------------------------------------------

        struct Glyph
        {
            float u, v, width;
        };
        // A character's place in the font (ASCII from ' ' in rows of 16 from v 32, 16 x 16 cells) and its width.
        bool glyph(const uint8_t *ram, unsigned char c, Glyph &g)
        {
            if (c < 32 || c >= 32 + 130)
                return false;
            const unsigned i = c - 32u;
            const int8_t left = static_cast<int8_t>(ram[kPropTbl + 2 * i]), width = static_cast<int8_t>(ram[kPropTbl + 2 * i + 1]);
            g.u = static_cast<float>((i % 16) * 16 + left);
            g.v = static_cast<float>(32 + (i / 16) * 16);
            g.width = static_cast<float>(std::max<int>(width, 1));
            if (c == '-') // the font's is a long dash (its whole cell): a hyphen's middle part
                g.u += 4, g.width = 8;
            return true;
        }
        // As Mess_print: each glyph 1.5 times its texels (rounded down), 24 high, side by side.
        float textWidth(const uint8_t *ram, const std::string &s)
        {
            float w = 0;
            for (unsigned char c : s)
                if (Glyph g; glyph(ram, c, g))
                    w += std::floor(g.width * 1.5f);
            return w;
        }
        void text(std::vector<sprite::Sprite> &out, const uint8_t *ram, const std::string &s, float x, float y, uint32_t rgba,
                  float squeeze = 1)
        {
            for (unsigned char c : s)
            {
                Glyph g;
                if (!glyph(ram, c, g))
                    continue;
                const float w = std::floor(g.width * 1.5f) * squeeze;
                if (c != ' ')
                    out.push_back({kTextAttr, x, y, x + w, y + 24, g.u, g.v, g.u + g.width, g.v + 16, rgba, kFont});
                x += w;
            }
        }
        void textCentred(std::vector<sprite::Sprite> &out, const uint8_t *ram, const std::string &s, float centre, float y,
                         uint32_t rgba)
        {
            text(out, ram, s, std::floor(centre - textWidth(ram, s) / 2), y, rgba);
        }
        void part(std::vector<sprite::Sprite> &out, uint64_t tex0, float x, float y, float u0, float v0, float u1, float v1)
        {
            out.push_back({kPartAttr, x, y, x + (u1 - u0), y + (v1 - v0), u0, v0, u1, v1, 0x80808080u, tex0});
        }
        void orb(std::vector<sprite::Sprite> &out, float x, float y, bool selected)
        {
            part(out, kOrbs, x, y, selected ? 0.f : 18.f, 0, selected ? 18.f : 36.f, 23);
        }
        void bar(std::vector<sprite::Sprite> &out, float x0, float y0, float x1, float y1)
        {
            out.push_back({kBarAttr, x0, y0, x1, y1, 0, 0, 0, 0, kBarColour, 0});
        }

        // The title plate's text, with the L1 / R1 glyphs either side of the plate.
        void title(std::vector<sprite::Sprite> &out, const uint8_t *ram, bool withName)
        {
            if (withName) // narrowed to fit between the plate's ornaments
            {
                const std::string name = kTitles[g_menu.page];
                const float width = textWidth(ram, name), squeeze = std::min(1.0f, 124 / width);
                text(out, ram, name, std::floor(312 - width * squeeze / 2), 18, kWhite, squeeze);
            }
            text(out, ram, std::string(1, kGlyphL1), 168, 18, kWhite);
            text(out, ram, std::string(1, kGlyphR1), 442, 18, kWhite);
        }

        std::vector<sprite::Sprite> drawPage(const uint8_t *ram)
        {
            std::vector<sprite::Sprite> out;
            const int page = g_menu.page, cursor = g_menu.cursor[page];
            const std::vector<Row> r = rows(page);
            title(out, ram, true);
            for (int i = 0; i < static_cast<int>(r.size()) && i < kRows; ++i)
            {
                const Row &row = r[i];
                const float top = kRowTop + kRowPitch * static_cast<float>(i);
                const bool here = row.selectable && i == cursor;
                if (here)
                    bar(out, kRowLeft, top, kRowRight, top + 28);
                if (row.selectable)
                    orb(out, 95, top + 4, here);
                text(out, ram, row.label, row.selectable ? 120 : 100, top + 1, row.dim ? kGrey : kWhite);
                if (row.arrows)
                {
                    part(out, kParts, 363, top + 6, here ? 53.f : 38.f, 0, here ? 67.f : 52.f, 22);
                    part(out, kParts, 527, top + 6, here ? 68.f : 83.f, 0, here ? 82.f : 97.f, 22);
                }
                if (!row.value.empty())
                    textCentred(out, ram, row.value, 452, top + 1, row.valueColour);
            }
            // The buttons: BACK, and the page's own (APPLY).
            const float y = kButtonTop;
            if (cursor == kBack)
                bar(out, kLeftButton[0], y, kLeftButton[1], y + 28);
            orb(out, 95, y + 4, cursor == kBack);
            text(out, ram, "BACK", 120, y + 1, kWhite);
            if (const char *action = actionButton(page))
            {
                if (cursor == kAction)
                    bar(out, kRightButton[0], y, kRightButton[1], y + 28);
                orb(out, 341, y + 4, cursor == kAction);
                text(out, ram, action, 366, y + 1, g_menu.applied ? kYellow : kWhite);
            }
            return out;
        }

        // ---- The task's routines ---------------------------------------------------------------------------------

        bool customPage() { return g_menu.page != Options; }

        // A guest call as the override's last act (its return goes to the override's caller): Snd_secall(id, 0, 0).
        void tailSound(uint8_t *ram, R5900Context *ctx, Runtime *rt, uint32_t id)
        {
            ctx->r[4].ud[0] = id;
            ctx->r[5].ud[0] = 0;
            ctx->r[6].ud[0] = 0;
            Snd_secall_0x192c10(ram, ctx, rt);
        }

        void syouInit(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            g_menu.page = Options;
            syou_init_0x1b4a00(ram, ctx, rt);
        }

        void syouMove(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint16_t trg = read16(ram, kTrg), rep = read16(ram, kRep);
            const bool fading = ram[kFade] == 1;
            uint32_t se = 0;
            if (!fading && (trg & (kL1 | kR1)))
            {
                openPage((g_menu.page + ((trg & kR1) ? 1 : PageCount - 1)) % PageCount);
                se = kSeCursor;
            }
            else if (!customPage())
            {
                syou_move_0x1b4ac0(ram, ctx, rt); // the game's own list
                return;
            }
            else if (!fading && (trg & kTriangle))
            {
                g_menu.page = Options;
                se = kSeBack;
            }
            else if (!fading)
                se = pageInput(trg, rep);
            if (se)
                tailSound(ram, ctx, rt, se);
            else
                ctx->pc = ctx->r[31].uw[0];
        }

        void spriteDisp(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            if (!customPage())
            {
                // Options' own, and the L1 / R1 that lead to the port's pages.
                std::vector<sprite::Sprite> hint;
                title(hint, ram, false);
                sprite::overlay(std::move(hint));
                sprite_disp_0x1b5330(ram, ctx, rt);
                return;
            }
            sprite::overlay(drawPage(ram));
            ctx->pc = ctx->r[31].uw[0];
        }

        void messDisp(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            if (!customPage())
            {
                mess_disp_0x1b57e0(ram, ctx, rt);
                return;
            }
            ctx->pc = ctx->r[31].uw[0]; // the page's text is in its sprites
        }
    } // namespace

    void setAccess(Access access) { g_access = std::move(access); }

    void install(bozo::port::Registry &r)
    {
        r.registerFunction(0x1b4a00, syouInit);   // syou_init
        r.registerFunction(0x1b4ac0, syouMove);   // syou_move
        r.registerFunction(0x1b5330, spriteDisp); // sprite_disp
        r.registerFunction(0x1b57e0, messDisp);   // mess_disp
    }

    bool takeQuitRequest() { return g_quit.exchange(false, std::memory_order_relaxed); }

    void stateLoaded()
    {
        g_menu.page = Options;
        g_quit.store(false, std::memory_order_relaxed);
    }
} // namespace oni::menu
