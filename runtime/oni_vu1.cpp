// Onimusha's VU1 path, natively. Md_tag_trans 0x130e40 starts VIF1's DMA on a part's chain (a source chain, the tags'
// upper halves sent to VIF1 as VIF codes): this walks the same chain, runs VIF1's commands on a VU1 state kept here
// (its 16 KB data memory, the double-buffer registers, the program in micro memory), runs the translated microprograms
// (oni_vu1_gen.py) where the chain starts them, and turns each GIF packet a program kicks (XGKICK) or the chain sends
// (DIRECT) into the frame's draws: screen-space triangles with the GS's Z, Q and the draw state the packet sets.
#include "oni_vu1.h"

#include "oni_render.h"
#include "oni_tex.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

namespace oni::vu1
{
    namespace
    {
        using bozo::render::Material;
        using bozo::render::Vertex;

        int trace()
        {
            static const int level = [] { const char *e = std::getenv("ONI_VU1_TRACE"); return e ? std::atoi(e) : 0; }();
            return level;
        }

        // ---- The GS state the packets set, and the triangles they draw ----

        struct GsVertex
        {
            float x, y, z, s, t, q, u, v, fog;
            uint32_t rgba;
        };

        struct Gs
        {
            uint64_t prim = 0;
            uint64_t tex0[2] = {}, tex1[2] = {}, clamp[2] = {}, alpha[2] = {}, test[2] = {}, zbuf[2] = {};
            uint64_t fogcol = 0, texa = 0;
            uint64_t frame[2] = {};
            uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
            std::vector<uint8_t> image; // an IMAGE transfer's texels so far (TRXDIR started it)
            bool imaging = false;
            uint32_t rgba = 0x80808080u;
            float s = 0, t = 0, q = 1.0f;
            uint32_t uv = 0;
            uint32_t fog = 0xff;
            GsVertex queue[3];
            int queued = 0;    // vertices in the queue
            int fanFirst = -1; // a fan's first vertex is kept in queue[0]
            // The batch being built: the material it was built with and its triangles.
            std::vector<Vertex> batch;
            uint64_t batchKey[9] = {};
            bool skip = false;       // the batch is not drawn (it writes nothing the PC frame keeps, or reads an unknown target)
            bool fromTarget = false; // the batch samples the shadow target: u, v from the position
            bool batchOpen = false;
            Material material;
            std::shared_ptr<const bozo::render::Texture> texture;
            uint64_t triangles = 0, batches = 0, missingTextures = 0, uploads = 0;
            float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f}; // x, y, z, q seen
        } g_gs;

        int64_t g_programNs = 0, g_chainNs = 0, g_kickNs = 0, g_cycles = 0; // time in the programs (their kicks included), in whole chains
        constexpr uint32_t kShadowPage = 0xd2;   // FRAME.FBP of the shadows' buffer
        constexpr uint8_t kShadowImage = 3;      // its renderer auxiliary image
        std::set<uint32_t> g_targets; // off-screen targets' first blocks (FRAME.FBP x 32)

        void flush()
        {
            Gs &gs = g_gs;
            if (gs.batchOpen && !gs.batch.empty() && !gs.skip)
            {
                static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
                // The GS's scissor: the frame (640 x 448 here, the fields' 224 lines doubled).
                const bozo::render::Screen s = oni::render::screen();
                bozo::render::Renderer::instance().setClip(s.originX, s.originY, s.originX + s.width, s.originY + s.height);
                bozo::render::Renderer::instance().draw(gs.material, kIdentity, gs.batch.data(),
                                                        static_cast<uint32_t>(gs.batch.size()), gs.texture);
                bozo::render::Renderer::instance().clearClip();
                ++gs.batches;
            }
            else if (gs.batchOpen && !gs.batch.empty() && trace() >= 2)
                std::printf("[oni:vu1] batch skipped: prim %03llx frame %llx tex0 %016llx alpha %llx test %llx zbuf %llx\n",
                            (unsigned long long)gs.prim, (unsigned long long)gs.frame[(gs.prim >> 9) & 1],
                            (unsigned long long)gs.tex0[(gs.prim >> 9) & 1], (unsigned long long)gs.alpha[(gs.prim >> 9) & 1],
                            (unsigned long long)gs.test[(gs.prim >> 9) & 1], (unsigned long long)gs.zbuf[(gs.prim >> 9) & 1]);
            gs.batch.clear();
            gs.batchOpen = false;
        }

