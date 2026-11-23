// Onimusha's backgrounds: the pre-rendered rooms, logos and menus are IPU streams (`ipum` files: a 16-byte header, then
// one frame of MPEG-2 intra macroblocks). On the console Cut_set / Cut_set_onmemory put the file at 0x7f3440 and
// decodeIPU 0x1221a0 runs the IPU over it (IDEC, CSC to RGB32), splitting the 640 x 448 picture into its two fields:
// the even lines at 0xa69c80 and the odd lines at 0x9ddc80 (640 x 224 RGBA each); Bg_disp 0x122040 then sends the
// field shown to the GS every frame.
//
// Natively: decodeIPU decodes the same stream with FFmpeg's IPU decoder and the IPU's colour conversion and writes the
// same two field buffers (the game's own code may read or patch them: cut_replace). Bg_disp still runs (its camera,
// lights, shadows, BG movies); the frame publishes the picture rebuilt from the field buffers as its backdrop.
#include "oni_render.h"

#include "bozo/ee/dispatcher.h"
#include "oni_functions.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace oni::bg
{
    namespace
    {
        constexpr uint32_t kStream = 0x7f3440;   // the IPU file as Cut_set read it (header included)
        constexpr uint32_t kToIpuChain = 0x90b440; // setToIPUtags' chain: its first tag's address is the stream's start
        constexpr uint32_t kStreamSize = 0x214380; // its size in bytes (gp - 0x6df0)
        constexpr uint32_t kFieldEven = 0xa69c80; // lines 0, 2, 4 ... of the picture, 640 x 224 RGBA
        constexpr uint32_t kFieldOdd = 0x9ddc80;  // lines 1, 3, 5 ...
        constexpr int kWidth = 640, kHeight = 448;

        uint32_t read32(const uint8_t *ram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, ram + (address & 0x1ffffffu), 4);
            return v;
        }

        bool trace()
        {
            static const bool on = [] { const char *e = std::getenv("ONI_BG_TRACE"); return e && *e == '1'; }();
            return on;
        }

        // The IPU's CSC (YCbCr to RGB32): BT.601 studio range, the coefficients in 128ths as the IPU's converter has
        // them; alpha from the threshold registers left at 0: every pixel 0x80 (opaque).
        uint32_t csc(int y, int cb, int cr)
        {
            const int luma = 149 * std::max(0, y - 16); // 1.164 in 128ths
            cb -= 128;
            cr -= 128;
            auto clamp = [](int v) { return static_cast<uint32_t>(std::clamp((v + 64) >> 7, 0, 255)); };
            const uint32_t r = clamp(luma + 204 * cr);            // 1.594
            const uint32_t g = clamp(luma - 50 * cb - 104 * cr);  // -0.391, -0.813
            const uint32_t b = clamp(luma + 258 * cb);            // 2.016
            return r | g << 8 | b << 16 | 0x80u << 24;
        }

        struct Decoder
        {
            AVCodecContext *context = nullptr;
            AVPacket *packet = nullptr;
            AVFrame *frame = nullptr;
            Decoder()
            {
                const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_IPU);
                if (!codec)
                    throw std::runtime_error("FFmpeg has no IPU decoder (scripts/build_ffmpeg.sh)");
                context = avcodec_alloc_context3(codec);
                context->width = kWidth;
                context->height = kHeight;
                if (avcodec_open2(context, codec, nullptr) < 0)
                    throw std::runtime_error("cannot open FFmpeg's IPU decoder");
                packet = av_packet_alloc();
                frame = av_frame_alloc();
            }
            ~Decoder()
            {
                av_frame_free(&frame);
                av_packet_free(&packet);
                avcodec_free_context(&context);
            }
        };

        // The picture published as the frame's backdrop, rebuilt from the field buffers when the game shows it.
        std::shared_ptr<bozo::render::Texture> g_picture;
        uint64_t g_version = 0;

        // decodeIPU(): the stream the to-IPU DMA chain (0x90b440, setToIPUtags: REF tags over the stream after its
        // 16-byte header) points at, to the two field buffers. Cut_set / Cut_set_onmemory put an `ipum` file at
        // 0x7f3440; Cut_change (the rooms) puts a cut block there whose IPU stream lies after the block's Z buffer.
        void decodeIpu(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            static Decoder decoder;
            const uint32_t size = read32(ram, kStreamSize);
            const uint32_t start = read32(ram, kToIpuChain + 4) & 0x1ffffffu; // the first tag's address
            const uint8_t *file = ram + ((start >= 16 ? start : kStream + 16) - 16);
            // ONI_BG_DUMP=<folder>: each stream decodeIPU is given, as it is in EE memory (header included).
            if (const char *dir = std::getenv("ONI_BG_DUMP"); dir && size && size <= 0x200000)
            {
                static unsigned n = 0;
                char name[512];
                std::snprintf(name, sizeof name, "%s/cut_%03u.bin", dir, n++);
                if (FILE *f = std::fopen(name, "wb"))
                {
                    std::fwrite(file, 1, size, f);
                    std::fclose(f);
                }
            }
            // An `ipum` header names its size; a room cut's header is its own (the picture is always 640 x 448).
            const bool ipum = std::memcmp(file, "ipum", 4) == 0;
            const uint16_t width = ipum ? static_cast<uint16_t>(file[8] | file[9] << 8) : kWidth;
            const uint16_t height = ipum ? static_cast<uint16_t>(file[10] | file[11] << 8) : kHeight;
            if (size <= 16 || size > 0x200000 || width != kWidth || height != kHeight)
            {
                std::fprintf(stderr, "[oni:bg] not a 640 x 448 IPU stream at 0x%x (size %u, %ux%u), called from 0x%x\n",
                             kStream, size, width, height, ctx->r[31].uw[0]);
                ctx->pc = ctx->r[31].uw[0];
                return;
            }
            // FFmpeg's bit reader may read past the end: a padded copy. Its IPU decoder wants exactly one 32-bit end code
            // after the picture; Onimusha's files end with two (0x000001b0, then the sequence end 0x000001b1): the packet
            // stops after the first one.
            uint32_t bytes = size - 16;
            for (uint32_t at = 0; at + 4 <= bytes; ++at)
                if (file[16 + at] == 0 && file[17 + at] == 0 && file[18 + at] == 1 && file[19 + at] == 0xb0)
                {
                    bytes = at + 4; // the picture's macroblocks never contain a start code
                    break;
                }
            std::vector<uint8_t> data(bytes + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            std::memcpy(data.data(), file + 16, bytes);
            av_packet_unref(decoder.packet);
            decoder.packet->data = data.data();
            decoder.packet->size = static_cast<int>(bytes);
            int got = avcodec_send_packet(decoder.context, decoder.packet);
            if (got >= 0)
                got = avcodec_receive_frame(decoder.context, decoder.frame);
            decoder.packet->data = nullptr;
            decoder.packet->size = 0;
            if (got < 0)
            {
                std::fprintf(stderr, "[oni:bg] IPU decode failed (%d)\n", got);
                avcodec_flush_buffers(decoder.context);
                ctx->pc = ctx->r[31].uw[0];
                return;
            }
            const AVFrame *f = decoder.frame;
            for (int y = 0; y < kHeight; ++y)
            {
                const uint8_t *Y = f->data[0] + y * f->linesize[0];
                const uint8_t *U = f->data[1] + (y / 2) * f->linesize[1];
                const uint8_t *V = f->data[2] + (y / 2) * f->linesize[2];
                uint8_t *row = ram + ((y & 1) ? kFieldOdd : kFieldEven) + (y / 2) * kWidth * 4;
                for (int x = 0; x < kWidth; ++x)
                {
                    const uint32_t p = csc(Y[x], U[x / 2], V[x / 2]);
                    std::memcpy(row + x * 4, &p, 4);
                }
            }
            av_frame_unref(decoder.frame);
            if (const char *dir = std::getenv("ONI_BG_DUMP")) // the picture too, from the field buffers (PAM, RGBA)
            {
                static unsigned n = 0;
                char name[512];
                std::snprintf(name, sizeof name, "%s/picture_%03u.pam", dir, n++);
                if (FILE *f = std::fopen(name, "wb"))
                {
                    std::fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", kWidth, kHeight);
                    for (int y = 0; y < kHeight; ++y)
                        std::fwrite(ram + ((y & 1) ? kFieldOdd : kFieldEven) + (y / 2) * kWidth * 4, 4, kWidth, f);
                    std::fclose(f);
                }
            }
            if (trace())
                std::printf("[oni:bg] decoded %u bytes, flags %02x\n", size, file[16]);
            g_picture.reset(); // rebuilt from the fields when shown
            ctx->pc = ctx->r[31].uw[0];
        }

        // Bg_disp(work): the game's own (camera, lights, shadows, the BG movie), then the picture into the frame, as the
        // GS has it: under the models drawn after it and darkened by the shadows' pass with them. The room's depth is
        // already there (drawDepth, the frame's first draw); the picture neither tests nor writes it.
        void bgDisp(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            Bg_disp_0x122040(ram, ctx, rt);
            if (read32(ram, 0x2828ac) & 0x80000000u) // the same flag Bg_disp skips everything on
                return;
            const auto shown = picture(ram);
            bozo::render::Material m;
            m.textured = true;
            m.blend = bozo::render::Material::Blend::Opaque;
            m.depthTest = false;
            m.depthWrite = false;
            m.clampU = m.clampV = true;
            m.tex0 = shown->key;
            const bozo::render::Screen s = oni::render::screen();
            bozo::render::Vertex corner[4];
            for (int i = 0; i < 4; ++i)
            {
                corner[i].x = s.originX + ((i & 1) ? s.width : 0.0f);
                corner[i].y = s.originY + ((i >> 1) ? s.height : 0.0f);
                corner[i].u = (i & 1) ? 1.0f : 0.0f;
                corner[i].v = (i >> 1) ? 1.0f : 0.0f;
                corner[i].rgba = 0x80808080u;
            }
            const bozo::render::Vertex quad[6] = {corner[0], corner[1], corner[2], corner[1], corner[3], corner[2]};
            static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            bozo::render::Renderer::instance().draw(m, kIdentity, quad, 6, shown);
        }

        // The room's depth: Bgz_disp 0x123800 sends, at the end of each frame, the cut's Z buffer (one field: 640 x 224
        // 24-bit Z, PSMZ24, to the ZBUF at block 0x1180) for the next frame's models, the GS's ZTST then hiding them
        // behind the pre-rendered scenery. The chains are prebuilt (0x21c050: the even lines' Z, 0x21bd30: the odd
        // lines', each a REF of the GIF registers and a REF of the IMAGE data): natively both fields make one 640 x 448
        // depth image, written as the next frame's first draw (Material::depthFromTexture).
        constexpr uint32_t kZEvenChain = 0x21c050, kZOddChain = 0x21bd30;
        std::shared_ptr<bozo::render::Texture> g_depth;
        uint64_t g_depthVersion = 0;
        bool g_depthDue = false; // Bgz_disp ran this frame: the depth goes under the next one

        void bgzDisp(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            const uint32_t even = read32(ram, kZEvenChain + 16 + 4) & 0x1ffffffu, odd = read32(ram, kZOddChain + 16 + 4) & 0x1ffffffu;
            const uint32_t qwc = read32(ram, kZEvenChain + 16) & 0xffffu;
            ctx->pc = ctx->r[31].uw[0];
            if (qwc * 16u != kWidth * (kHeight / 2) * 3u || even + qwc * 16u > 0x2000000u || odd + qwc * 16u > 0x2000000u)
                return; // not a 640 x 224 Z24 field
            if (!g_depth)
            {
                g_depth = std::make_shared<bozo::render::Texture>();
                g_depth->width = kWidth;
                g_depth->height = kHeight;
                g_depth->rgba.resize(static_cast<size_t>(kWidth) * kHeight);
                g_depth->key = 0x6f6e695a00000000ull; // "oniZ"
            }
            bool changed = false;
            for (int y = 0; y < kHeight; ++y)
            {
                const uint8_t *row = ram + ((y & 1) ? odd : even) + static_cast<size_t>(y / 2) * kWidth * 3;
                uint32_t *dst = g_depth->rgba.data() + static_cast<size_t>(y) * kWidth;
                for (int x = 0; x < kWidth; ++x)
                {
                    const uint32_t z = row[3 * x] | row[3 * x + 1] << 8 | row[3 * x + 2] << 16 | 0xff000000u;
                    if (dst[x] != z)
                    {
                        dst[x] = z;
                        changed = true;
                    }
                }
            }
            if (changed)
                g_depth->version = ++g_depthVersion;
            g_depthDue = true;
        }

        // sceIpuSync(mode, timeout): waits for the IPU; nothing runs.
        void ipuSync(uint8_t *, R5900Context *ctx, Runtime *)
        {
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }
    } // namespace

    std::shared_ptr<const bozo::render::Texture> picture(const uint8_t *ram)
    {
        if (!g_picture)
        {
            auto t = std::make_shared<bozo::render::Texture>();
            t->width = kWidth;
            t->height = kHeight;
            t->rgba.resize(static_cast<size_t>(kWidth) * kHeight);
            t->key = 0x6f6e694267000000ull; // "oniBg"
            t->version = ++g_version;
            t->asset = true;
            g_picture = std::move(t);
        }
        // From the field buffers each time: the game may patch them (cut_replace). Alpha 0x80 is opaque.
        auto &rgba = g_picture->rgba;
        bool changed = false;
        for (int y = 0; y < kHeight; ++y)
        {
            const uint8_t *row = ram + ((y & 1) ? kFieldOdd : kFieldEven) + (y / 2) * kWidth * 4;
            for (int x = 0; x < kWidth; ++x)
            {
                uint32_t p;
                std::memcpy(&p, row + x * 4, 4);
                const uint32_t a = std::min<uint32_t>(255u, (p >> 24) * 2u);
                p = (p & 0xffffffu) | a << 24;
                uint32_t &dst = rgba[static_cast<size_t>(y) * kWidth + x];
                if (dst != p)
                {
                    dst = p;
                    changed = true;
                }
            }
        }
        if (changed)
            g_picture->version = ++g_version;
        return g_picture;
    }

    void stateLoaded()
    {
        g_picture.reset();
        g_depth.reset();
        g_depthDue = false;
    }

    void drawDepth()
    {
        if (!g_depthDue || !g_depth)
            return;
        g_depthDue = false;
        static const bool off = [] { const char *e = std::getenv("ONI_BGZ"); return e && *e == '0'; }();
        if (off)
            return;
        bozo::render::Material m;
        m.textured = true;
        m.depthFromTexture = true;
        m.nearest = true;
        m.clampU = m.clampV = true;
        m.blend = bozo::render::Material::Blend::Opaque;
        m.colourMask = 0;
        m.depthTest = false;
        m.depthWrite = true;
        m.tex0 = 0x6f6e695a00000000ull;
        const bozo::render::Screen s = oni::render::screen();
        bozo::render::Vertex corner[4];
        for (int i = 0; i < 4; ++i)
        {
            corner[i].x = s.originX + ((i & 1) ? s.width : 0.0f);
            corner[i].y = s.originY + ((i >> 1) ? s.height : 0.0f);
            corner[i].u = (i & 1) ? 1.0f : 0.0f;
            corner[i].v = (i >> 1) ? 1.0f : 0.0f;
            corner[i].rgba = 0x80808080u;
        }
        const bozo::render::Vertex quad[6] = {corner[0], corner[1], corner[2], corner[1], corner[3], corner[2]};
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        bozo::render::Renderer::instance().draw(m, kIdentity, quad, 6, g_depth);
    }

    void install(bozo::port::Registry &r)
    {
        r.registerFunction(0x123800, bgzDisp); // Bgz_disp
        r.registerFunction(0x1221a0, decodeIpu); // decodeIPU
        r.registerFunction(0x122190, decodeIpu); // divFrameBuff: "j decodeIPU" (a direct jump runs the guest code)
        r.registerFunction(0x122040, bgDisp);    // Bg_disp
        r.registerFunction(0x10ae48, ipuSync);   // sceIpuSync
    }
} // namespace oni::bg
