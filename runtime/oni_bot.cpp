// The story bot (oni_bot.h). Everything it reads is the game's own state, named by its DWARF: the player's _OCW at
// 0x287ed0 (Pos_x / Pos_y / Pos_z floats at +0x70, Life +0x6d0), the work block G 0x27de30 (mode +0x44b0: 2 in play,
// 3 a door; the enemies' list pEm_top +0x45bc .. pEm_end +0x45cc, linked by pNext +0x30, each an _OCW-shaped
// _ENEMY_WORK) and the place (stage, room, cut: 0x2875e0).
//
// Steering: the game's tank controls on the D-pad (the stick is not read: the pad stays digital). LEFT / RIGHT turn
// Dir_y (+0xc2), UP walks where it faces; the bot turns toward its goal and walks once roughly facing it, and slides
// sideways for a while when it walks into something.
#include "oni_bot.h"

#include "bozo/host/free_host.h"
#include "bozo/libs/pad.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace oni::bot
{
    namespace
    {
        constexpr uint32_t kG = 0x27de30, kMode = kG + 0x44b0, kEnemyTop = kG + 0x45bc, kEnemyEnd = kG + 0x45cc;
        constexpr uint32_t kPlayer = 0x287ed0, kPlace = 0x2875e0;
        constexpr float kPi = 3.14159265f;

        struct Step
        {
            std::string op;
            std::vector<std::string> args;
            int line = 0;
        };

        // What the controller reports (written on the game thread at the frame boundary, read by libpad).
        std::atomic<uint16_t> g_buttons{0};
        std::atomic<uint16_t> g_move{0}; // the steering's buttons (the step's own are OR'd over them)

        class BotPad final : public bozo::libs::PadInput
        {
        public:
            void read(uint32_t, int player, bozo::libs::PadInputState &out) override
            {
                if (player != 0)
                    return;
                out.connected = true;
                out.buttons = g_buttons.load(std::memory_order_relaxed) | g_move.load(std::memory_order_relaxed);
                for (int k = 0; k < 12; ++k)
                    out.pressure[k] = (out.buttons & bozo::libs::kPadPressureOrder[k]) ? 255 : 0;
            }
        };

        BotPad g_pad;
        bozo::host::FreeHost *g_host = nullptr;
        std::vector<Step> g_steps;
        size_t g_step = 0;
        uint64_t g_stepStart = 0, g_frame = 0;
        bool g_stepBegun = false, g_finished = false, g_stopped = false;
        int g_counter = 0; // a step's own counter

        bool g_stickOn = false; // walking (the stuck detection)
        float g_lastX = 0, g_lastZ = 0;
        int g_stuck = 0;

        uint16_t button(const std::string &name)
        {
            using namespace bozo::libs;
            static const std::pair<const char *, uint16_t> names[] = {
                {"CROSS", kPadCross}, {"CIRCLE", kPadCircle}, {"SQUARE", kPadSquare}, {"TRIANGLE", kPadTriangle},
                {"L1", kPadL1}, {"R1", kPadR1}, {"L2", kPadL2}, {"R2", kPadR2}, {"START", kPadStart},
                {"SELECT", kPadSelect}, {"UP", kPadUp}, {"DOWN", kPadDown}, {"LEFT", kPadLeft}, {"RIGHT", kPadRight}};
            for (const auto &[n, b] : names)
                if (name == n)
                    return b;
            return 0;
        }

        float wrap(float a)
        {
            while (a > kPi)
                a -= 2 * kPi;
            while (a < -kPi)
                a += 2 * kPi;
            return a;
        }

        int16_t read16(const uint8_t *ram, uint32_t a)
        {
            int16_t v;
            std::memcpy(&v, ram + a, 2);
            return v;
        }
        float readf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + a, 4);
            return v;
        }

        void log(const char *fmt, const std::string &text)
        {
            std::printf("[oni:bot] frame %llu: ", (unsigned long long)g_frame);
            std::printf(fmt, text.c_str());
            std::printf("\n");
            std::fflush(stdout);
        }

        void halt()
        {
            g_move = 0;
            g_stickOn = false;
        }

        // Steers toward (x, z) with the D-pad (the game's tank controls: LEFT turns Dir_y up, RIGHT down, about 45
        // units of 4096 a frame; UP walks where Dir_y faces: x by its sine, z by its cosine). Returns the distance.
        float steer(const uint8_t *ram, float x, float z, bool walk = true)
        {
            using namespace bozo::libs;
            const float px = readf(ram, kPlayer + 0x70), pz = readf(ram, kPlayer + 0x78);
            const float dx = x - px, dz = z - pz, dist = std::sqrt(dx * dx + dz * dz);
            float want = std::atan2(dx, dz);
            if (g_stuck > 45) // pressed against something: slide along it for a while
                want += (g_stuck / 90 % 2 ? 1.0f : -1.0f) * kPi / 2.0f;
            const float facing = read16(ram, kPlayer + 0xc2) * (2 * kPi / 4096.0f);
            const float err = wrap(want - facing);
            uint16_t b = 0;
            if (err > 0.035f)
                b |= kPadLeft;
            else if (err < -0.035f)
                b |= kPadRight;
            if (walk && std::fabs(err) < 0.8f)
                b |= kPadUp;
            g_move = b;
            g_stickOn = (b & kPadUp) != 0;
            return dist;
        }

        // The way to a goal: the room's route graph (G.pRdt_route +0x488c, the enemies' own: 0x30-byte nodes, the
        // position and radius as floats, the node's index, its links' count and the linked indices as shorts), from
        // the node nearest the player to the one nearest the goal (shortest by distance), then the goal itself.
        struct Node
        {
            float x, z, r;
            std::vector<int> links;
        };
        std::vector<Node> g_nodes;
        std::vector<int> g_path; // nodes still to pass
        float g_goalX = 1e30f, g_goalZ = 1e30f;

        void loadRoute(const uint8_t *ram)
        {
            g_nodes.clear();
            uint32_t at;
            std::memcpy(&at, ram + kG + 0x488c, 4);
            at &= 0x1ffffffu;
            for (int k = 0; at && at + 0x30 <= 0x2000000u && k < 256; ++k, at += 0x30)
            {
                uint16_t id, count;
                std::memcpy(&id, ram + at + 16, 2);
                std::memcpy(&count, ram + at + 18, 2);
                if (id != k || count > 14)
                    break;
                Node n{readf(ram, at), readf(ram, at + 8), readf(ram, at + 12), {}};
                for (int l = 0; l < count; ++l)
                {
                    uint16_t to;
                    std::memcpy(&to, ram + at + 20 + 2 * l, 2);
                    n.links.push_back(to);
                }
                g_nodes.push_back(std::move(n));
            }
        }

        int nearestNode(float x, float z)
        {
            int best = -1;
            float bd = 0;
            for (size_t k = 0; k < g_nodes.size(); ++k)
            {
                const float d = std::hypot(g_nodes[k].x - x, g_nodes[k].z - z);
                if (best < 0 || d < bd)
                    best = static_cast<int>(k), bd = d;
            }
            return best;
        }

        void plan(const uint8_t *ram, float x, float z)
        {
            g_goalX = x;
            g_goalZ = z;
            g_path.clear();
            loadRoute(ram);
            const float px = readf(ram, kPlayer + 0x70), pz = readf(ram, kPlayer + 0x78);
            const int from = nearestNode(px, pz), to = nearestNode(x, z);
            if (from < 0 || from == to || std::hypot(x - px, z - pz) < 1500.0f)
                return;
            std::vector<float> dist(g_nodes.size(), 1e30f);
            std::vector<int> prev(g_nodes.size(), -1);
            std::vector<bool> done(g_nodes.size(), false);
            dist[from] = 0;
            for (;;)
            {
                int u = -1;
                for (size_t k = 0; k < g_nodes.size(); ++k)
                    if (!done[k] && dist[k] < 1e29f && (u < 0 || dist[k] < dist[u]))
                        u = static_cast<int>(k);
                if (u < 0 || u == to)
                    break;
                done[u] = true;
                for (int v : g_nodes[u].links)
                    if (v < static_cast<int>(g_nodes.size()))
                    {
                        const float w = dist[u] + std::hypot(g_nodes[u].x - g_nodes[v].x, g_nodes[u].z - g_nodes[v].z);
                        if (w < dist[v])
                            dist[v] = w, prev[v] = u;
                    }
            }
            if (prev[to] < 0)
                return; // not connected: straight on
            for (int v = to; v >= 0; v = prev[v])
                g_path.insert(g_path.begin(), v);
            std::string text = "route";
            for (int v : g_path)
                text += " " + std::to_string(v);
            std::printf("[oni:bot] %s\n", text.c_str());
        }

        // Steers along the planned way to (x, z) (planned again when the goal changes); returns the distance to it.
        float navigate(const uint8_t *ram, float x, float z, bool walk = true)
        {
            if (std::hypot(x - g_goalX, z - g_goalZ) > 500.0f) // (a moving goal, an enemy: planned again when it has gone far)
                plan(ram, x, z);
            const float px = readf(ram, kPlayer + 0x70), pz = readf(ram, kPlayer + 0x78);
            while (!g_path.empty())
            {
                const Node &n = g_nodes[g_path.front()];
                const float reach = std::max(250.0f, n.r * 0.7f);
                // Passed: within reach of it, or the next node (or the goal, last) is nearer than it is.
                const float toNode = std::hypot(n.x - px, n.z - pz);
                const float nx = g_path.size() > 1 ? g_nodes[g_path[1]].x : x, nz = g_path.size() > 1 ? g_nodes[g_path[1]].z : z;
                if (toNode < reach || std::hypot(nx - px, nz - pz) + 200.0f < std::hypot(nx - n.x, nz - n.z) * 0.6f)
                    g_path.erase(g_path.begin());
                else
                    break;
            }
            const float dist = std::hypot(x - px, z - pz);
            if (!g_path.empty())
                steer(ram, g_nodes[g_path.front()].x, g_nodes[g_path.front()].z, walk);
            else
                steer(ram, x, z, walk);
            return dist;
        }

        struct Enemy
        {
            float x, z, dist;
        };
        bool nearestEnemy(const uint8_t *ram, Enemy &out)
        {
            uint32_t at, end;
            std::memcpy(&at, ram + kEnemyTop, 4);
            std::memcpy(&end, ram + kEnemyEnd, 4);
            at &= 0x1ffffffu;
            end &= 0x1ffffffu;
            const float px = readf(ram, kPlayer + 0x70), pz = readf(ram, kPlayer + 0x78);
            bool found = false;
            for (int guard = 0; at && at + 0x7e0 <= 0x2000000u && guard < 64; ++guard)
            {
                if ((ram[at] & 1u) && read16(ram, at + 0x6d0) > 0)
                {
                    const float ex = readf(ram, at + 0x70), ez = readf(ram, at + 0x78);
                    const float d = std::hypot(ex - px, ez - pz);
                    if (!found || d < out.dist)
                        out = {ex, ez, d}, found = true;
                }
                if (at == end)
                    break;
                std::memcpy(&at, ram + at + 0x30, 4);
                at &= 0x1ffffffu;
            }
            return found;
        }

        // An enemy within reach while walking somewhere: it is fought first (turned to, attacked). True while one is.
        bool g_engaged = false;
        int g_target = -1;       // take: the AOT slot
        uint32_t g_targetAt = 0; // and the AOT it held
        bool g_windowSeen = false; // the item window opened
        int g_atDoor = 0;          // door: frames left standing at it
        bool engage(const uint8_t *ram)
        {
            Enemy e{};
            if (!nearestEnemy(ram, e) || e.dist > (g_engaged ? 2200.0f : 1500.0f))
            {
                if (g_engaged)
                    g_buttons = 0;
                g_engaged = false;
                return false;
            }
            g_engaged = true;
            steer(ram, e.x, e.z, e.dist > 650.0f);
            g_buttons = (g_frame % 10) < 4 ? bozo::libs::kPadSquare : 0;
            return true;
        }

        const Door *findDoor(int stage, int room, int toStage, int toRoom)
        {
            const Door *best = nullptr;
            for (size_t i = 0; i < kDoorCount; ++i)
            {
                const Door &d = kDoors[i];
                if (d.stage != stage || d.room != room || d.toStage != toStage || d.toRoom != toRoom)
                    continue;
                if (d.w > 8000 || d.d > 8000) // (a byte pattern in the room file's data, not a door)
                    continue;
                if (!best || static_cast<uint32_t>(d.w) * d.d < static_cast<uint32_t>(best->w) * best->d)
                    best = &d;
            }
            return best;
        }

        int arg(const Step &s, size_t i, int fallback)
        {
            return i < s.args.size() ? std::atoi(s.args[i].c_str()) : fallback;
        }

        void next()
        {
            ++g_step;
            g_stepBegun = false;
            g_goalX = g_goalZ = 1e30f; // the next step plans its own way
            g_atDoor = 0;
            g_counter = 0;
            g_buttons = 0;
            halt();
            g_stuck = 0;
        }

        void fail(const std::string &why)
        {
            log("STOPPED: %s", why);
            g_stopped = true;
            g_buttons = 0;
            halt();
        }
    } // namespace

    bool start(bozo::host::FreeHost &host)
    {
        const char *path = std::getenv("ONI_BOT");
        if (!path || !*path)
            return false;
        std::ifstream in(path);
        if (!in)
        {
            std::fprintf(stderr, "[oni:bot] cannot read %s\n", path);
            return false;
        }
        std::string line;
        int n = 0;
        while (std::getline(in, line))
        {
            ++n;
            if (const size_t hash = line.find('#'); hash != std::string::npos)
                line.erase(hash);
            std::istringstream words(line);
            Step s;
            s.line = n;
            if (!(words >> s.op))
                continue;
            for (std::string w; words >> w;)
                s.args.push_back(w);
            g_steps.push_back(std::move(s));
        }
        g_host = &host;
        host.libraries().pad.input = &g_pad;
        std::printf("[oni:bot] %zu steps from %s\n", g_steps.size(), path);
        return true;
    }

    bool finished() { return g_finished; }

    void frame(uint8_t *ram, uint64_t frameNumber)
    {
        g_frame = frameNumber;
        if (!g_host || g_stopped || g_finished)
            return;
        // Stuck: walking and not moving.
        const float px = readf(ram, kPlayer + 0x70), pz = readf(ram, kPlayer + 0x78);
        const float speed = std::hypot(px - g_lastX, pz - g_lastZ);
        g_lastX = px;
        g_lastZ = pz;
        if (g_stickOn)
            g_stuck = speed < 3.0f ? g_stuck + 1 : std::max(0, g_stuck - 4);
        else
            g_stuck = 0;
        if (g_step >= g_steps.size())
        {
            log("%s", "script done");
            g_finished = true;
            g_buttons = 0;
            halt();
            return;
        }
        const Step &s = g_steps[g_step];
        if (!g_stepBegun)
        {
            g_stepBegun = true;
            g_stepStart = frameNumber;
            std::string text = s.op;
            for (const std::string &a : s.args)
                text += " " + a;
            log("%s", "step " + std::to_string(s.line) + ": " + text);
        }
        const uint64_t age = frameNumber - g_stepStart;
        const int stage = read16(ram, kPlace), room = read16(ram, kPlace + 2);
        const uint8_t mode = ram[kMode];
        const bool inPlay = mode == 2;

        if (s.op == "go")
        {
            const float tx = static_cast<float>(arg(s, 0, 0)), tz = static_cast<float>(arg(s, 1, 0));
            const float radius = static_cast<float>(arg(s, 2, 300));
            if (!inPlay)
                halt();
            else if (engage(ram))
                ;
            else if (navigate(ram, tx, tz) < radius)
            {
                log("%s", "reached (" + std::to_string(static_cast<int>(px)) + ", " + std::to_string(static_cast<int>(pz)) + ")");
                next();
                return;
            }
            if (age > static_cast<uint64_t>(arg(s, 3, 1800)))
                fail("go: not reached from (" + std::to_string(static_cast<int>(px)) + ", " + std::to_string(static_cast<int>(pz)) + ")");
        }
        else if (s.op == "door")
        {
            const int ts = arg(s, 0, 0), tr = arg(s, 1, 0);
            if (stage == ts && room == tr && inPlay)
            {
                log("%s", "in " + std::to_string(ts) + ":" + std::to_string(tr));
                next();
                return;
            }
            const Door *d = findDoor(stage, room, ts, tr);
            if (!d && (stage != ts || room != tr) && inPlay)
            {
                fail("door: room " + std::to_string(stage) + ":" + std::to_string(room) + " has no door to " + std::to_string(ts) + ":" + std::to_string(tr));
                return;
            }
            if (d && inPlay && engage(ram))
                ;
            else if (d && inPlay)
            {
                const float cx = d->x + d->w * 0.5f, cz = d->z + d->d * 0.5f;
                // At the door (its AOT, or within a margin of it: a door is a wall): facing its middle, the action
                // button every 20 frames; a door that opens by walking in takes the player at once.
                const bool inside = px >= d->x && px <= d->x + d->w && pz >= d->z && pz <= d->z + d->d;
                const bool near = px >= d->x - 300 && px <= d->x + d->w + 300 && pz >= d->z - 300 && pz <= d->z + d->d + 300;
                // Near it: stop, turn to face its middle squarely, then press (the game's action point is 600
                // ahead of the player: it must fall in the door's AOT). Not there after a while: a few steps closer.
                if (near && g_atDoor == 0)
                    g_atDoor = 300;
                if (g_atDoor > 60)
                {
                    --g_atDoor;
                    const float facing = read16(ram, kPlayer + 0xc2) * (2 * kPi / 4096.0f);
                    if (std::fabs(wrap(std::atan2(cx - px, cz - pz) - facing)) > 0.05f)
                    {
                        steer(ram, cx, cz, false);
                        g_buttons = 0;
                    }
                    else
                    {
                        halt();
                        g_buttons = (++g_counter % 30) < 8 ? bozo::libs::kPadCross : 0;
                    }
                }
                else if (g_atDoor > 0)
                {
                    if (--g_atDoor == 0)
                        g_atDoor = -90; // walk a little before stopping again
                    navigate(ram, cx, cz);
                    g_buttons = 0;
                }
                else if (g_atDoor < 0)
                {
                    if (++g_atDoor == 0 && !near)
                        g_atDoor = 0;
                    navigate(ram, cx, cz);
                    g_buttons = 0;
                }
                else
                {
                    navigate(ram, cx, cz);
                    g_buttons = inside && (++g_counter % 20) < 6 ? bozo::libs::kPadCross : 0;
                }
            }
            else
            {
                halt();
                g_buttons = 0;
            }
            if (age > static_cast<uint64_t>(arg(s, 2, 2400)))
                fail("door: still in " + std::to_string(stage) + ":" + std::to_string(room) + " at (" + std::to_string(static_cast<int>(px)) + ", " + std::to_string(static_cast<int>(pz)) + ")");
        }
        else if (s.op == "fight")
        {
            Enemy e{};
            if (!inPlay)
            {
                halt();
                g_buttons = 0;
            }
            else if (!nearestEnemy(ram, e))
            {
                if (++g_counter > 90) // none for a moment (a wave may follow)
                {
                    log("%s", "no enemy left");
                    next();
                    return;
                }
                halt();
                g_buttons = 0;
            }
            else
            {
                g_counter = 0;
                if (age % 300 == 0)
                    log("%s", "enemy at (" + std::to_string(static_cast<int>(e.x)) + ", " + std::to_string(static_cast<int>(e.z)) +
                                  "), the player at (" + std::to_string(static_cast<int>(px)) + ", " + std::to_string(static_cast<int>(pz)) + ")");
                const float dist = std::hypot(e.x - px, e.z - pz) > 1400.0f ? navigate(ram, e.x, e.z) : steer(ram, e.x, e.z);
                if (dist < 1400.0f)
                {
                    if (dist < 700.0f)
                        steer(ram, e.x, e.z, false); // face it, standing
                    g_buttons = (age % 10) < 4 ? bozo::libs::kPadSquare : 0;
                }
                else
                    g_buttons = 0;
            }
            if (age > static_cast<uint64_t>(arg(s, 0, 3600)))
                fail("fight: enemies still alive");
        }
        else if (s.op == "play")
        {
            halt();
            if (inPlay)
                ++g_counter;
            else
                g_counter = 0;
            g_buttons = (!inPlay && age % 40 < 4) ? bozo::libs::kPadCross : 0;
            if (g_counter > 60)
            {
                log("%s", "in play");
                next();
                return;
            }
            if (age > static_cast<uint64_t>(arg(s, 0, 3600)))
                fail("play: the game is still not in play (mode " + std::to_string(mode) + ")");
        }
        else if (s.op == "press")
        {
            const int hold = arg(s, 1, 6);
            g_buttons = static_cast<int>(age) < hold ? button(s.args.empty() ? "" : s.args[0]) : 0;
            if (static_cast<int>(age) >= 2 * hold)
                next();
        }
        else if (s.op == "wait")
        {
            if (static_cast<int>(age) >= arg(s, 0, 60))
                next();
        }
        else if (s.op == "place")
        {
            if (stage == arg(s, 0, 0) && room == arg(s, 1, 0))
            {
                next();
                return;
            }
            if (age > static_cast<uint64_t>(arg(s, 2, 3600)))
                fail("place: still in " + std::to_string(stage) + ":" + std::to_string(room));
        }
        else if (s.op == "save")
        {
            if (!s.args.empty())
                g_host->requestSave(s.args[0]);
            next();
        }
        else if (s.op == "take")
        {
            // The nearest live item AOT (Id 2), or slot N: walked into, faced, the action button pressed and the
            // message closed; done once the slot no longer holds that AOT (the item taken).
            if (!g_stepBegun || g_counter == 0)
            {
                g_target = -1;
                float bd = 0;
                for (int k = 0; k < 32; ++k)
                {
                    uint32_t at;
                    std::memcpy(&at, ram + kG + 0x97e8 + 4 * k, 4);
                    at &= 0x1ffffffu;
                    if (!at || ram[at] != 2 || (s.args.size() && k != arg(s, 0, -1)))
                        continue;
                    const float cx = read16(ram, at + 4) + static_cast<uint16_t>(read16(ram, at + 8)) * 0.5f;
                    const float cz = read16(ram, at + 6) + static_cast<uint16_t>(read16(ram, at + 10)) * 0.5f;
                    const float d = std::hypot(cx - px, cz - pz);
                    if (g_target < 0 || d < bd)
                        g_target = k, bd = d, g_targetAt = at;
                }
                g_counter = 1;
                if (g_target < 0)
                {
                    fail("take: no item AOT in the room");
                    return;
                }
                log("%s", "item slot " + std::to_string(g_target));
            }
            // Taken: the item window (fade.c's Fade_w 0x27c370, type 5) opened and closed again; or the AOT gone.
            uint32_t now;
            std::memcpy(&now, ram + kG + 0x97e8 + 4 * g_target, 4);
            if (ram[0x27c370] && ram[0x27c373] == 5)
                g_windowSeen = true;
            if (inPlay && ((g_windowSeen && !ram[0x27c370]) || (now & 0x1ffffffu) != g_targetAt))
            {
                g_windowSeen = false;
                log("%s", "taken");
                next();
                return;
            }
            const uint32_t at = g_targetAt;
            const float x0 = read16(ram, at + 4), z0 = read16(ram, at + 6);
            const float w = static_cast<uint16_t>(read16(ram, at + 8)), dd = static_cast<uint16_t>(read16(ram, at + 10));
            const float cx = x0 + w * 0.5f, cz = z0 + dd * 0.5f;
            // (The item may be solid, a box: its AOT reached within a margin.)
            const bool inside = px >= x0 - 300 && px <= x0 + w + 300 && pz >= z0 - 300 && pz <= z0 + dd + 300;
            if (!inPlay)
            {
                halt();
                g_buttons = age % 40 < 4 ? bozo::libs::kPadCross : 0; // the item's message closed
            }
            else if (engage(ram))
                ;
            else if (inside || std::hypot(cx - px, cz - pz) < 450.0f)
            {
                steer(ram, cx, cz, false); // facing it
                g_buttons = (++g_counter % 40) < 5 ? bozo::libs::kPadCross : 0;
            }
            else
            {
                navigate(ram, cx, cz);
                g_buttons = 0;
            }
            if (age > static_cast<uint64_t>(arg(s, 1, 2400)))
                fail("take: the item is still there");
        }
        else if (s.op == "face")
        {
            // Turned toward (X, Z) on the spot.
            const float tx = static_cast<float>(arg(s, 0, 0)), tz = static_cast<float>(arg(s, 1, 0));
            const float facing = read16(ram, kPlayer + 0xc2) * (2 * kPi / 4096.0f);
            if (std::fabs(wrap(std::atan2(tx - px, tz - pz) - facing)) < 0.06f || age > 240)
            {
                halt();
                next();
                return;
            }
            steer(ram, tx, tz, false);
        }
        else if (s.op == "teleport")
        {
            // A shortcut within the room (where the bot cannot find the way): the position (and the last frame's, so
            // nothing interpolates) set, the height kept; the game's own triggers then act as usual.
            const float x = static_cast<float>(arg(s, 0, 0)), z = static_cast<float>(arg(s, 1, 0));
            for (uint32_t at : {kPlayer + 0x70u, kPlayer + 0x80u})
            {
                std::memcpy(ram + at, &x, 4);
                std::memcpy(ram + at + 8, &z, 4);
            }
            if (s.args.size() > 2)
            {
                const float y = static_cast<float>(arg(s, 2, 0));
                std::memcpy(ram + kPlayer + 0x74, &y, 4);
                std::memcpy(ram + kPlayer + 0x84, &y, 4);
            }
            next();
        }
        else if (s.op == "aots")
        {
            // The room's live AOTs (G.Sce_aot[32] +0x97e8: _SCE_AOT Id, Type, nFloor, Super, X, Z, W, D; a door's
            // _IN_DOOR_WORK after it).
            for (int k = 0; k < 32; ++k)
            {
                uint32_t at;
                std::memcpy(&at, ram + kG + 0x97e8 + 4 * k, 4);
                at &= 0x1ffffffu;
                if (!at || at + 0x30 > 0x2000000u)
                    continue;
                std::printf("[oni:bot] aot %2d at %06x: id %u type %02x floor %d super %u  x %d z %d w %u d %u  data",
                            k, at, ram[at], ram[at + 1], static_cast<int8_t>(ram[at + 2]), ram[at + 3], read16(ram, at + 4),
                            read16(ram, at + 6), static_cast<uint16_t>(read16(ram, at + 8)), static_cast<uint16_t>(read16(ram, at + 10)));
                for (int b = 12; b < 32; ++b)
                    std::printf(" %02x", ram[at + b]);
                std::printf("\n");
            }
            next();
        }
        else if (s.op == "end")
        {
            log("%s", "end");
            g_finished = true;
            g_host->requestStop();
        }
        else
            fail("unknown step '" + s.op + "' at line " + std::to_string(s.line));
    }
} // namespace oni::bot