        // The material from the GS registers of the primitive's context.
        void openBatch()
        {
            Gs &gs = g_gs;
            const int c = (gs.prim >> 9) & 1;
            const uint64_t key[9] = {gs.prim & 0x7f8, gs.tex0[c], gs.tex1[c], gs.clamp[c], gs.alpha[c], gs.test[c], gs.zbuf[c], gs.fogcol,
                                     gs.frame[c]};
            if (gs.batchOpen && std::equal(key, key + 9, gs.batchKey))
                return;
            flush();
            std::copy(key, key + 9, gs.batchKey);
            gs.skip = false;
            gs.batchOpen = true;
            Material m;
            const bool tme = (gs.prim >> 4) & 1, fge = (gs.prim >> 5) & 1, abe = (gs.prim >> 6) & 1;
            m.textured = tme;
            gs.texture = nullptr;
            if (tme)
            {
                const uint64_t tex0 = gs.tex0[c];
                m.tex0 = tex0;
                const uint32_t tfx = static_cast<uint32_t>(tex0 >> 35 & 3u);
                m.textureFunction = tfx == 1 ? Material::TextureFunction::Decal
                                    : tfx == 2 ? Material::TextureFunction::Highlight
                                    : tfx == 3 ? Material::TextureFunction::Highlight2
                                               : Material::TextureFunction::Modulate;
                m.textureAlpha = (tex0 >> 34 & 1u) != 0;
                gs.texture = oni::tex::fromTex0(tex0);
                if (!gs.texture)
                {
                    // Not uploaded: the frame or another target read back (the screen effects' passes), not drawn.
                    ++gs.missingTextures;
                    gs.skip = true;
                }
                m.nearest = ((gs.tex1[c] >> 5) & 1u) == 0;
                const uint32_t wms = gs.clamp[c] & 3u, wmt = (gs.clamp[c] >> 2) & 3u;
                m.clampU = wms == 1;
                m.clampV = wmt == 1;
                m.useQ = ((gs.prim >> 8) & 1) == 0;
            }
            if (abe)
            {
                const uint64_t a = gs.alpha[c];
                m.gsBlend = true;
                m.blendA = a & 3;
                m.blendB = (a >> 2) & 3;
                m.blendC = (a >> 4) & 3;
                m.blendD = (a >> 6) & 3;
                m.blendFix = static_cast<uint8_t>(a >> 32);
            }
            else
                m.blend = Material::Blend::Opaque;
            const uint64_t test = gs.test[c];
            if (test & 1u)
            {
                m.alphaTest = true;
                m.alphaFunc = static_cast<Material::AlphaFunc>((test >> 1) & 7u);
                m.alphaRef = static_cast<uint8_t>(test >> 4);
            }
            const bool zte = (test >> 16) & 1u;
            const uint32_t ztst = (test >> 17) & 3u;
            m.depthTest = zte && ztst >= 2;
            m.depthGreater = ztst == 3;
            m.depthWrite = ((gs.zbuf[c] >> 32) & 1u) == 0;
            // FRAME.FBMSK: a channel whose bits are all masked is not written.
            const uint32_t fbmsk = static_cast<uint32_t>(gs.frame[c] >> 32);
            m.colourMask = 0;
            for (int k = 0; k < 4; ++k)
                if (((fbmsk >> (8 * k)) & 0xffu) != 0xffu)
                    m.colourMask |= 1u << k;
            if (m.colourMask == 0 && !m.depthWrite)
                gs.skip = true;
            // The frame is CT24 (FRAME.PSM 1): its alpha is not kept, a blend changes only the colour. On the PC the frame's
            // alpha is what covers the backdrop (the room's picture under the 3D): a blended draw leaves it as it is.
            if (abe && ((gs.frame[c] >> 24) & 0x3fu) == 1u)
                m.colourMask &= 0x7u;
            // An untextured primitive's colour is what the GS writes (bytes), not a 0x80-unit modulation.
            m.directColour = !tme;
            // FRAME.FBP: the display's two field buffers are pages 0 and 0x46 (640 x 224, 32 bits). A draw elsewhere goes
            // to an off-screen target: the shadows' buffer at 0xd2 (cleared to grey, the shadow volumes counted in its
            // colour against the scene's depth, graded to 0x14 where shadowed), read back by the pass that darkens the
            // frame with it. Natively an auxiliary image of the renderer sharing the frame's depth (Material::frameDepth).
            const uint32_t fbp = static_cast<uint32_t>(gs.frame[c] & 0x1ffu);
            gs.fromTarget = false;
            if (fbp >= 0x8cu)
            {
                if (fbp != kShadowPage)
                    gs.skip = true; // no other target is known
                m.target = kShadowImage;
                m.frameDepth = true;
                g_targets.insert(fbp * 32u);
            }
            // A texture read from such a target: the target's image (u, v over the frame), or nothing (blocks an earlier
            // scene uploaded to may still be kept).
            if (tme && g_targets.count(static_cast<uint32_t>(gs.tex0[c] & 0x3fffu)))
            {
                if ((gs.tex0[c] & 0x3fffu) == kShadowPage * 32u)
                {
                    gs.skip = false;
                    gs.fromTarget = true;
                    m.fromFrame = 1;
                    m.source = kShadowImage;
                    m.nearest = true;
                    m.useQ = false;
                    if (!gs.texture)
                        --gs.missingTextures;
                    gs.texture = nullptr;
                }
                else
                    gs.skip = true;
            }
            static const bool noShadows = [] { const char *e = std::getenv("ONI_SHADOWS"); return e && *e == '0'; }();
            if (noShadows && (m.target || gs.fromTarget))
                gs.skip = true;
            static const bool noDepth = std::getenv("ONI_VU1_NODEPTH") != nullptr;
            if (noDepth)
                m.depthTest = false;
            if (fge)
            {
                m.fog = true;
                m.fogColour = static_cast<uint32_t>(gs.fogcol & 0xffffffu);
            }
            static const bool noTex = std::getenv("ONI_VU1_NOTEX") != nullptr; // diagnostic: vertex colours only
            if (noTex && m.textured)
            {
                m.textured = false;
                gs.texture = nullptr;
            }
            static const bool flat = std::getenv("ONI_VU1_FLAT") != nullptr; // diagnostic: plain opaque triangles
            if (flat)
            {
                m = Material{};
                m.textured = false;
                m.blend = Material::Blend::Opaque;
                m.depthTest = false;
                gs.texture = nullptr;
            }
            gs.material = m;
        }

