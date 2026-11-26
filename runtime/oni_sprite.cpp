// Onimusha's 2D: Ot_draw(pOt, length) 0x12e7f0 walks the frame's ordering table and turns each primitive into a GIF
// packet. Natively the same walk turns each primitive into overlay triangles from its own fields; no packet is built.
//
// The table: pOt[0 .. length); a slot holds 0 (empty) or the first primitive of a chain; a primitive's word 0 (Tag) is
// the next primitive, or a pointer back into the table (the chain goes on at that slot). Each primitive:
//   +4 Attribute: bits 0-10 the GS PRIM value (kind & 7, IIP 0x8, TME 0x10, ABE 0x40, FST 0x100, CTXT 0x200);
//      0x800 coordinates already in GS units; 0x1000 TEX1 = 0 (point sampling); blending with ABE: 0x2000 (Cs - Cd) A +
//      Cd, 0x4000 (0 - Cs) A + Cd, else Cs A + Cd, A the texture's alpha with 0x8000 else the colour's A byte (FIX);
//      bits 24-31 the layout:
//   0 reset ALPHA_2; 1 one vertex; 2 two untextured vertices (RGBA, XY, Z each); 3 textured triangle (3 x RGBA, UV, XY,
//   Z from +8, TEX0 pointer at +0x40); 4 textured sprite (RGBA, UV0, XY0, Z0, UV1, XY1, Z1, TEX0 pointer at +0x24);
//   5 textured quad (4 x RGBA, UV, XY, Z, TEX0 pointer at +0x50); 6 a 3D model (Md_draw_entry(+8)); 7 another quad
//   layout (TEX0 pointer at +0x70).
// Coordinates: x 0..639, y 0..447 of the frame (the GS draws fields: Ot_draw halves y); with 0x800 the GS's own 12.4.
#include "oni_render.h"
#include "oni_vu1.h"
#include "oni_tex.h"
#include "oni_display.h"

