// Onimusha's movies: movie.c is Sony's ezmpeg sample player (as Twisted Metal: Black's), extended with captions.
// Movie_task_exe(no) 0x1bd9b0 starts Movie_task 0x1bcf60 on its own thread and puts the main thread to sleep until the
// movie ends: readMpeg 0x1bd1f0 streams the PSS file (strFileRead), demuxes it (sceMpegDemuxPssRing: native libmpeg,
// FFmpeg), sends its PCM to the sound driver (audioDecSendToIOP) and starts the display; videoDecMain / decBs0 decode
// into the picture buffer voBuf 0x460ec0 (sceMpegGetPicture, dispSetTags), and vblankHandler 0x1bca40 (INTC vblank
// start) shows each picture for two fields through loadImage 0x1bca10 (a GIF DMA of the picture's chain), whose DMA
// end interrupt (handler_endimage 0x1bcea0) frees its buffer (voBufDecCount).
//
// Natively: libmpeg's picture hook keeps the decoder's last picture, loadImage shows it (the frame's backdrop) and
// completes at once (no DMA, no interrupt), and vblankHandler runs natively (TMB_Recomp's, with Onimusha's addresses):
// it also ends the game frame while a movie shows, since the main thread sleeps through the movie.
#include "oni_render.h"
#include "oni_vu1.h"

