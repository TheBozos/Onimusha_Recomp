// Onimusha's textures: every image the game uploads goes through libgraph's sceGsSetDefLoadImage(lp, dbp, dbw, psm, x, y,
// w, h) and sceGsExecLoadImage(lp, src) (Mapping_oim2 / Mapping_oim3 for the OIM textures and their palettes, the fonts,
// the fades). Natively there is no GS memory: the port answers those two calls by keeping each upload as it was given
// (its destination block, format, rectangle and texel bytes, copied from EE memory), and a draw's TEX0 finds its texels
// and its palette by their block address. A texture is decoded to RGBA once per TEX0 and re-decoded when an upload it
// uses is replaced.
#include "oni_tex.h"

#include "bozo/ee/dispatcher.h"
#include "bozo/render/texture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

namespace oni::tex
{
    namespace
    {
        struct Params
        {
            uint32_t dbp = 0, dbw = 0, psm = 0, x = 0, y = 0, w = 0, h = 0;
        };
        struct Upload
        {
            Params p;
            std::vector<uint8_t> bytes;
            uint64_t generation = 0;
        };

        std::unordered_map<uint32_t, Params> g_packets; // load packet address -> its parameters
        std::map<uint32_t, Upload> g_uploads;            // by destination block (x = y = 0 uploads; others logged)
        uint64_t g_generation = 0;

        struct Cached
        {
            std::shared_ptr<bozo::render::Texture> texture;
            uint64_t pixels = 0, palette = 0;
        };
        std::unordered_map<uint64_t, Cached> g_cache;

        bool trace()
        {
            static const bool on = [] { const char *e = std::getenv("ONI_TEX_TRACE"); return e && *e == '1'; }();
            return on;
        }

        int16_t s16(uint32_t v) { return static_cast<int16_t>(v & 0xffffu); }

        uint32_t bitsPerPixel(uint32_t psm)
        {
            switch (psm)
            {
            case 0x00: return 32; // CT32
            case 0x01: return 24; // CT24
            case 0x02: case 0x0a: return 16; // CT16, CT16S
            case 0x13: return 8;  // T8
            case 0x14: return 4;  // T4
            default: return 0;
            }
        }

        // The block address of pixel (x, y) in a buffer of `dbw` 64-pixel columns starting at `dbp`, for the 32-bit
        // formats (8 x 8-pixel blocks, 32 to a 64 x 32 page, in the GS's block order). Only block-aligned positions.
        bool blockAt(uint32_t psm, uint32_t dbw, uint32_t x, uint32_t y, uint32_t &block)
        {
            static const uint8_t order[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21},
                                                {2, 3, 6, 7, 18, 19, 22, 23},
                                                {8, 9, 12, 13, 24, 25, 28, 29},
                                                {10, 11, 14, 15, 26, 27, 30, 31}};
            if ((psm != 0x00 && psm != 0x01) || x % 8 || y % 8 || !dbw)
                return false;
            const uint32_t page = (y / 32) * dbw + x / 64;
            block += page * 32 + order[(y % 32) / 8][(x % 64) / 8];
            return true;
        }

        // sceGsSetDefLoadImage(lp, dbp, dbw, dpsm, x, y, w, h): the parameters are kept for the upload.
        void setDefLoadImage(uint8_t *, R5900Context *ctx, Runtime *)
        {
            Params p;
            p.dbp = static_cast<uint32_t>(s16(ctx->r[5].uw[0])) & 0x3fffu;
            p.dbw = static_cast<uint32_t>(s16(ctx->r[6].uw[0])) & 0x3fu;
            p.psm = static_cast<uint32_t>(s16(ctx->r[7].uw[0])) & 0x3fu;
            p.x = static_cast<uint32_t>(s16(ctx->r[8].uw[0])) & 0x7ffu;
            p.y = static_cast<uint32_t>(s16(ctx->r[9].uw[0])) & 0x7ffu;
            p.w = static_cast<uint32_t>(s16(ctx->r[10].uw[0])) & 0xfffu;
            p.h = static_cast<uint32_t>(s16(ctx->r[11].uw[0])) & 0xfffu;
            g_packets[ctx->r[4].uw[0]] = p;
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }

        // An upload kept by its destination block (Params: the transfer's BITBLTBUF / TRXPOS / TRXREG).
        void keep(const Params &p, const uint8_t *texels, uint64_t bytes)
        {
            // An upload at (x, y) of its buffer starts at another block: the palettes are packed four to a block group
            // (8 x 2 each at (0,0), (8,0), (0,8), (8,8)), and a TEX0's CBP names that block.
            uint32_t block = p.dbp;
            const bool placed = (p.x || p.y) && blockAt(p.psm, p.dbw, p.x, p.y, block);
            if ((p.x || p.y) && !placed)
            {
                if (trace())
                    std::printf("[oni:tex] upload at (%u,%u) of block %#x not kept\n", p.x, p.y, p.dbp);
                return;
            }
            Upload &u = g_uploads[block];
            u.p = p;
            u.bytes.assign(texels, texels + bytes);
            u.generation = ++g_generation;
            if (trace())
                std::printf("[oni:tex] upload block %#x bw %u psm %#x %ux%u\n", p.dbp, p.dbw, p.psm, p.w, p.h);
        }

        // sceGsExecLoadImage(lp, src): the upload's texels, copied as they are.
        void execLoadImage(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            const auto found = g_packets.find(ctx->r[4].uw[0]);
            const uint32_t src = ctx->r[5].uw[0] & 0x1ffffffu;
            if (found != g_packets.end())
            {
                const Params &p = found->second;
                const uint32_t bpp = bitsPerPixel(p.psm);
                const uint64_t bytes = (static_cast<uint64_t>(p.w) * p.h * bpp + 7) / 8;
                if (bpp && bytes && src + bytes <= 0x2000000u)
                    keep(p, ram + src, bytes);
                else if (trace())
                    std::printf("[oni:tex] upload psm %#x %ux%u from %#x skipped\n", p.psm, p.w, p.h, src);
            }
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }

        uint32_t colour32(const uint8_t *p) // GS RGBA, alpha 0x80 = opaque -> 255
        {
            const uint32_t a = std::min<uint32_t>(255u, p[3] * 2u);
            return p[0] | p[1] << 8 | p[2] << 16 | a << 24;
        }

        uint32_t colour16(uint16_t v, bool aem)
        {
            const uint32_t r = (v & 31u) << 3, g = (v >> 5 & 31u) << 3, b = (v >> 10 & 31u) << 3;
            uint32_t a = (v & 0x8000u) ? 255u : 0u; // TEXA TA1 = 0x80, TA0 = 0 as Ot_draw sets them
            if (aem && !(r | g | b))
                a = 0;
            return r | g << 8 | b << 16 | a << 24;
        }

