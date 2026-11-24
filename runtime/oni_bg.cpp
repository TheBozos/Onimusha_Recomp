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
#include "oni_tex.h"
#include "oni_vu1.h"

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
#include <map>
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
            explicit Decoder(int width = kWidth, int height = kHeight)
            {
                const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_IPU);
                if (!codec)
                    throw std::runtime_error("FFmpeg has no IPU decoder (scripts/build_ffmpeg.sh)");
                context = avcodec_alloc_context3(codec);
                context->width = width;
                context->height = height;
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
            (void)kIdentity;
            oni::vu1::queueDraw(m, quad, 6, shown);
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

        void bgzDisp(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            // ONI_FORCE_CUT=<frames>:<cut> (a test): after that many frames, the room changes to that cut as its camera
            // regions would make it (cut_chg_main 0x136930).
            static int forceAfter = -1, forceCut = 0;
            static bool parsed = false;
            if (!parsed)
            {
                parsed = true;
                if (const char *e = std::getenv("ONI_FORCE_CUT"))
                    std::sscanf(e, "%d:%d", &forceAfter, &forceCut);
            }
            if (trace())
            {
                static uint64_t calls = 0;
                if (++calls % 30 == 0)
                    std::printf("[oni:bg] Bgz_disp: %llu calls\n", (unsigned long long)calls);
            }
            if (forceAfter >= 0 && forceAfter-- == 0)
            {
                const R5900Context saved = *ctx;
                ctx->r[4].ud[0] = static_cast<uint64_t>(forceCut);
                ctx->r[5].ud[0] = 0;
                std::printf("[oni:bg] forcing cut %d\n", forceCut);
                cut_chg_main_0x136930(ram, ctx, rt);
                *ctx = saved;
            }
            const uint32_t even = read32(ram, kZEvenChain + 16 + 4) & 0x1ffffffu, odd = read32(ram, kZOddChain + 16 + 4) & 0x1ffffffu;
            const uint32_t qwc = read32(ram, kZEvenChain + 16) & 0xffffu;
            ctx->pc = ctx->r[31].uw[0];
            if (qwc * 16u != kWidth * (kHeight / 2) * 3u || even + qwc * 16u > 0x2000000u || odd + qwc * 16u > 0x2000000u)
                return; // not a 640 x 224 Z24 field
            // A changed depth is a new texture (as the picture).
            static std::vector<uint32_t> zs;
            zs.resize(static_cast<size_t>(kWidth) * kHeight);
            for (int y = 0; y < kHeight; ++y)
            {
                const uint8_t *row = ram + ((y & 1) ? odd : even) + static_cast<size_t>(y / 2) * kWidth * 3;
                uint32_t *dst = zs.data() + static_cast<size_t>(y) * kWidth;
                for (int x = 0; x < kWidth; ++x)
                    dst[x] = row[3 * x] | row[3 * x + 1] << 8 | row[3 * x + 2] << 16 | 0xff000000u;
            }
            if (!g_depth || g_depth->rgba != zs)
            {
                auto t = std::make_shared<bozo::render::Texture>();
                t->width = kWidth;
                t->height = kHeight;
                t->rgba = zs;
                t->key = 0x6f6e695a00000000ull; // "oniZ"
                t->version = ++g_depthVersion;
                g_depth = std::move(t);
            }
            // Not under a trick box's puzzle (set06: Status_flg 0x2828ac bit 30, Flg_on(.., 1)): its tiles are 3D models
            // seen through the puzzle's own close-up camera (set06_camera) over the box's picture, and the room's Z
            // buffer (the last cut's, from another camera: the puzzle loads no Z) hid most of them (stage 1 room 16).
            g_depthDue = (read32(ram, 0x2828ac) & 0x40000000u) == 0;
        }

        // The animated backgrounds (IMAGE/BGS/MOV*.IPU: `ipum` movies of a patch of the room, e.g. 192 x 256 for
        // MOV000L): Cut_change's Movie_init sets them up when the cut's table entry says so, Bg_disp's MoviePlay
        // advances each one and, when a frame is due, points the to-IPU chain (0x7f2c00, REF tags) at the frame's data
        // and calls DecodeIPU_Movie(params, dest, entry) 0x197b80: IPU_CTRL from params, the alpha threshold from the
        // entry (+0x3a, SETTH TH0 = TH1), then IDEC (RGB32) of entry+0x18 x entry+0x1a macroblocks, each 16 x 16 pixels
        // (1 KB) one after the other to dest, where the movie's prebuilt GIF chains (setLoadImageTags) take them to the
        // frame. Natively: the frame decoded by FFmpeg's IPU decoder at the patch's size, written as the IPU writes it.
        constexpr uint32_t kMovieChain = 0x7f2c00;
        std::map<uint32_t, std::unique_ptr<Decoder>> g_movieDecoders; // by width << 16 | height

        void decodeIpuMovie(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            ctx->pc = ctx->r[31].uw[0];
            const uint32_t entry = ctx->r[5].uw[0] & 0x1ffffffu, dest = ctx->r[6].uw[0] & 0x1ffffffu;
            int16_t wh[2];
            std::memcpy(wh, ram + entry + 0x18, 4);
            const uint32_t threshold = ram[entry + 0x3a];
            // The frame's data, as the chain's REF tags give it (a REFE ends it).
            std::vector<uint8_t> data;
            for (uint32_t tag = kMovieChain, guard = 0; guard < 64; ++guard, tag += 16)
            {
                const uint32_t head = read32(ram, tag), address = read32(ram, tag + 4) & 0x1ffffffu;
                const uint32_t bytes = (head & 0xffffu) * 16u;
                if (address + bytes <= 0x2000000u)
                    data.insert(data.end(), ram + address, ram + address + bytes);
                if (((head >> 28) & 7u) == 0u)
                    break;
            }
            static const bool trace = std::getenv("ONI_BGMV_TRACE") != nullptr;
            if (trace)
                std::printf("[oni:bgmv] DecodeIPU_Movie params %x dest %x entry %x: %d x %d macroblocks, threshold %u, %zu bytes"
                            " %02x %02x %02x %02x\n", ctx->r[4].uw[0], dest, entry, wh[0], wh[1], threshold, data.size(),
                            data.size() > 0 ? data[0] : 0, data.size() > 1 ? data[1] : 0, data.size() > 2 ? data[2] : 0,
                            data.size() > 3 ? data[3] : 0);
            const int mbw = wh[0], mbh = wh[1];
            if (mbw <= 0 || mbh <= 0 || mbw > 40 || mbh > 28 || data.size() < 8 ||
                dest + static_cast<uint32_t>(mbw * mbh) * 1024u > 0x2000000u)
                return;
            auto &decoder = g_movieDecoders[static_cast<uint32_t>(mbw) << 16 | static_cast<uint32_t>(mbh)];
            if (!decoder)
                decoder = std::make_unique<Decoder>(mbw * 16, mbh * 16);
            // One frame: up to its end code (0x000001b0), as the room pictures.
            size_t bytes = data.size();
            for (size_t at = 0; at + 4 <= data.size(); ++at)
                if (data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1 && data[at + 3] == 0xb0)
                {
                    bytes = at + 4;
                    break;
                }
            std::vector<uint8_t> packet(bytes + AV_INPUT_BUFFER_PADDING_SIZE, 0);
            std::memcpy(packet.data(), data.data(), bytes);
            av_packet_unref(decoder->packet);
            decoder->packet->data = packet.data();
            decoder->packet->size = static_cast<int>(bytes);
            int got = avcodec_send_packet(decoder->context, decoder->packet);
            if (got >= 0)
                got = avcodec_receive_frame(decoder->context, decoder->frame);
            decoder->packet->data = nullptr;
            decoder->packet->size = 0;
            if (got < 0)
            {
                std::fprintf(stderr, "[oni:bgmv] IPU movie frame decode failed (%d)\n", got);
                avcodec_flush_buffers(decoder->context);
                return;
            }
            // RGB32 by macroblock (raster order of the patch), alpha from the threshold as the IPU's CSC sets it.
            const AVFrame *f = decoder->frame;
            for (int mb = 0; mb < mbw * mbh; ++mb)
            {
                const int mx = (mb % mbw) * 16, my = (mb / mbw) * 16;
                uint8_t *out = ram + dest + static_cast<uint32_t>(mb) * 1024u;
                for (int y = 0; y < 16; ++y)
                {
                    const uint8_t *Y = f->data[0] + (my + y) * f->linesize[0] + mx;
                    const uint8_t *U = f->data[1] + ((my + y) / 2) * f->linesize[1] + mx / 2;
                    const uint8_t *V = f->data[2] + ((my + y) / 2) * f->linesize[2] + mx / 2;
                    for (int x = 0; x < 16; ++x)
                    {
                        uint32_t p = csc(Y[x], U[x / 2], V[x / 2]);
                        const uint32_t r = p & 0xff, g = (p >> 8) & 0xff, b = (p >> 16) & 0xff;
                        if (r < threshold && g < threshold && b < threshold)
                            p &= 0xffffffu; // below TH0: transparent
                        std::memcpy(out + (y * 16 + x) * 4, &p, 4);
                    }
                }
            }
            av_frame_unref(decoder->frame);
        }

        // The movie's loadImage(chain) 0x197f10 (movie.c's own, not libmpeg's): GIF (PATH3) DMA on one of the two chains
        // Movie_init built (0xaf5c80 for the even lines, 0xaf5c80 + 0x1a500 for the odd ones, one sent a frame: the
        // field shown), IMAGE transfers of the decoded macroblocks' lines into the frame buffer at the patch's place.
        // Natively both chains' transfers go into the room picture's field buffers (0xa69c80 even, 0x9ddc80 odd): the
        // picture drawn under the frame is rebuilt from them.
        constexpr uint32_t kMovieImageEven = 0xaf5c80, kMovieImageStride = 0x1a500;
        void movieImages(uint8_t *ram, uint32_t chain, uint32_t field, bool sent)
        {
            static const bool trace = std::getenv("ONI_BGMV_TRACE") != nullptr;
            static int logged = 0;
            // The chain's data as one GIF stream (an IMAGE tag's texels may come in the next DMA tag: a REF to the
            // decoded macroblocks).
            std::vector<uint8_t> stream;
            uint32_t at = chain;
            for (int guard = 0; guard < 8192; ++guard)
            {
                const uint32_t head = read32(ram, at), address = read32(ram, at + 4) & 0x1ffffffu;
                const uint32_t qwc = head & 0xffffu, id = (head >> 28) & 7u;
                const uint32_t data = (id == 3 || id == 4 || id == 0) ? address : (at + 16) & 0x1ffffffu;
                if (data + qwc * 16u <= 0x2000000u)
                    stream.insert(stream.end(), ram + data, ram + data + qwc * 16u);
                if (id == 0 || id == 7)
                    break;
                at = (id == 3 || id == 4) ? at + 16 : (id == 2 ? address : at + 16 + 16 * qwc);
            }
            // A movie into a texture rather than the frame (Movie_init's setLoadImageTags2, a movie of type 2: block
            // 0x1a40, the shadows' buffer too; stage 5 room 19's water caustics, stage 3 room 7's pond, drawn through
            // StencilMapPolygon's strips): the chain goes to the GS in its place among the frame's draws, so that a
            // read of the block after it is the movie and one after the shadows are drawn there is the shadows.
            if (stream.size() >= 32)
            {
                uint64_t ad[2];
                std::memcpy(ad, stream.data() + 16, 16);
                const uint64_t tag0 = [&] { uint64_t t; std::memcpy(&t, stream.data(), 8); return t; }();
                const uint32_t dbp = static_cast<uint32_t>((ad[0] >> 32) & 0x3fff);
                if (((tag0 >> 58) & 3) == 0 && (ad[1] & 0xff) == 0x50 && dbp != 0u && dbp != 0x8c0u)
                {
                    if (sent) // (the chain the game sends this frame: the field shown)
                        oni::vu1::runGifStream(std::move(stream), chain);
                    return;
                }
            }
            uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
            const size_t qwords = stream.size() / 16;
            for (size_t q = 0; q < qwords;)
            {
                uint64_t tag[2];
                std::memcpy(tag, stream.data() + 16 * q, 16);
                ++q;
                const uint32_t nloop = tag[0] & 0x7fff, flg = (tag[0] >> 58) & 3;
                if (flg == 0) // PACKED: A+D
                {
                    for (uint32_t l = 0; l < nloop && q < qwords; ++l, ++q)
                    {
                        uint64_t ad[2];
                        std::memcpy(ad, stream.data() + 16 * q, 16);
                        const uint32_t reg = ad[1] & 0xff;
                        if (reg == 0x50) bitbltbuf = ad[0];
                        else if (reg == 0x51) trxpos = ad[0];
                        else if (reg == 0x52) trxreg = ad[0];
                    }
                    continue;
                }
                if (flg == 2 && q + nloop <= qwords)
                {
                    const uint32_t x0 = (trxpos >> 32) & 0x7ff, y0 = (trxpos >> 48) & 0x7ff;
                    const uint32_t w = trxreg & 0xfff, h = (trxreg >> 32) & 0xfff;
                    const uint32_t psm = (bitbltbuf >> 56) & 0x3f;
                    const uint32_t dbp = static_cast<uint32_t>((bitbltbuf >> 32) & 0x3fff);
                    if (trace && logged < 4)
                    {
                        ++logged;
                        std::printf("[oni:bgmv] image dbp %x dbw %u psm %x at (%u,%u) %ux%u, %u qwords (field %u)\n", dbp,
                                    static_cast<uint32_t>((bitbltbuf >> 48) & 0x3f), psm, x0, y0, w, h, nloop, field);
                    }
                    const uint8_t *texels = stream.data() + 16 * q;
                    // A movie into a texture rather than the frame (stage 5 room 19's 256 x 256 water caustics: block
                    // 0x1a40): a texture upload, not the room's picture.
                    if (dbp != 0u && dbp != 0x8c0u)
                        oni::tex::upload(bitbltbuf, trxpos, trxreg, texels, static_cast<size_t>(nloop) * 16u);
                    else if (psm == 0 && x0 + w <= static_cast<uint32_t>(kWidth) && y0 + h <= static_cast<uint32_t>(kHeight / 2) &&
                        static_cast<uint64_t>(w) * h * 4 <= static_cast<uint64_t>(nloop) * 16)
                        for (uint32_t y = 0; y < h; ++y)
                            std::memcpy(ram + (field ? kFieldOdd : kFieldEven) + ((y0 + y) * kWidth + x0) * 4, texels + y * w * 4, w * 4);
                }
                q += nloop;
            }
        }

        void movieLoadImage(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            ctx->pc = ctx->r[31].uw[0];
            const uint32_t sent = ctx->r[4].uw[0] & 0x1ffffffu;
            movieImages(ram, kMovieImageEven, 0, sent == kMovieImageEven);
            movieImages(ram, kMovieImageEven + kMovieImageStride, 1, sent == kMovieImageEven + kMovieImageStride);
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
        // From the field buffers each time: the game may patch them (cut_replace, the background movies). Alpha 0x80 is
        // opaque. A changed picture is a new texture (the renderer knows a texture by its object, and may be reading the
        // previous one).
        static std::vector<uint32_t> rgba;
        rgba.resize(static_cast<size_t>(kWidth) * kHeight);
        for (int y = 0; y < kHeight; ++y)
        {
            const uint8_t *row = ram + ((y & 1) ? kFieldOdd : kFieldEven) + (y / 2) * kWidth * 4;
            uint32_t *dst = rgba.data() + static_cast<size_t>(y) * kWidth;
            for (int x = 0; x < kWidth; ++x)
            {
                uint32_t p;
                std::memcpy(&p, row + x * 4, 4);
                const uint32_t a = std::min<uint32_t>(255u, (p >> 24) * 2u);
                dst[x] = (p & 0xffffffu) | a << 24;
            }
        }
        if (!g_picture || g_picture->rgba != rgba)
        {
            auto t = std::make_shared<bozo::render::Texture>();
            t->width = kWidth;
            t->height = kHeight;
            t->rgba = rgba;
            t->key = 0x6f6e694267000000ull; // "oniBg"
            t->version = ++g_version;
            t->asset = true;
            g_picture = std::move(t);
        }
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
        (void)kIdentity;
        oni::vu1::queueDraw(m, quad, 6, g_depth);
    }

    // ONI_BGMV_TRACE=1: what Get_bgmv_adrs (a cut's background movie among the loaded ones) answers.
    void getBgmvAdrs(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        const int32_t cut = static_cast<int16_t>(ctx->r[4].uw[0]);
        const uint32_t ra = ctx->r[31].uw[0];
        Get_bgmv_adrs_0x196a90(ram, ctx, rt);
        std::printf("[oni:bgmv] Get_bgmv_adrs(%d) from %x = %x (cd_work loaded %u)\n", cut, ra, ctx->r[2].uw[0], ram[0x3dc67c]);
    }

    void install(bozo::port::Registry &r)
    {
        if (std::getenv("ONI_BGMV_TRACE"))
            r.registerFunction(0x196a90, getBgmvAdrs);
        r.registerFunction(0x123800, bgzDisp); // Bgz_disp
        r.registerFunction(0x1221a0, decodeIpu); // decodeIPU
        r.registerFunction(0x122190, decodeIpu); // divFrameBuff: "j decodeIPU" (a direct jump runs the guest code)
        r.registerFunction(0x122040, bgDisp);    // Bg_disp
        r.registerFunction(0x10ae48, ipuSync);   // sceIpuSync
        r.registerFunction(0x197b80, decodeIpuMovie); // DecodeIPU_Movie
        r.registerFunction(0x197f10, movieLoadImage); // movie.c's loadImage (the background movies' patch)
    }
} // namespace oni::bg