#include "bozo/ee/dispatcher.h"
#include "bozo/render/movie.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace oni::movie
{
    namespace
    {
        constexpr uint32_t kGp = 0x21b170;
        constexpr uint32_t kDisplayOn = kGp - 0x6b14, kFrameCount = kGp - 0x6b10, kImageHeld = kGp - 0x6b0c,
                           kField = kGp - 0x6b08, kPathBusy = kGp - 0x6b04, kNoPicture = kGp - 0x6ac4;
        constexpr uint32_t kFadeLevel = kGp - 0x6b00, kFadeTimer = kGp - 0x6afc, kFadeType = kGp - 0x6ae8; // int, short, short
        constexpr uint32_t kVoBuf = 0x460ec0;     // +4 tags, +8 next index, +0xc count, +0x10 size
        constexpr uint32_t kTagStride = 0x34c40;  // VoTag: its state, then the odd (+0x40) and even (+0x1a640) chains

        uint32_t read32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x1ffffffu), 4);
            return v;
        }
        void write32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & 0x1ffffffu), &v, 4); }

        bool trace()
        {
            static const bool on = [] { const char *e = std::getenv("ONI_MOVIE_TRACE"); return e && *e == '1'; }();
            return on;
        }

        std::mutex g_mutex;
        std::shared_ptr<const bozo::render::Texture> g_picture; // the decoder's last picture
        bool g_showing = false;                                   // a movie's picture is shown (playing or fading out)
        float g_brightness = 1.0f;                                // the fade after the display is off
        uint64_t g_shown = 0;

        void onPicture(uint32_t, uint32_t width, uint32_t height, const uint8_t *rgba)
        {
            static uint64_t version = 0;
            auto t = std::make_shared<bozo::render::Texture>();
            t->width = width;
            t->height = height;
            t->rgba.resize(static_cast<size_t>(width) * height);
            std::memcpy(t->rgba.data(), rgba, t->rgba.size() * 4);
            for (uint32_t &p : t->rgba)
                p |= 0xff000000u; // the movie is opaque
            t->key = 0x6f6e694d6f766965ull; // "oniMovie": one picture at a time
            t->version = ++version;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_picture = std::move(t);
        }

        void releasePicture(uint8_t *ram) // voBufDecCount(&voBuf)
        {
            const int32_t count = static_cast<int32_t>(read32(ram, kVoBuf + 0xc));
            if (count > 0)
                write32(ram, kVoBuf + 0xc, static_cast<uint32_t>(count - 1));
        }

        // loadImage(chain): nothing transferred; what handler_endimage does when the transfer ends: a picture still held
        // is freed.
        void loadImage(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            if (read32(ram, kImageHeld))
            {
                write32(ram, kImageHeld, 0);
                releasePicture(ram);
            }
            ctx->pc = ctx->r[31].uw[0];
        }

        // vblankHandler(): each decoded picture (the oldest in voBuf) shown for two fields: the even field when its tag
        // says 2 (then 1), the odd one when it says 1 (then 0, its buffer freed: its transfer has ended). While the
        // display is on, the game frame ends here (the main thread sleeps through the movie). No guest code runs.
        void vblankHandler(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t field = static_cast<uint32_t>(bozo::port::vblankCount(rt) & 1u); // CSR.FIELD
            write32(ram, kField, field);
            const bool on = read32(ram, kDisplayOn) != 0;
            if (on)
            {
                write32(ram, kFrameCount, read32(ram, kFrameCount) + 1);
                write32(ram, kPathBusy, 0); // sceGsSyncPath: nothing in flight
                const int32_t count = static_cast<int32_t>(read32(ram, kVoBuf + 0xc));
                const int32_t size = static_cast<int32_t>(read32(ram, kVoBuf + 0x10));
                if (count > 0 && size > 0)
                {
                    const int32_t next = static_cast<int32_t>(read32(ram, kVoBuf + 0x8));
                    const uint32_t oldest = static_cast<uint32_t>(((next - count) % size + size) % size);
                    const uint32_t tag = read32(ram, kVoBuf + 0x4) + oldest * kTagStride;
                    const uint32_t state = read32(ram, tag);
                    if ((field == 0 && state == 2u) || (field == 1 && state == 1u))
                    {
                        if (read32(ram, kImageHeld)) // the last loadImage's completion
                        {
                            write32(ram, kImageHeld, 0);
                            releasePicture(ram);
                        }
                        write32(ram, tag, field == 0 ? 1u : 0u);
                        if (field == 1)
                            releasePicture(ram);
                        ++g_shown;
                    }
                }
                else
                    write32(ram, kNoPicture, read32(ram, kNoPicture) + 1);
            }
            else
            {
                // The display off (endDisplay: type 1, timer 0x80, level 0x200; a skip or the end): the level goes down
                // by 0x200 / timer each field (type > 0; up for type < 0) until the timer runs out, which readMpeg waits
                // for. The game darkens the last picture with a flat rectangle each field: natively the picture is shown
                // at the level's brightness.
                int16_t type, timer;
                std::memcpy(&type, ram + kFadeType, 2);
                std::memcpy(&timer, ram + kFadeTimer, 2);
                int32_t level = static_cast<int32_t>(read32(ram, kFadeLevel));
                if (type != 0 && timer != 0)
                {
                    const int32_t step = 0x200 / timer;
                    level = type > 0 ? std::max(0, level - step) : std::min(0x200, level + step);
                    write32(ram, kFadeLevel, static_cast<uint32_t>(level));
                    --timer;
                    std::memcpy(ram + kFadeTimer, &timer, 2);
                    if (timer == 0)
                    {
                        const int16_t zero = 0;
                        std::memcpy(ram + kFadeType, &zero, 2);
                    }
                }
                g_brightness = static_cast<float>(std::clamp(level, 0, 0x200)) / 512.0f;
            }
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_showing = on || g_picture != nullptr;
            }
            if (on)
                g_brightness = 1.0f;
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
            // The main thread sleeps through the whole movie task: its frames end here, the display on or off.
            if (trace() && (!on || g_shown % 60 == 1))
                std::printf("[oni:movie] vblank field %u, display %d, pictures shown %llu, buffered %u, brightness %.2f\n",
                            field, on ? 1 : 0, (unsigned long long)g_shown, read32(ram, kVoBuf + 0xc), g_brightness);
            oni::render::publish(ram); // (the picture and its darkening: drawOver, from publish)
            bozo::port::frameEnd(rt, ctx); // last: it may defer a stop
        }

        // dispCreate(d, fb_w, fb_h): the display's environment and its GS registers (a DMA of its own): only the frame
        // size is kept in the structure.
        void dispCreate(uint8_t *ram, R5900Context *ctx, Runtime *)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_picture.reset(); // a new movie: nothing shown until its first picture
                g_showing = false;
            }
            g_brightness = 1.0f;
            const uint32_t d = ctx->r[4].uw[0];
            write32(ram, d + 0x30, ctx->r[5].uw[0]);
            write32(ram, d + 0x34, ctx->r[6].uw[0]);
            ctx->pc = ctx->r[31].uw[0];
        }

        // dispDelete(): the movie task is ending (its handlers removed): the frame goes back to the game's own.
        void dispDelete(uint8_t *, R5900Context *ctx, Runtime *)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_picture.reset();
                g_showing = false;
            }
            g_brightness = 1.0f;
            ctx->pc = ctx->r[31].uw[0];
        }

        // dispClear(d, colour): a flat rectangle over the frame by DMA: the backdrop is cleared instead.
        void dispClear(uint8_t *, R5900Context *ctx, Runtime *)
        {
            ctx->pc = ctx->r[31].uw[0];
        }

        // videoCallback(mp, cbstr, data): the game copies the demuxed video into its IPU FIFO (viBuf) and starts its DMA;
        // natively the decoder takes the video from the stream itself (BozoEngine's answer to TMB's videoCallback): 1.
        void videoCallback(uint8_t *, R5900Context *ctx, Runtime *)
        {
            ctx->r[2].ud[0] = 1;
            ctx->pc = ctx->r[31].uw[0];
        }
        // switchThread() 0x1bd4c0: `j RotateThreadReadyQueue(1)`. The player's threads wait for each other with it in
        // call-free loops (decBs0 for a free picture buffer, which only a vblank frees; readMpeg between reads): each
        // pass costs the EE almost nothing, so a field took tens of thousands of them and the window fell below 60
        // frames a second during movies. Each pass now costs 0.1 ms of EE time (the waits reach the vblank in about 160
        // passes, the reader still gets as many), then the kernel call as before (a tail call into the guest stub).
        void switchThread(uint8_t *, R5900Context *ctx, Runtime *rt)
        {
            bozo::port::accountCycles(rt, ctx, 294912000ull / 10000ull);
            ctx->r[4].ud[0] = 1;
            ctx->pc = 0x10b450; // RotateThreadReadyQueue, returning to switchThread's caller
        }
    } // namespace

    // The movie's picture while its display is on (nullptr otherwise).
    std::shared_ptr<const bozo::render::Texture> shown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_showing ? g_picture : nullptr;
    }

    void drawOver()
    {
        const auto picture = shown();
        const bozo::render::Screen s = oni::render::screen();
        const float xs[6] = {0, 1, 0, 1, 1, 0}, ys[6] = {0, 0, 1, 0, 1, 1};
        if (trace())
        {
            static unsigned n = 0;
            if (n++ % 120 == 0)
                std::printf("[oni:movie] drawOver: picture %s %ux%u, mean %u, brightness %.2f\n", picture ? "yes" : "no",
                            picture ? picture->width : 0, picture ? picture->height : 0,
                            picture && !picture->rgba.empty() ? [&] {
                                    uint64_t sum = 0;
                                    for (uint32_t p : picture->rgba)
                                        sum += (p & 0xff) + ((p >> 8) & 0xff) + ((p >> 16) & 0xff);
                                    return static_cast<uint32_t>(sum / (3 * picture->rgba.size()));
                                }() : 0u, g_brightness);
        }
        if (picture)
        {
            // The picture as the frame's last draw, over whatever the game drew (a movie started from a room's script
            // plays over the room, as the player's pictures go into the display buffer over it): opaque, centred.
            bozo::render::Material m;
            m.textured = true;
            m.depthTest = m.depthWrite = false;
            m.blend = bozo::render::Material::Blend::Opaque;
            m.clampU = m.clampV = true;
            m.tex0 = 0x6f6e694d6f766965ull; // "oniMovie"
            const float w = static_cast<float>(picture->width), h = static_cast<float>(picture->height);
            const float x0 = s.originX + (s.width - w) * 0.5f, y0 = s.originY + (s.height - h) * 0.5f;
            bozo::render::Vertex v[6];
            for (int i = 0; i < 6; ++i)
            {
                v[i].x = x0 + xs[i] * w;
                v[i].y = y0 + ys[i] * h;
                v[i].u = xs[i];
                v[i].v = ys[i];
                v[i].rgba = 0x80808080u;
            }
            // Black around a picture smaller than the frame.
            bozo::render::Material black;
            black.textured = false;
            black.depthTest = black.depthWrite = false;
            black.blend = bozo::render::Material::Blend::Opaque;
            bozo::render::Vertex b[6];
            for (int i = 0; i < 6; ++i)
            {
                b[i].x = s.originX + xs[i] * s.width;
                b[i].y = s.originY + ys[i] * s.height;
                b[i].rgba = 0x80000000u;
            }
            oni::vu1::queueDraw(black, b, 6, nullptr);
            oni::vu1::queueDraw(m, v, 6, picture);
        }
        if (g_brightness < 1.0f)
        {
            // The darkening over the picture: black, alpha (1 - brightness), over the whole frame.
            bozo::render::Material m;
            m.textured = false;
            m.depthTest = m.depthWrite = false;
            m.blend = bozo::render::Material::Blend::Alpha;
            const uint32_t alpha = static_cast<uint32_t>((1.0f - g_brightness) * 128.0f + 0.5f);
            bozo::render::Vertex v[6];
            for (int i = 0; i < 6; ++i)
            {
                v[i].x = s.originX + xs[i] * s.width;
                v[i].y = s.originY + ys[i] * s.height;
                v[i].rgba = alpha << 24;
            }
            oni::vu1::queueDraw(m, v, 6, nullptr);
        }
    }

    std::string unsaveable()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_showing ? "a movie is playing" : "";
    }

    void install(bozo::port::Registry &r)
    {
        bozo::render::setMoviePictureHook(onPicture);
        r.registerFunction(0x1bca10, loadImage);
        r.registerFunction(0x1bca40, vblankHandler);
        r.registerFunction(0x1bb900, dispCreate);
        r.registerFunction(0x1bc170, dispClear);
        r.registerFunction(0x1bbbc0, dispDelete);
        r.registerFunction(0x1bfc20, videoCallback);
        r.registerFunction(0x1bd4c0, switchThread);
    }
} // namespace oni::movie