        // A palette entry: CT32 or CT16, stored as the game uploaded it (CSM1: 16 x 16 for T8, 8 x 2 for T4).
        uint32_t paletteEntry(const Upload &clut, uint32_t index)
        {
            uint32_t x, y;
            if (clut.p.w >= 16)
                bozo::render::csm1Position(index & 0xffu, x, y);
            else
            {
                x = index & 7u;
                y = index >> 3;
            }
            if (x >= clut.p.w || y >= clut.p.h)
                return 0;
            const uint32_t at = y * clut.p.w + x;
            if (clut.p.psm == 0x00)
                return at * 4 + 4 <= clut.bytes.size() ? colour32(clut.bytes.data() + at * 4) : 0;
            uint16_t v = 0;
            if (at * 2 + 2 <= clut.bytes.size())
                std::memcpy(&v, clut.bytes.data() + at * 2, 2);
            return colour16(v, false);
        }
    } // namespace

    std::shared_ptr<const bozo::render::Texture> fromTex0(uint64_t tex0)
    {
        const uint32_t tbp = static_cast<uint32_t>(tex0 & 0x3fffu);
        const uint32_t psm = static_cast<uint32_t>(tex0 >> 20 & 0x3fu);
        const uint32_t tw = 1u << (tex0 >> 26 & 0xfu), th = 1u << (tex0 >> 30 & 0xfu);
        const uint32_t cbp = static_cast<uint32_t>(tex0 >> 37 & 0x3fffu);
        const uint32_t csa = static_cast<uint32_t>(tex0 >> 56 & 0x1fu);
        const auto pixels = g_uploads.find(tbp);
        if (pixels == g_uploads.end())
        {
            if (trace())
                std::printf("[oni:tex] no upload for TEX0 %016llx (block %#x)\n", (unsigned long long)tex0, tbp);
            return nullptr;
        }
        const Upload &px = pixels->second;
        const bool indexed = psm == 0x13 || psm == 0x14;
        const Upload *clut = nullptr;
        if (indexed)
        {
            const auto c = g_uploads.find(cbp);
            if (c == g_uploads.end())
            {
                if (trace())
                    std::printf("[oni:tex] no palette for TEX0 %016llx (block %#x)\n", (unsigned long long)tex0, cbp);
                return nullptr;
            }
            clut = &c->second;
        }
        const uint64_t key = tex0 & 0x1fffffffffffffffull; // without CLD
        Cached &cached = g_cache[key];
        if (cached.texture && cached.pixels == px.generation && cached.palette == (clut ? clut->generation : 0))
            return cached.texture;
        auto t = std::make_shared<bozo::render::Texture>();
        t->width = tw;
        t->height = th;
        t->rgba.assign(static_cast<size_t>(tw) * th, 0);
        t->key = key ^ 0x6f6e695465780000ull;
        t->asset = true;
        const uint32_t uploadBpp = bitsPerPixel(px.p.psm);
        const uint32_t w = std::min(tw, px.p.w), h = std::min(th, px.p.h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint64_t bit = (static_cast<uint64_t>(y) * px.p.w + x) * uploadBpp;
                const size_t at = static_cast<size_t>(bit / 8);
                if (at + (uploadBpp + 7) / 8 > px.bytes.size())
                    continue;
                const uint8_t *b = px.bytes.data() + at;
                uint32_t c = 0;
                switch (psm)
                {
                case 0x00: c = colour32(b); break;
                case 0x01: c = b[0] | b[1] << 8 | b[2] << 16 | 0xffu << 24; break;
                case 0x02: case 0x0a: { uint16_t v; std::memcpy(&v, b, 2); c = colour16(v, true); break; }
                case 0x13: c = paletteEntry(*clut, csa * 16u + b[0]); break;
                case 0x14: c = paletteEntry(*clut, csa * 16u + ((bit & 4) ? b[0] >> 4 : b[0] & 15u)); break;
                default: break;
                }
                t->rgba[static_cast<size_t>(y) * tw + x] = c;
            }
        static uint64_t version = 0;
        t->version = ++version;
        cached.texture = t;
        cached.pixels = px.generation;
        cached.palette = clut ? clut->generation : 0;
        if (trace())
            std::printf("[oni:tex] decoded TEX0 %016llx: %ux%u psm %#x from block %#x (upload %ux%u psm %#x)\n",
                        (unsigned long long)tex0, tw, th, psm, tbp, px.p.w, px.p.h, px.p.psm);
        // ONI_TEX_DUMP=<folder>: every decoded texture as a PAM image (RGBA), named by its TEX0.
        if (const char *dir = std::getenv("ONI_TEX_DUMP"))
        {
            char name[512];
            std::snprintf(name, sizeof name, "%s/%016llx.pam", dir, (unsigned long long)key);
            if (FILE *f = std::fopen(name, "wb"))
            {
                std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", tw, th);
                std::fwrite(t->rgba.data(), 4, t->rgba.size(), f);
                std::fclose(f);
            }
        }
        return t;
    }

    void upload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, const uint8_t *texels, size_t bytes)
    {
        Params p;
        p.dbp = static_cast<uint32_t>(bitbltbuf >> 32) & 0x3fffu;
        p.dbw = static_cast<uint32_t>(bitbltbuf >> 48) & 0x3fu;
        p.psm = static_cast<uint32_t>(bitbltbuf >> 56) & 0x3fu;
        p.x = static_cast<uint32_t>(trxpos >> 32) & 0x7ffu;
        p.y = static_cast<uint32_t>(trxpos >> 48) & 0x7ffu;
        p.w = static_cast<uint32_t>(trxreg) & 0xfffu;
        p.h = static_cast<uint32_t>(trxreg >> 32) & 0xfffu;
        const uint32_t bpp = bitsPerPixel(p.psm);
        const uint64_t need = (static_cast<uint64_t>(p.w) * p.h * bpp + 7) / 8;
        if (!bpp || !need || bytes < need)
        {
            if (trace())
                std::printf("[oni:tex] GIF upload psm %#x %ux%u (%zu bytes) skipped\n", p.psm, p.w, p.h, bytes);
            return;
        }
        keep(p, texels, need);
    }

    void install(bozo::port::Registry &r)
    {
        r.registerFunction(0x100ec8, setDefLoadImage); // sceGsSetDefLoadImage
        r.registerFunction(0x1010b0, execLoadImage);   // sceGsExecLoadImage
    }
} // namespace oni::tex