#include "bozo/ee/dispatcher.h"
#include "oni_functions.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace oni::sprite
{
    namespace
    {
        using bozo::render::Material;
        using bozo::render::Vertex;

        uint32_t read32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x1ffffffu), 4);
            return v;
        }
        uint64_t read64(const uint8_t *ram, uint32_t a)
        {
            uint64_t v;
            std::memcpy(&v, ram + (a & 0x1ffffffu), 8);
            return v;
        }

        int trace()
        {
            static const int level = [] { const char *e = std::getenv("ONI_2D_TRACE"); return e ? std::atoi(e) : 0; }();
            return level;
        }

        // Pit_trans's own state while it draws: its screen offset (the draw origin it reads at 0x28e420, as frame lines
        // and pixels from the usual 0x6c0, 0xf20) and its fixed-alpha blend, which scales by the destination's alpha.
        bool g_pit = false;
        float g_offsetX = 0, g_offsetY = 0;
        // The port's own sprites (overlay()): drawn over the frame without its depth, blended as the game's.
        bool g_overlay = false;
        std::vector<Sprite> g_sprites;

        struct Point
        {
            float x, y, u, v;
            uint32_t rgba;
            uint32_t z = 0; // the GS's Z (XYZ2's upper word)
        };

        // A coordinate word (x low, y high) to GS screen units on the 640 x 448 frame.
        void position(uint32_t word, bool gs, float &x, float &y)
        {
            const bozo::render::Screen s = oni::render::screen();
            if (gs) // 12.4, the field's lines
            {
                x = static_cast<float>(word & 0xffffu) / 16.0f;
                y = s.originY + (static_cast<float>(word >> 16) / 16.0f - 1936.0f) * 2.0f;
                return;
            }
            x = s.originX + g_offsetX + static_cast<float>(static_cast<int16_t>(word & 0xffffu));
            y = s.originY + g_offsetY + static_cast<float>(static_cast<int16_t>(word >> 16));
        }

        Material material(uint32_t attr, uint32_t rgba, uint64_t tex0, bool textured)
        {
            Material m;
            m.textured = textured;
            // Ot_draw's environment: TEST_2 0x7000d (ZTE, ZTST GREATER) against the frame's Z buffer, which ZBUF_2 lets an
            // opaque primitive write and a blended one not (ZMSK). The 3D effects queued in the table carry their own Z (the
            // red wall sealing a fight's doorway, esp080c_lock_wall: its sides go behind the room's walls). Pit_trans's
            // sprites (the fades, the HUD) draw over everything.
            static const bool depth = [] { const char *e = std::getenv("ONI_2D_DEPTH"); return !(e && *e == '0'); }(); // diagnostic
            m.depthTest = depth && !g_pit && !g_overlay;
            m.depthGreater = true;
            m.depthWrite = depth && !g_pit && !g_overlay && !(attr & 0x40u);
            // Point sampling: the menus' sprites map texels 1:1 (a filtered edge would bleed the neighbouring row).
            m.nearest = true;
            m.clampU = m.clampV = true;
            if (attr & 0x40u)
            {
                m.gsBlend = true;
                const bool textureAlpha = (attr & 0x8000u) != 0;
                // As; Pit_trans: Ad, which on the 24-bit display frame the GS reads as 0x80 (FIX 128; the frame's alpha
                // here is whatever the port's draws left, and the cinema bars went through where it was 0); Ot_draw: FIX.
                m.blendC = textureAlpha ? 0 : 2;
                m.blendFix = textureAlpha ? 0 : g_pit ? 0x80 : static_cast<uint8_t>(rgba >> 24);
                m.blendD = 1;
                if (attr & 0x2000u) { m.blendA = 0; m.blendB = 1; }      // (Cs - Cd) A + Cd
                else if (attr & 0x4000u) { m.blendA = 2; m.blendB = 0; } // (0 - Cs) A + Cd
                else { m.blendA = 0; m.blendB = 2; }                     // Cs A + Cd
            }
            else
                m.blend = Material::Blend::Opaque;
            if (textured)
            {
                const uint32_t tfx = static_cast<uint32_t>(tex0 >> 35 & 3u);
                m.textureFunction = tfx == 1 ? Material::TextureFunction::Decal
                                    : tfx == 2 ? Material::TextureFunction::Highlight
                                    : tfx == 3 ? Material::TextureFunction::Highlight2
                                               : Material::TextureFunction::Modulate;
                m.textureAlpha = (tex0 >> 34 & 1u) != 0;
                m.tex0 = tex0;
                // The draw environment's alpha test (TEST_2; Ot_draw never changes it): fully transparent texels are not
                // drawn, so an opaque sprite's empty border leaves what is under it (the menu's frame under its items).
                m.alphaTest = true;
                m.alphaFunc = Material::AlphaFunc::Greater;
                m.alphaRef = 0;
            }
            return m;
        }

        Vertex vertex(const Point &p, float tw, float th)
        {
            Vertex v;
            v.x = p.x;
            v.y = p.y;
            v.z = static_cast<float>(p.z);
            v.u = tw > 0 ? p.u / tw : 0;
            v.v = th > 0 ? p.v / th : 0;
            v.rgba = p.rgba;
            return v;
        }

        uint64_t g_prims = 0, g_drawn = 0, g_missing = 0;

        void emit(uint32_t attr, const Point *pts, int count, uint64_t tex0)
        {
            const uint32_t kind = attr & 7u;
            const bool textured = (attr & 0x10u) != 0;
            std::shared_ptr<const bozo::render::Texture> texture;
            float tw = 1, th = 1;
            if (textured)
            {
                texture = oni::tex::fromTex0(tex0);
                if (!texture)
                {
                    ++g_missing;
                    return;
                }
                tw = static_cast<float>(texture->width);
                th = static_cast<float>(texture->height);
            }
            // Flat shading (IIP off): the last vertex's colour.
            Point p[4];
            for (int i = 0; i < count; ++i)
            {
                p[i] = pts[i];
                if (!(attr & 8u))
                    p[i].rgba = pts[count - 1].rgba;
            }
            std::vector<Vertex> tris;
            if (kind == 6 && count == 2) // sprite: two corners
            {
                Point c[4] = {p[0], p[1], p[0], p[1]};
                c[1].x = p[1].x; c[1].y = p[0].y; c[1].u = p[1].u; c[1].v = p[0].v;
                c[2].x = p[0].x; c[2].y = p[1].y; c[2].u = p[0].u; c[2].v = p[1].v;
                c[3] = p[1];
                for (int i : {0, 1, 2, 1, 3, 2})
                    tris.push_back(vertex(c[i], tw, th));
            }
            else if (kind == 3 && count >= 3)
                for (int i = 0; i < 3; ++i)
                    tris.push_back(vertex(p[i], tw, th));
            else if (kind == 4 && count >= 3) // strip
                for (int i = 2; i < count; ++i)
                    for (int k : {i - 2, i - 1, i})
                        tris.push_back(vertex(p[k], tw, th));
            else if (kind == 5 && count >= 3) // fan
                for (int i = 2; i < count; ++i)
                    for (int k : {0, i - 1, i})
                        tris.push_back(vertex(p[k], tw, th));
            else
            {
                if (trace() >= 2)
                    std::printf("[oni:2d] kind %u with %d vertices not drawn\n", kind, count);
                return;
            }
            // A frame draw in screen space (identity matrix), in the table's order over the background: the frame pass
            // has the GS's own blend, the alpha test and point sampling (the overlay pass reduces them to plain blends).
            static constexpr float kScreen[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            (void)kScreen;
            // Widescreen's zoom (oni_display.h): the 2D in screen units (not GS coordinates: those are the 3D effects
            // projected into the frame) at 3/4 of its size around the frame's centre, inside the 16:9 band; what spans
            // the frame's whole width (a fade, a full-screen picture, an event's cinema bars, which then fall outside
            // the band and are cut with it) as it is.
            if (oni::display::zoomed() && (g_pit || !(attr & 0x800u)) && !tris.empty())
            {
                const bozo::render::Screen s = oni::render::screen();
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const Vertex &v : tris)
                    x0 = std::min(x0, v.x), x1 = std::max(x1, v.x), y0 = std::min(y0, v.y), y1 = std::max(y1, v.y);
                if (x1 - x0 < s.width * 0.9f)
                {
                    const float cx = s.originX + s.width * 0.5f, cy = s.originY + s.height * 0.5f;
                    for (Vertex &v : tris)
                    {
                        v.x = cx + (v.x - cx) * oni::display::kZoom2d;
                        v.y = cy + (v.y - cy) * oni::display::kZoom2d;
                    }
                }
            }
            oni::vu1::queueDraw(material(attr, pts[count - 1].rgba, tex0, textured), tris.data(), static_cast<uint32_t>(tris.size()),
                                texture);
            ++g_drawn;
        }

        // One primitive.
        void primitive(uint8_t *ram, R5900Context *ctx, Runtime *rt, uint32_t at)
        {
            const uint32_t attr = read32(ram, at + 4);
            const uint32_t layout = attr >> 24;
            const bool gs = (attr & 0x800u) != 0;
            ++g_prims;
            if (trace() >= 4)
                std::printf("[oni:2d] prim %#x layout %u attr %08x words %08x %08x %08x %08x\n", at, layout, attr,
                            read32(ram, at + 8), read32(ram, at + 0xc), read32(ram, at + 0x10), read32(ram, at + 0x14));
            Point p[4] = {};
            auto uv = [&](Point &q, uint32_t word) {
                q.u = static_cast<float>(word & 0xffffu);
                q.v = static_cast<float>(word >> 16);
            };
            switch (layout)
            {
            case 2: // two untextured vertices
                for (int i = 0; i < 2; ++i)
                {
                    p[i].rgba = read32(ram, at + 8 + 12u * i);
                    position(read32(ram, at + 0xc + 12u * i), gs, p[i].x, p[i].y);
                    p[i].z = read32(ram, at + 0x10 + 12u * i);
                }
                emit(attr, p, 2, 0);
                return;
            case 3: // textured triangle
            case 5: // textured quad
            {
                const int n = layout == 3 ? 3 : 4;
                for (int i = 0; i < n; ++i)
                {
                    p[i].rgba = read32(ram, at + 8 + 16u * i);
                    uv(p[i], read32(ram, at + 0xc + 16u * i));
                    position(read32(ram, at + 0x10 + 16u * i), gs, p[i].x, p[i].y);
                    p[i].z = read32(ram, at + 0x14 + 16u * i);
                }
                const uint32_t texp = read32(ram, at + (layout == 3 ? 0x40 : 0x50));
                emit(attr, p, n, (attr & 0x10u) && texp ? read64(ram, texp) : 0);
                return;
            }
            case 4: // textured sprite
            {
                const uint32_t rgba = read32(ram, at + 8);
                p[0].rgba = p[1].rgba = rgba;
                uv(p[0], read32(ram, at + 0xc));
                position(read32(ram, at + 0x10), gs, p[0].x, p[0].y);
                p[0].z = read32(ram, at + 0x14);
                uv(p[1], read32(ram, at + 0x18));
                position(read32(ram, at + 0x1c), gs, p[1].x, p[1].y);
                p[1].z = read32(ram, at + 0x20);
                const uint32_t texp = read32(ram, at + 0x24);
                if (trace() >= 3)
                    std::printf("[oni:2d] sprite %#x attr %08x rgba %08x uv %g,%g-%g,%g xy %g,%g-%g,%g tex0 %016llx\n", at, attr,
                                rgba, p[0].u, p[0].v, p[1].u, p[1].v, p[0].x, p[0].y, p[1].x, p[1].y,
                                (unsigned long long)((attr & 0x10u) && texp ? read64(ram, texp) : 0));
                emit(attr, p, 2, (attr & 0x10u) && texp ? read64(ram, texp) : 0);
                return;
            }
            case 6: // a 3D model: the game's own entry
            {
                const uint32_t ra = ctx->r[31].uw[0];
                ctx->r[4].ud[0] = read32(ram, at + 8);
                Md_draw_entry_0x130cc0(ram, ctx, rt);
                ctx->r[31].ud[0] = ra;
                return;
            }
            default:
                if (trace() >= 2)
                    std::printf("[oni:2d] layout %u (attr %08x) at %#x not drawn\n", layout, attr, at);
                return;
            }
        }

        // Ot_draw(pOt, length).
        void otDraw(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            const uint32_t ot = ctx->r[4].uw[0] & 0x1ffffffu;
            const uint32_t end = ot + 4u * ctx->r[5].uw[0];
            uint32_t at = ot, guard = 0;
            while (at != end && ++guard < 200000)
            {
                const uint32_t next = read32(ram, at) & 0x1ffffffu;
                if (next == 0)
                {
                    at += 4;
                    continue;
                }
                if (next < ot || next >= end)
                    primitive(ram, ctx, rt, next);
                at = next;
            }
            ctx->r[2].ud[0] = 0;
            ctx->pc = ra;
        }
        // Pit_trans(): the sprites Pit_prim_entry(prim, priority) queued (the fades, the effects over everything): the
        // pointers at 0x40d690, their count at 0x214604; priorities 0 to 7 in turn, each prim's word 0 its priority,
        // drawn with the sprite layout. The queue is emptied.
        void pitTrans(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t ra = ctx->r[31].uw[0];
            const int32_t count = static_cast<int32_t>(read32(ram, 0x214604));
            if (!g_sprites.empty())
            {
                const bozo::render::Screen s = oni::render::screen();
                g_overlay = true;
                for (const Sprite &q : g_sprites)
                {
                    Point p[2] = {};
                    p[0].rgba = p[1].rgba = q.rgba;
                    p[0].x = s.originX + q.x0, p[0].y = s.originY + q.y0, p[0].u = q.u0, p[0].v = q.v0;
                    p[1].x = s.originX + q.x1, p[1].y = s.originY + q.y1, p[1].u = q.u1, p[1].v = q.v1;
                    emit(q.attr, p, 2, q.tex0);
                }
                g_overlay = false;
                g_sprites.clear();
            }
            int16_t ox, oy;
            std::memcpy(&ox, ram + 0x28e420, 2);
            std::memcpy(&oy, ram + 0x28e422, 2);
            g_pit = true;
            g_offsetX = static_cast<float>(ox - 0x6c0);
            g_offsetY = static_cast<float>(oy - 0xf20);
            for (uint32_t priority = 0; priority < 8; ++priority)
                for (int32_t i = 0; i < count && i < 0x800; ++i)
                {
                    const uint32_t prim = read32(ram, 0x40d690 + 4u * static_cast<uint32_t>(i)) & 0x1ffffffu;
                    if (prim && read32(ram, prim) == priority)
                    {
                        // Always the sprite's fields; the layout byte is not read.
                        const uint32_t attr = read32(ram, prim + 4);
                        Point p[2] = {};
                        p[0].rgba = p[1].rgba = read32(ram, prim + 8);
                        const uint32_t uv0 = read32(ram, prim + 0xc), uv1 = read32(ram, prim + 0x18);
                        p[0].u = static_cast<float>(uv0 & 0xffffu); p[0].v = static_cast<float>(uv0 >> 16);
                        p[1].u = static_cast<float>(uv1 & 0xffffu); p[1].v = static_cast<float>(uv1 >> 16);
                        position(read32(ram, prim + 0x10), false, p[0].x, p[0].y);
                        position(read32(ram, prim + 0x1c), false, p[1].x, p[1].y);
                        const uint32_t texp = read32(ram, prim + 0x24);
                        ++g_prims;
                        if (trace() >= 4)
                            std::printf("[oni:2d] pit %#x priority %u attr %08x rgba %08x xy %g,%g-%g,%g\n", prim, priority,
                                        attr, p[0].rgba, p[0].x, p[0].y, p[1].x, p[1].y);
                        emit(attr, p, 2, (attr & 0x10u) && texp ? read64(ram, texp) : 0);
                    }
                }
            if (trace() >= 5)
            {
                std::printf("[oni:2d] fade work");
                for (int i = 0; i < 16; ++i)
                    std::printf(" %02x", ram[0x27c370 + i]);
                std::printf("\n");
            }
            g_pit = false;
            g_offsetX = g_offsetY = 0;
            std::memset(ram + 0x214604, 0, 4);
            ctx->pc = ra;
        }
    } // namespace

    void submit(uint8_t *)
    {
        if (trace() >= 1 && g_prims)
            std::printf("[oni:2d] frame: %llu primitives, %llu drawn, %llu without texture\n", (unsigned long long)g_prims,
                        (unsigned long long)g_drawn, (unsigned long long)g_missing);
        g_prims = g_drawn = g_missing = 0;
    }

    void overlay(std::vector<Sprite> sprites) { g_sprites = std::move(sprites); }

    void install(bozo::port::Registry &r)
    {
        oni::tex::install(r);
        r.registerFunction(0x12e7f0, otDraw);   // Ot_draw
        r.registerFunction(0x1af6f0, pitTrans); // Pit_trans
    }
} // namespace oni::sprite