        Vertex toVertex(const GsVertex &g)
        {
            Gs &gs = g_gs;
            const bozo::render::Screen s = oni::render::screen();
            Vertex v;
            v.x = g.x;
            v.y = s.originY + (g.y - 1936.0f) * 2.0f; // the field's lines to the frame's
            v.z = g.z;
            v.rgba = g.rgba;
            v.fog = g.fog / 255.0f;
            const int c = (gs.prim >> 9) & 1;
            if (gs.fromTarget) // the shadow target, over the frame
            {
                v.u = (v.x - s.originX) / s.width;
                v.v = (v.y - s.originY) / s.height;
                v.q = 1.0f;
            }
            else if ((gs.prim >> 8) & 1) // FST: UV in texels
            {
                const float tw = static_cast<float>(1u << ((gs.tex0[c] >> 26) & 15u));
                const float th = static_cast<float>(1u << ((gs.tex0[c] >> 30) & 15u));
                v.u = g.u / tw;
                v.v = g.v / th;
                v.q = 1.0f;
            }
            else
            {
                const float q = g.q != 0.0f ? g.q : 1.0f;
                v.u = g.s / q;
                v.v = g.t / q;
                v.q = q;
            }
            return v;
        }

        void triangle(const GsVertex &a, const GsVertex &b, const GsVertex &c)
        {
            Gs &gs = g_gs;
            openBatch();
            for (const GsVertex *g : {&a, &b, &c})
            {
                Vertex v = toVertex(*g);
                static const bool flat = std::getenv("ONI_VU1_FLAT") != nullptr;
                if (flat)
                {
                    if ((gs.prim & 7u) == 6u)
                        return; // no sprites
                    const uint32_t h = static_cast<uint32_t>(gs.batches * 2654435761u);
                    v.rgba = 0x80000000u | (h & 0xffffffu);
                    v.q = 1.0f;
                }
                gs.batch.push_back(v);
                const float f[4] = {v.x, v.y, v.z, v.q};
                for (int k = 0; k < 4; ++k)
                {
                    gs.lo[k] = std::min(gs.lo[k], f[k]);
                    gs.hi[k] = std::max(gs.hi[k], f[k]);
                }
            }
            ++gs.triangles;
        }

        // A vertex kick (XYZ2 / XYZF2): the primitive assembled as the GS does; adc: XYZ's ADC bit (queued, no draw).
        void kickVertex(uint32_t x, uint32_t y, uint32_t z, uint32_t fog, bool adc)
        {
            Gs &gs = g_gs;
            GsVertex v{static_cast<float>(x) / 16.0f, static_cast<float>(y) / 16.0f, static_cast<float>(z), gs.s, gs.t, gs.q,
                       static_cast<float>(gs.uv & 0x3fff) / 16.0f, static_cast<float>((gs.uv >> 16) & 0x3fff) / 16.0f,
                       static_cast<float>(fog), gs.rgba};
            const uint32_t type = gs.prim & 7u;
            switch (type)
            {
            case 3: // triangles
                gs.queue[gs.queued++] = v;
                if (gs.queued == 3)
                {
                    if (!adc)
                        triangle(gs.queue[0], gs.queue[1], gs.queue[2]);
                    gs.queued = 0;
                }
                break;
            case 4: // strip
                if (gs.queued == 3)
                {
                    gs.queue[0] = gs.queue[1];
                    gs.queue[1] = gs.queue[2];
                    gs.queued = 2;
                }
                gs.queue[gs.queued++] = v;
                if (gs.queued == 3 && !adc)
                    triangle(gs.queue[0], gs.queue[1], gs.queue[2]);
                break;
            case 5: // fan
                if (gs.queued == 3)
                {
                    gs.queue[1] = gs.queue[2];
                    gs.queued = 2;
                }
                gs.queue[gs.queued++] = v;
                if (gs.queued == 3 && !adc)
                    triangle(gs.queue[0], gs.queue[1], gs.queue[2]);
                break;
            case 6: // sprite: two corners
                gs.queue[gs.queued++] = v;
                if (gs.queued == 2)
                {
                    if (trace() >= 2)
                    {
                        const int c = (gs.prim >> 9) & 1;
                        std::printf("[oni:gif] sprite prim %03llx (%.0f,%.0f)-(%.0f,%.0f) z %.0f rgba %08x tex0 %016llx alpha %016llx test %llx zbuf %llx\n",
                                    (unsigned long long)gs.prim, gs.queue[0].x, gs.queue[0].y, gs.queue[1].x, gs.queue[1].y,
                                    gs.queue[1].z, gs.queue[1].rgba, (unsigned long long)gs.tex0[c], (unsigned long long)gs.alpha[c],
                                    (unsigned long long)gs.test[c], (unsigned long long)gs.zbuf[c]);
                    }
                    if (!adc)
                    {
                        const GsVertex &a = gs.queue[0], &b = gs.queue[1];
                        GsVertex c = a, d = a;
                        c.x = b.x; c.s = b.s; c.u = b.u;
                        d.y = b.y; d.t = b.t; d.v = b.v;
                        c.rgba = d.rgba = b.rgba;
                        c.z = d.z = b.z;
                        triangle(a, c, d);
                        GsVertex e = b;
                        triangle(c, e, d);
                    }
                    gs.queued = 0;
                }
                break;
            default: // points and lines: not drawn
                gs.queued = 0;
                break;
            }
        }

