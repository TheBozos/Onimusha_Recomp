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
#include <functional>
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

        // The way as points to pass (a route graph's nodes, or the collision grid's corners), each with its reach.
        struct Waypoint
        {
            float x, z, reach;
        };
        std::vector<Waypoint> g_wps;

        // The collision grid: the room's collision mesh (G.pRdt_sca +0x4880, its size at 0x27c638: the vertices as
        // float triples, then the triangles as index triples (u16), then an attribute byte each; the vertex and triangle
        // counts in its last 6 bytes, as Sca_data_set_sub reads them). A tall steep triangle (a wall: higher than 600,
        // so a staircase's risers are not) that reaches the player's height marks the cells along it, widened by the
        // player's radius; the way is the shortest through the free cells to the free cell nearest the goal (a door is
        // in a wall), straightened where the cells see each other.
        struct Grid
        {
            float x0 = 0, z0 = 0, cell = 100;
            int w = 0, h = 0;
            std::vector<uint8_t> blocked;
            bool free(int i, int j) const { return i >= 0 && j >= 0 && i < w && j < h && !blocked[static_cast<size_t>(j) * w + i]; }
        };

        bool buildGrid(const uint8_t *ram, float py, Grid &g, float px, float pz, float tx, float tz)
        {
            uint32_t base;
            int32_t size;
            std::memcpy(&base, ram + kG + 0x4880, 4);
            std::memcpy(&size, ram + 0x27c638, 4);
            base &= 0x1ffffffu;
            if (!base || size < 6 || base + static_cast<uint32_t>(size) > 0x2000000u)
                return false;
            uint16_t nv, nt;
            std::memcpy(&nv, ram + base + size - 6, 2);
            std::memcpy(&nt, ram + base + size - 4, 2);
            if (!nv || !nt || 12u * nv + 6u * nt > static_cast<uint32_t>(size))
                return false;
            auto vert = [&](int k, float *v) { std::memcpy(v, ram + base + 12 * k, 12); };
            float x0 = std::min(px, tx), x1 = std::max(px, tx), z0 = std::min(pz, tz), z1 = std::max(pz, tz);
            for (int k = 0; k < nv; ++k)
            {
                float v[3];
                vert(k, v);
                x0 = std::min(x0, v[0]), x1 = std::max(x1, v[0]), z0 = std::min(z0, v[2]), z1 = std::max(z1, v[2]);
            }
            g.cell = 100;
            g.x0 = x0 - 500, g.z0 = z0 - 500;
            g.w = static_cast<int>((x1 - x0 + 1000) / g.cell) + 1;
            g.h = static_cast<int>((z1 - z0 + 1000) / g.cell) + 1;
            if (g.w <= 0 || g.h <= 0 || static_cast<size_t>(g.w) * g.h > 4000000u)
                return false;
            std::vector<uint8_t> wall(static_cast<size_t>(g.w) * g.h, 0);
            auto seg2 = [](float qx, float qz, const float *a, const float *b) {
                const float vx = b[0] - a[0], vz = b[2] - a[2], wx = qx - a[0], wz = qz - a[2], l = vx * vx + vz * vz;
                const float t = l > 0 ? std::clamp((wx * vx + wz * vz) / l, 0.0f, 1.0f) : 0.0f;
                const float dx = wx - t * vx, dz = wz - t * vz;
                return dx * dx + dz * dz;
            };
            for (int k = 0; k < nt; ++k)
            {
                uint16_t idx[3];
                std::memcpy(idx, ram + base + 12 * nv + 6 * k, 6);
                if (idx[0] >= nv || idx[1] >= nv || idx[2] >= nv)
                    continue;
                float a[3], b[3], c[3];
                vert(idx[0], a), vert(idx[1], b), vert(idx[2], c);
                const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
                const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                const float n = std::sqrt(nx * nx + ny * ny + nz * nz);
                const float ylo = std::min({a[1], b[1], c[1]}), yhi = std::max({a[1], b[1], c[1]});
                if (n <= 0 || std::fabs(ny / n) > 0.5f || yhi - ylo < 600 || yhi < py + 150 || ylo > py + 1500)
                    continue;
                const int i0 = static_cast<int>((std::min({a[0], b[0], c[0]}) - g.x0) / g.cell) - 1;
                const int i1 = static_cast<int>((std::max({a[0], b[0], c[0]}) - g.x0) / g.cell) + 1;
                const int j0 = static_cast<int>((std::min({a[2], b[2], c[2]}) - g.z0) / g.cell) - 1;
                const int j1 = static_cast<int>((std::max({a[2], b[2], c[2]}) - g.z0) / g.cell) + 1;
                const float r2 = (g.cell * 0.75f) * (g.cell * 0.75f);
                for (int j = std::max(0, j0); j <= std::min(g.h - 1, j1); ++j)
                    for (int i = std::max(0, i0); i <= std::min(g.w - 1, i1); ++i)
                    {
                        const float qx = g.x0 + (i + 0.5f) * g.cell, qz = g.z0 + (j + 0.5f) * g.cell;
                        if (seg2(qx, qz, a, b) < r2 || seg2(qx, qz, b, c) < r2 || seg2(qx, qz, c, a) < r2)
                            wall[static_cast<size_t>(j) * g.w + i] = 1;
                    }
            }
            // Widened by the player's radius (two cells).
            g.blocked.assign(wall.size(), 0);
            for (int j = 0; j < g.h; ++j)
                for (int i = 0; i < g.w; ++i)
                    if (wall[static_cast<size_t>(j) * g.w + i])
                        for (int dj = -2; dj <= 2; ++dj)
                            for (int di = -2; di <= 2; ++di)
                                if (i + di >= 0 && j + dj >= 0 && i + di < g.w && j + dj < g.h)
                                    g.blocked[static_cast<size_t>(j + dj) * g.w + i + di] = 1;
            return true;
        }

        bool gridPath(const Grid &g, float px, float pz, float tx, float tz, std::vector<Waypoint> &out)
        {
            const int si = static_cast<int>((px - g.x0) / g.cell), sj = static_cast<int>((pz - g.z0) / g.cell);
            const int gi = static_cast<int>((tx - g.x0) / g.cell), gj = static_cast<int>((tz - g.z0) / g.cell);
            if (si < 0 || sj < 0 || si >= g.w || sj >= g.h)
                return false;
            const size_t n = static_cast<size_t>(g.w) * g.h;
            auto id = [&](int i, int j) { return static_cast<size_t>(j) * g.w + i; };
            // The goal: the free cell reachable from the player nearest to it.
            std::vector<uint8_t> seen(n, 0);
            std::vector<size_t> queue{id(si, sj)};
            seen[id(si, sj)] = 1;
            size_t best = id(si, sj);
            float bd = 1e30f;
            for (size_t q = 0; q < queue.size(); ++q)
            {
                const int i = static_cast<int>(queue[q] % g.w), j = static_cast<int>(queue[q] / g.w);
                const float d = std::hypot(static_cast<float>(i - gi), static_cast<float>(j - gj));
                if (d < bd)
                    bd = d, best = queue[q];
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di)
                        if ((di || dj) && g.free(i + di, j + dj) && !seen[id(i + di, j + dj)])
                            seen[id(i + di, j + dj)] = 1, queue.push_back(id(i + di, j + dj));
            }
            // Shortest way there (8 neighbours).
            std::vector<float> dist(n, 1e30f);
            std::vector<int64_t> prev(n, -1);
            using Item = std::pair<float, size_t>;
            std::vector<Item> heap;
            dist[id(si, sj)] = 0;
            heap.push_back({0, id(si, sj)});
            const int bi = static_cast<int>(best % g.w), bj = static_cast<int>(best / g.w);
            while (!heap.empty())
            {
                std::pop_heap(heap.begin(), heap.end(), std::greater<Item>());
                const size_t u = heap.back().second;
                heap.pop_back();
                if (u == best)
                    break;
                const int i = static_cast<int>(u % g.w), j = static_cast<int>(u / g.w);
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di)
                    {
                        if (!(di || dj) || !g.free(i + di, j + dj))
                            continue;
                        const size_t v = id(i + di, j + dj);
                        const float nd = dist[u] + ((di && dj) ? 1.4142f : 1.0f);
                        if (nd < dist[v])
                        {
                            dist[v] = nd, prev[v] = static_cast<int64_t>(u);
                            heap.push_back({nd + std::hypot(static_cast<float>(bi - i - di), static_cast<float>(bj - j - dj)), v});
                            std::push_heap(heap.begin(), heap.end(), std::greater<Item>());
                        }
                    }
            }
            if (best != id(si, sj) && prev[best] < 0)
                return false;
            std::vector<size_t> cells;
            for (int64_t u = static_cast<int64_t>(best); u >= 0; u = prev[static_cast<size_t>(u)])
                cells.insert(cells.begin(), static_cast<size_t>(u));
            // Straightened: from each kept cell, on to the farthest one it sees (free cells all the way).
            auto sees = [&](size_t a, size_t b) {
                const float ax = static_cast<float>(a % g.w), az = static_cast<float>(a / g.w);
                const float bx = static_cast<float>(b % g.w), bz = static_cast<float>(b / g.w);
                const int steps = static_cast<int>(std::max(std::fabs(bx - ax), std::fabs(bz - az)) * 2) + 1;
                for (int k = 0; k <= steps; ++k)
                {
                    const float t = static_cast<float>(k) / steps;
                    if (!g.free(static_cast<int>(ax + (bx - ax) * t + 0.5f), static_cast<int>(az + (bz - az) * t + 0.5f)))
                        return false;
                }
                return true;
            };
            out.clear();
            size_t at = 0;
            while (at + 1 < cells.size())
            {
                size_t next = at + 1;
                for (size_t k = cells.size() - 1; k > at + 1; --k)
                    if (sees(cells[at], cells[k]))
                    {
                        next = k;
                        break;
                    }
                out.push_back({g.x0 + (cells[next] % g.w + 0.5f) * g.cell, g.z0 + (cells[next] / g.w + 0.5f) * g.cell, 250.0f});
                at = next;
            }
            return true;
        }

        void plan(const uint8_t *ram, float x, float z)
        {
            g_goalX = x;
            g_goalZ = z;
            g_wps.clear();
            const float px = readf(ram, kPlayer + 0x70), py = readf(ram, kPlayer + 0x74), pz = readf(ram, kPlayer + 0x78);
            if (std::hypot(x - px, z - pz) < 600.0f)
                return;
            Grid grid;
            if (buildGrid(ram, py, grid, px, pz, x, z) && gridPath(grid, px, pz, x, z, g_wps))
            {
                std::printf("[oni:bot] way: %zu points on the collision grid\n", g_wps.size());
                return;
            }
            // No mesh, or no way through it: the route graph.
            loadRoute(ram);
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
            std::string text = "route";
            for (int v = to; v >= 0; v = prev[v])
            {
                g_wps.insert(g_wps.begin(), {g_nodes[v].x, g_nodes[v].z, std::max(250.0f, g_nodes[v].r * 0.7f)});
                text.insert(5, " " + std::to_string(v));
            }
            std::printf("[oni:bot] %s\n", text.c_str());
        }

        // Steers along the planned way to (x, z) (planned again when the goal moves far); returns the distance to it.
        float navigate(const uint8_t *ram, float x, float z, bool walk = true)
        {
            if (std::hypot(x - g_goalX, z - g_goalZ) > 500.0f) // (a moving goal, an enemy: planned again when it has gone far)
                plan(ram, x, z);
            const float px = readf(ram, kPlayer + 0x70), pz = readf(ram, kPlayer + 0x78);
            while (!g_wps.empty())
            {
                const Waypoint &w = g_wps.front();
                // Passed: within reach of it, or the next point (or the goal, last) is nearer than it is.
                const float nx = g_wps.size() > 1 ? g_wps[1].x : x, nz = g_wps.size() > 1 ? g_wps[1].z : z;
                if (std::hypot(w.x - px, w.z - pz) < w.reach || std::hypot(nx - px, nz - pz) + 200.0f < std::hypot(nx - w.x, nz - w.z) * 0.6f)
                    g_wps.erase(g_wps.begin());
                else
                    break;
            }
            const float dist = std::hypot(x - px, z - pz);
            if (!g_wps.empty())
                steer(ram, g_wps.front().x, g_wps.front().z, walk);
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
        int g_inPlayFor = 0;       // frames in play in a row
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

        // A live door AOT of the room to that stage and room (G.Sce_aot, Id 1: a rectangle and the door's work after
        // the 12 bytes, or with Type bit 7 four corners and the work after 20), first; else the room files' doors.
        Door g_liveDoor{};
        const Door *findDoor(const uint8_t *ram, int stage, int room, int toStage, int toRoom)
        {
            for (int k = 0; k < 32; ++k)
            {
                uint32_t at;
                std::memcpy(&at, ram + kG + 0x97e8 + 4 * k, 4);
                at &= 0x1ffffffu;
                if (!at || at + 0x30 > 0x2000000u || ram[at] != 1)
                    continue;
                const bool four = (ram[at + 1] & 0x80u) != 0;
                const uint32_t work = at + (four ? 20u : 12u);
                if (ram[work + 8] != toStage || ram[work + 9] != toRoom)
                    continue;
                Door d{static_cast<uint8_t>(stage), static_cast<uint8_t>(room), 0, 0, 0, 0, ram[work + 8], ram[work + 9], ram[work + 10], ram[work + 15]};
                if (four)
                {
                    int16_t p[8];
                    std::memcpy(p, ram + at + 4, 16);
                    const int16_t xs[4] = {p[0], p[2], p[4], p[6]}, zs[4] = {p[1], p[3], p[5], p[7]};
                    d.x = *std::min_element(xs, xs + 4), d.z = *std::min_element(zs, zs + 4);
                    d.w = static_cast<uint16_t>(*std::max_element(xs, xs + 4) - d.x);
                    d.d = static_cast<uint16_t>(*std::max_element(zs, zs + 4) - d.z);
                }
                else
                {
                    d.x = read16(ram, at + 4), d.z = read16(ram, at + 6);
                    d.w = static_cast<uint16_t>(read16(ram, at + 8)), d.d = static_cast<uint16_t>(read16(ram, at + 10));
                }
                g_liveDoor = d;
                return &g_liveDoor;
            }
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
        // In play: game_move, and nothing stopped (G.Stop_flg[0] +0x4a60: set through an event, a message, the item
        // window, a menu; for a frame or two at a cut change).
        uint32_t stopped;
        std::memcpy(&stopped, ram + kG + 0x4a60, 4);
        const bool inPlay = mode == 2 && stopped == 0;
        g_inPlayFor = inPlay ? g_inPlayFor + 1 : 0;

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
            const Door *d = findDoor(ram, stage, room, ts, tr);
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
                const bool near = px >= d->x - 450 && px <= d->x + d->w + 450 && pz >= d->z - 450 && pz <= d->z + d->d + 450;
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
        else if (s.op == "take" || s.op == "examine")
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
                    const uint8_t id = ram[at];
                    const bool wanted = s.op == "take" ? id == 2 : (id == 4 || id == 5 || id == 12);
                    if (!at || !wanted || (s.args.size() && k != arg(s, 0, -1)))
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
                    fail(s.op + ": no such AOT in the room");
                    return;
                }
                log("%s", "item slot " + std::to_string(g_target));
            }
            // Taken: the item window (fade.c's Fade_w 0x27c370, type 5) opened and closed again; or the AOT gone.
            uint32_t now;
            std::memcpy(&now, ram + kG + 0x97e8 + 4 * g_target, 4);
            // (examine: done once the event or message it starts is over: the game out of play, a fade, or the AOT
            // changed, then in play again.)
            if (ram[0x27c370] && (ram[0x27c373] == 5 || s.op == "examine"))
                g_windowSeen = true;
            if (s.op == "examine" && g_counter > 8 && (!inPlay || ram[(now & 0x1ffffffu)] != ram[g_targetAt]))
                g_windowSeen = true;
            if (inPlay && g_inPlayFor > 30 && ((g_windowSeen && !ram[0x27c370]) || (now & 0x1ffffffu) != g_targetAt))
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
            const bool inside = px >= x0 - 450 && px <= x0 + w + 450 && pz >= z0 - 450 && pz <= z0 + dd + 450;
            if (!inPlay)
            {
                halt();
                g_buttons = age % 40 < 4 ? bozo::libs::kPadCross : 0; // the item's message closed
            }
            else if (engage(ram))
                ;
            else if (inside || std::hypot(cx - px, cz - pz) < 450.0f)
            {
                // Facing it squarely first (an action while turning is not taken), then the button.
                const float facing = read16(ram, kPlayer + 0xc2) * (2 * kPi / 4096.0f);
                if (std::fabs(wrap(std::atan2(cx - px, cz - pz) - facing)) > 0.05f)
                {
                    steer(ram, cx, cz, false);
                    g_buttons = 0;
                }
                else
                {
                    halt();
                    g_buttons = (++g_counter % 40) < 8 ? bozo::libs::kPadCross : 0;
                }
            }
            else
            {
                navigate(ram, cx, cz);
                g_buttons = 0;
            }
            if (age > static_cast<uint64_t>(arg(s, 1, 2400)))
                fail("take: the item is still there");
        }
        else if (s.op == "attack")
        {
            // Turned toward (X, Z), then N swings (SQUARE): pots, crates, a rope.
            const float tx = static_cast<float>(arg(s, 0, 0)), tz = static_cast<float>(arg(s, 1, 0));
            const int swings = arg(s, 2, 4);
            const float facing = read16(ram, kPlayer + 0xc2) * (2 * kPi / 4096.0f);
            if (g_counter == 0 && std::fabs(wrap(std::atan2(tx - px, tz - pz) - facing)) > 0.05f && age < 240)
            {
                steer(ram, tx, tz, false);
                g_buttons = 0;
                return;
            }
            halt();
            const int phase = g_counter++ % 30;
            g_buttons = phase < 5 ? bozo::libs::kPadSquare : 0;
            if (g_counter >= swings * 30)
                next();
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