        void setPrim(uint64_t value)
        {
            g_gs.prim = value & 0x7ff;
            g_gs.queued = 0;
        }

        uint64_t g_unknownAd = 0;

        // A register write (A+D or REGLIST).
        void writeRegister(uint32_t reg, uint64_t value)
        {
            Gs &gs = g_gs;
            switch (reg)
            {
            case 0x00: setPrim(value); break;
            case 0x01: gs.rgba = static_cast<uint32_t>(value); { uint32_t q = static_cast<uint32_t>(value >> 32); std::memcpy(&gs.q, &q, 4); } break;
            case 0x02: { uint32_t s = static_cast<uint32_t>(value), t = static_cast<uint32_t>(value >> 32); std::memcpy(&gs.s, &s, 4); std::memcpy(&gs.t, &t, 4); } break;
            case 0x03: gs.uv = static_cast<uint32_t>(value); break;
            case 0x04: kickVertex(value & 0xffff, (value >> 16) & 0xffff, (value >> 32) & 0xffffff, (value >> 56) & 0xff, false); break;
            case 0x05: kickVertex(value & 0xffff, (value >> 16) & 0xffff, static_cast<uint32_t>(value >> 32), gs.fog, false); break;
            case 0x06: gs.tex0[0] = value; break;
            case 0x07: gs.tex0[1] = value; break;
            case 0x08: gs.clamp[0] = value; break;
            case 0x09: gs.clamp[1] = value; break;
            case 0x0a: gs.fog = static_cast<uint32_t>(value >> 56); break;
            case 0x0c: kickVertex(value & 0xffff, (value >> 16) & 0xffff, (value >> 32) & 0xffffff, (value >> 56) & 0xff, true); break;
            case 0x0d: kickVertex(value & 0xffff, (value >> 16) & 0xffff, static_cast<uint32_t>(value >> 32), gs.fog, true); break;
            case 0x14: gs.tex1[0] = value; break;
            case 0x15: gs.tex1[1] = value; break;
            case 0x3b: gs.texa = value; break;
            case 0x3d: gs.fogcol = value; break;
            case 0x42: gs.alpha[0] = value; break;
            case 0x43: gs.alpha[1] = value; break;
            case 0x47: gs.test[0] = value; break;
            case 0x48: gs.test[1] = value; break;
            case 0x50: gs.bitbltbuf = value; break;
            case 0x51: gs.trxpos = value; break;
            case 0x52: gs.trxreg = value; break;
            case 0x53: // TRXDIR: host to local starts a transfer
                gs.image.clear();
                gs.imaging = (value & 3) == 0;
                break;
            case 0x4c:
                gs.frame[0] = value;
                if (trace() >= 2)
                    std::printf("[oni:gif] FRAME_1 fbp %llx fbw %llu psm %llx fbmsk %08llx\n", (unsigned long long)(value & 0x1ff),
                                (unsigned long long)((value >> 16) & 0x3f), (unsigned long long)((value >> 24) & 0x3f),
                                (unsigned long long)(value >> 32));
                break;
            case 0x4d: gs.frame[1] = value; break;
            case 0x4e: gs.zbuf[0] = value; break;
            case 0x4f: gs.zbuf[1] = value; break;
            case 0x3f: // TEXFLUSH
            case 0x16: case 0x17: // TEX2
            case 0x18: case 0x19: // XYOFFSET
            case 0x1a: // PRMODECONT
            case 0x1b: // PRMODE
            case 0x1c: // TEXCLUT
            case 0x22: // SCANMSK
            case 0x34: case 0x35: case 0x36: case 0x37: // MIPTBP
            case 0x40: case 0x41: // SCISSOR
            case 0x44: case 0x45: case 0x46: // DIMX, DTHE, COLCLAMP
            case 0x49: case 0x4a: case 0x4b: // PABE, FBA
            case 0x54: // HWREG
            case 0x60: // SIGNAL...
            case 0x61: case 0x62: case 0x7f:
                break;
            default:
                if (trace() && g_unknownAd++ < 32)
                    std::printf("[oni:gif] register 0x%02x = %016llx not handled\n", reg, (unsigned long long)value);
                break;
            }
        }

        // One GIF packet from qwords (get(i): the i-th qword); max: qwords available. Returns the qwords read.
        template <typename Get>
        uint32_t gifPacket(Get get, uint32_t max)
        {
            Gs &gs = g_gs;
            uint32_t at = 0;
            for (int guard = 0; guard < 4096 && at < max; ++guard)
            {
                uint64_t tag[2];
                get(at++, tag);
                const uint32_t nloop = tag[0] & 0x7fff;
                const bool eop = (tag[0] >> 15) & 1;
                const bool pre = (tag[0] >> 46) & 1;
                const uint32_t flg = (tag[0] >> 58) & 3;
                uint32_t nreg = (tag[0] >> 60) & 15;
                if (!nreg)
                    nreg = 16;
                if (trace() >= 5)
                {
                    std::printf("[oni:gif] tag %016llx %016llx\n", (unsigned long long)tag[0], (unsigned long long)tag[1]);
                    for (uint32_t k = 0; k < std::min<uint32_t>(nreg * 2, max - at); ++k)
                    {
                        uint64_t q[2];
                        get(at + k, q);
                        std::printf("[oni:gif]   %016llx %016llx\n", (unsigned long long)q[1], (unsigned long long)q[0]);
                    }
                }
                if (pre && flg == 0)
                    setPrim((tag[0] >> 47) & 0x7ff);
                if (flg == 0)
                {
                    for (uint32_t l = 0; l < nloop && at < max; ++l)
                        for (uint32_t r = 0; r < nreg && at < max; ++r)
                        {
                            uint64_t q[2];
                            get(at++, q);
                            const uint32_t reg = (tag[1] >> (4 * r)) & 15;
                            const uint32_t w0 = static_cast<uint32_t>(q[0]), w1 = static_cast<uint32_t>(q[0] >> 32);
                            const uint32_t w2 = static_cast<uint32_t>(q[1]), w3 = static_cast<uint32_t>(q[1] >> 32);
                            switch (reg)
                            {
                            case 0x0: setPrim(q[0]); break;
                            case 0x1: gs.rgba = (w0 & 0xff) | (w1 & 0xff) << 8 | (w2 & 0xff) << 16 | (w3 & 0xff) << 24; break;
                            case 0x2: std::memcpy(&gs.s, &w0, 4); std::memcpy(&gs.t, &w1, 4); std::memcpy(&gs.q, &w2, 4); break;
                            case 0x3: gs.uv = (w0 & 0x3fff) | (w1 & 0x3fff) << 16; break;
                            case 0x4: kickVertex(w0 & 0xffff, w1 & 0xffff, (w2 >> 4) & 0xffffff, (w3 >> 4) & 0xff, (w3 >> 15) & 1); break;
                            case 0x5: kickVertex(w0 & 0xffff, w1 & 0xffff, w2, gs.fog, (w3 >> 15) & 1); break;
                            case 0xa: gs.fog = (w3 >> 4) & 0xff; break;
                            case 0xc: kickVertex(w0 & 0xffff, w1 & 0xffff, (w2 >> 4) & 0xffffff, (w3 >> 4) & 0xff, true); break;
                            case 0xd: kickVertex(w0 & 0xffff, w1 & 0xffff, w2, gs.fog, true); break;
                            case 0xe: writeRegister(w2 & 0xff, q[0]); break;
                            case 0xf: break;
                            default: writeRegister(reg, q[0]); break; // TEX0, CLAMP: the register's own 64 bits
                            }
                        }
                }
                else if (flg == 1)
                {
                    const uint32_t words = nloop * nreg;
                    for (uint32_t k = 0; k < words; k += 2)
                    {
                        if (at >= max)
                            break;
                        uint64_t q[2];
                        get(at++, q);
                        writeRegister((tag[1] >> (4 * (k % nreg))) & 15, q[0]);
                        if (k + 1 < words)
                            writeRegister((tag[1] >> (4 * ((k + 1) % nreg))) & 15, q[1]);
                    }
                }
                else
                {
                    // IMAGE: texels for local memory, after TRXDIR; kept as an upload once the rectangle is complete.
                    for (uint32_t l = 0; l < nloop && at < max; ++l)
                    {
                        uint64_t q[2];
                        get(at++, q);
                        if (gs.imaging)
                            gs.image.insert(gs.image.end(), reinterpret_cast<const uint8_t *>(q), reinterpret_cast<const uint8_t *>(q) + 16);
                    }
                    if (gs.imaging)
                    {
                        const uint32_t psm = (gs.bitbltbuf >> 56) & 0x3f;
                        const uint32_t bpp = psm == 0 ? 32 : psm == 1 ? 24 : (psm == 2 || psm == 0xa) ? 16 : psm == 0x13 ? 8 : psm == 0x14 ? 4 : 32;
                        const uint64_t need = ((gs.trxreg & 0xfff) * ((gs.trxreg >> 32) & 0xfff) * bpp + 7) / 8;
                        if (gs.image.size() >= need)
                        {
                            oni::tex::upload(gs.bitbltbuf, gs.trxpos, gs.trxreg, gs.image.data(), gs.image.size());
                            ++gs.uploads;
                            gs.imaging = false;
                            gs.image.clear();
                        }
                    }
                }
                if (eop)
                    break;
            }
            return at;
        }

        // ---- VIF1 and VU1 ----

        struct Vif
        {
            uint32_t base = 0, offset = 0, tops = 0, top = 0, itops = 0, itop = 0;
            bool dbf = false;
            uint32_t cl = 1, wl = 1, mode = 0, mask = 0;
            uint32_t row[4] = {}, col[4] = {};
            const Program *program = nullptr;
            uint32_t extent = 0; // pairs uploaded from micro address 0
            uint32_t next = 0; // MSCNT's pc
            R5900Context regs;
            alignas(16) uint8_t mem[16384] = {};
            alignas(16) uint8_t micro[16384] = {};
            uint64_t unknownPrograms = 0, runs = 0;
        };
        Vif &vif()
        {
            static Vif *v = new Vif;
            return *v;
        }

        void kick(Vu1Io &io, uint32_t qword)
        {
            if (trace() >= 3)
                std::printf("[oni:vu1] xgkick %03x\n", qword);
            const auto t0 = std::chrono::steady_clock::now();
            gifPacket([&](uint32_t i, uint64_t q[2]) { std::memcpy(q, quad(io, qword + i), 16); }, 1024);
            g_kickNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        }

        void run(Vif &v, uint32_t pc)
        {
            // MSCAL / MSCNT: the double buffer swaps, ITOP latched.
            v.top = v.tops;
            v.itop = v.itops;
            if (v.dbf)
            {
                v.tops = v.base;
                v.dbf = false;
            }
            else
            {
                v.tops = v.base + v.offset;
                v.dbf = true;
            }
            if (!v.program)
            {
                ++v.unknownPrograms;
                return;
            }
            Vu1Io io;
            io.mem = v.mem;
            io.top = v.top;
            io.itop = v.itop;
            io.kick = kick;
            const auto t0 = std::chrono::steady_clock::now();
            v.next = v.program->run(&v.regs, io, pc);
            g_programNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            ++v.runs;
            g_cycles += static_cast<int64_t>(io.cycles);
            if (io.fault != ~0u)
                std::fprintf(stderr, "[oni:vu1] %s: pc 0x%x not translated\n", v.program->name, io.fault);
            if (trace() >= 2)
                std::printf("[oni:vu1] run %s at %x -> %x (%llu cycles)\n", v.program->name, pc, v.next, (unsigned long long)io.cycles);
        }

        uint64_t fnv(const uint8_t *p, size_t n)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < n; ++i)
                h = (h ^ p[i]) * 0x100000001B3ull;
            return h;
        }

        // UNPACK: num elements of vn + 1 components of vl's width to VU memory from address (qwords).
        uint32_t unpack(Vif &v, uint32_t cmd, uint32_t num, uint32_t imm, const uint32_t *words, uint32_t available)
        {
            const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
            const bool masked = (cmd >> 4) & 1, usn = (imm >> 14) & 1, flg = (imm >> 15) & 1;
            const uint32_t n = num ? num : 256;
            uint32_t address = imm & 0x3ff;
            if (flg)
                address += v.tops;
            const uint32_t bits = vl == 0 ? 32 : vl == 1 ? 16 : vl == 2 ? 8 : 16; // V4-5: 16 bits a vertex
            const uint32_t components = vl == 3 ? 1 : vn + 1;
            const uint32_t sizeWords = (n * components * bits + 31) / 32;
            const uint8_t *data = reinterpret_cast<const uint8_t *>(words);
            uint32_t element = 0;
            auto read = [&](uint32_t index) -> uint32_t
            {
                const uint32_t byteAt = index * bits / 8;
                if (byteAt + bits / 8 > available * 4)
                    return 0;
                if (bits == 32)
                {
                    uint32_t x;
                    std::memcpy(&x, data + byteAt, 4);
                    return x;
                }
                if (bits == 16)
                {
                    uint16_t x;
                    std::memcpy(&x, data + byteAt, 2);
                    return usn ? x : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(x)));
                }
                const uint8_t x = data[byteAt];
                return usn ? x : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(x)));
            };
            const uint32_t cl = v.cl ? v.cl : 1, wl = v.wl ? v.wl : 1;
            for (uint32_t j = 0; j < n; ++j)
            {
                uint32_t qaddr, cycle;
                bool fill = false;
                if (wl <= cl)
                {
                    qaddr = address + (j / wl) * cl + (j % wl);
                    cycle = j % wl;
                }
                else
                {
                    qaddr = address + j;
                    cycle = j % wl;
                    fill = cycle >= cl;
                }
                uint32_t value[4] = {};
                if (!fill)
                {
                    if (vl == 3)
                    {
                        const uint32_t c = read(element++);
                        value[0] = (c & 0x1f) << 3;
                        value[1] = ((c >> 5) & 0x1f) << 3;
                        value[2] = ((c >> 10) & 0x1f) << 3;
                        value[3] = ((c >> 15) & 1) << 7;
                    }
                    else if (vn == 0)
                    {
                        const uint32_t c = read(element++);
                        value[0] = value[1] = value[2] = value[3] = c;
                    }
                    else
                    {
                        for (uint32_t k = 0; k <= vn; ++k)
                            value[k] = read(element++);
                        // V2: z, w are x, y again; V3: w is undefined (left as the next data's would leave it: x).
                        if (vn == 1)
                        {
                            value[2] = value[0];
                            value[3] = value[1];
                        }
                    }
                }
                uint8_t *q = v.mem + ((qaddr & 0x3ff) << 4);
                for (uint32_t f = 0; f < 4; ++f)
                {
                    const uint32_t m = masked ? (v.mask >> ((std::min(cycle, 3u) * 8) + f * 2)) & 3 : (fill ? 1u : 0u);
                    if (!masked && vn == 2 && f == 3 && vl != 3 && !fill)
                        continue; // V3's w: not written here
                    uint32_t out;
                    if (m == 0)
                    {
                        out = value[f];
                        if (v.mode == 1)
                            out += v.row[f];
                        else if (v.mode == 2)
                            out = v.row[f] = v.row[f] + value[f];
                    }
                    else if (m == 1)
                        out = v.row[f];
                    else if (m == 2)
                        out = v.col[std::min(cycle, 3u)];
                    else
                        continue;
                    std::memcpy(q + 4 * f, &out, 4);
                }
            }
            return sizeWords;
        }

        uint64_t g_chains = 0, g_vifUnknown = 0;

        // VIF1's commands over the chain's words (the tags' upper halves at qword offsets 2 and 3).
        // w: the words VIF1 sees; grid: each one's place in the DMA's qwords (a tag's lower half is not VIF1's, but DIRECT's
        // data starts on a qword of that grid).
        void vifStream(Vif &v, const std::vector<uint32_t> &w, const std::vector<uint32_t> &grid)
        {
            size_t i = 0;
            while (i < w.size())
            {
                const uint32_t code = w[i++];
                const uint32_t cmd = (code >> 24) & 0x7f, num = (code >> 16) & 0xff, imm = code & 0xffff;
                if (trace() >= 4)
                    std::printf("[oni:vif] %08x\n", code);
                if (cmd >= 0x60)
                {
                    i += unpack(v, cmd, num, imm, w.data() + i, static_cast<uint32_t>(w.size() - i));
                    continue;
                }
                switch (cmd)
                {
                case 0x00: case 0x06: case 0x07: case 0x10: case 0x11: case 0x13: break;
                case 0x01: v.cl = imm & 0xff; v.wl = (imm >> 8) & 0xff; break;
                case 0x02: v.offset = imm & 0x3ff; v.dbf = false; v.tops = v.base; break;
                case 0x03: v.base = imm & 0x3ff; break;
                case 0x04: v.itops = imm & 0x3ff; break;
                case 0x05: v.mode = imm & 3; break;
                case 0x14: case 0x15: run(v, (imm * 8) & 0x3fff); break;
                case 0x17: run(v, v.next); break;
                case 0x20: if (i < w.size()) v.mask = w[i++]; break;
                case 0x30: for (int k = 0; k < 4 && i < w.size(); ++k) v.row[k] = w[i++]; break;
                case 0x31: for (int k = 0; k < 4 && i < w.size(); ++k) v.col[k] = w[i++]; break;
                case 0x4a:
                {
                    const uint32_t n = num ? num : 256;
                    const uint32_t at = (imm * 8) & 0x3fff;
                    const size_t words = std::min<size_t>(2 * n, w.size() - i);
                    std::memcpy(v.micro + at, w.data() + i, std::min<size_t>(words * 4, sizeof v.micro - at));
                    i += 2 * n;
                    // The program in micro memory: the MPGs from address 0 on, known by its words' hash.
                    v.extent = at == 0 ? n : at == v.extent * 8 ? v.extent + n : 0;
                    v.program = nullptr;
                    if (v.extent)
                    {
                        const uint64_t h = fnv(v.micro, v.extent * 8);
                        for (size_t k = 0; k < kProgramCount; ++k)
                            if (kPrograms[k].hash == h && kPrograms[k].pairs == v.extent)
                                v.program = &kPrograms[k];
                    }
                    if (!v.program && trace())
                        std::printf("[oni:vif] MPG of %u pairs at %x: not a known program\n", n, at);
                    break;
                }
                case 0x50: case 0x51:
                {
                    while (i < w.size() && (grid[i] & 3u))
                        ++i; // DIRECT's data starts at a qword
                    const uint32_t n = imm ? imm : 65536;
                    const size_t avail = (w.size() - i) / 4;
                    const uint32_t* data = w.data() + i;
                    gifPacket([&](uint32_t k, uint64_t q[2]) { std::memcpy(q, data + 4 * k, 16); }, static_cast<uint32_t>(std::min<size_t>(n, avail)));
                    i += 4 * size_t(n);
                    break;
                }
                default:
                    if (g_vifUnknown++ < 16)
                        std::fprintf(stderr, "[oni:vif] VIF code %08x not handled\n", code);
                    return;
                }
            }
        }
    } // namespace

    void runChain(uint8_t *ram, uint8_t *scratchpad, uint32_t tag)
    {
        // ONI_VU1=0: the models not drawn (a comparison).
        static const bool on = [] { const char *e = std::getenv("ONI_VU1"); return !(e && *e == '0'); }();
        if (!on)
            return;
        Vif &v = vif();
        std::vector<uint32_t> words, grid; // the stream, and each word's place (a tag's VIF codes at offsets 2, 3)
        uint32_t place = 0;
        auto qwords = [&](uint32_t address, uint32_t count)
        {
            const bool spr = (address & 0x80000000u) != 0;
            for (uint32_t k = 0; k < count * 4; ++k)
            {
                uint32_t x;
                if (spr)
                    std::memcpy(&x, scratchpad + ((address + 4 * k) & 0x3fff), 4);
                else
                    std::memcpy(&x, ram + ((address + 4 * k) & 0x1ffffff), 4);
                words.push_back(x);
                grid.push_back(place++);
            }
        };
        uint32_t stack[2] = {}, depth = 0;
        uint32_t at = tag;
        for (int guard = 0; guard < 65536; ++guard)
        {
            uint32_t t[4];
            if (at & 0x80000000u)
                std::memcpy(t, scratchpad + (at & 0x3ff0), 16);
            else
                std::memcpy(t, ram + (at & 0x1fffff0), 16);
            const uint32_t qwc = t[0] & 0xffff, id = (t[0] >> 28) & 7, addr = t[1];
            words.push_back(t[2]);
            grid.push_back(place + 2);
            words.push_back(t[3]);
            grid.push_back(place + 3);
            place += 4;
            bool end = false;
            switch (id)
            {
            case 0: qwords(addr, qwc); end = true; break;                          // refe
            case 1: qwords(at + 16, qwc); at += 16 + 16 * qwc; break;             // cnt
            case 2: qwords(at + 16, qwc); at = addr; break;                       // next
            case 3: case 4: qwords(addr, qwc); at += 16; break;                    // ref, refs
            case 5:                                                                // call
                qwords(at + 16, qwc);
                if (depth < 2)
                    stack[depth++] = at + 16 + 16 * qwc;
                at = addr;
                break;
            case 6:                                                                // ret
                qwords(at + 16, qwc);
                if (depth)
                    at = stack[--depth];
                else
                    end = true;
                break;
            default: qwords(at + 16, qwc); end = true; break;                    // end
            }
            if (trace() >= 3)
                std::printf("[oni:dma] tag %08x id %u qwc %u addr %08x vif %08x %08x\n", at, id, qwc, addr, t[2], t[3]);
            if (end)
                break;
        }
        ++g_chains;
        const auto t0 = std::chrono::steady_clock::now();
        vifStream(v, words, grid);
        flush();
        g_chainNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }

    void frameStats()
    {
        Gs &gs = g_gs;
        if (trace())
            std::printf("[oni:vu1] frame: %llu chains, %llu runs, %llu triangles in %llu batches, %llu without texture, %llu unknown programs\n",
                        (unsigned long long)g_chains, (unsigned long long)vif().runs, (unsigned long long)gs.triangles,
                        (unsigned long long)gs.batches, (unsigned long long)gs.missingTextures,
                        (unsigned long long)vif().unknownPrograms);
        if (trace() && gs.triangles)
            std::printf("[oni:vu1] x %.1f..%.1f y %.1f..%.1f z %.0f..%.0f q %g..%g\n", gs.lo[0], gs.hi[0], gs.lo[1], gs.hi[1],
                        gs.lo[2], gs.hi[2], gs.lo[3], gs.hi[3]);
        for (int k = 0; k < 4; ++k)
        {
            gs.lo[k] = 1e30f;
            gs.hi[k] = -1e30f;
        }
        if (trace())
            std::printf("[oni:vu1] time: %.2f ms in chains, %.2f ms in programs (%.2f ms in kicks), %lld VU cycles\n", g_chainNs / 1e6, g_programNs / 1e6, g_kickNs / 1e6, (long long)g_cycles);
        g_chainNs = g_programNs = g_kickNs = g_cycles = 0;
        g_chains = 0;
        vif().runs = 0;
        gs.triangles = gs.batches = gs.missingTextures = 0;
    }
} // namespace oni::vu1
