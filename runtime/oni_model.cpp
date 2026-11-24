// Onimusha's 3D models. The game turns each model into VIF1 packets for its VU1 programs (Md_draw_entry_vu0 /
// Md_draw_entry_clip build them from the model's own data, sys_sub.c) and queues them: Md_draw_entry(tag) 0x130cc0
// appends the tag at G + 0x5bdc (G = 0x27de30) and starts the queue if it is idle; Md_tag_trans(tag) 0x130e40 starts
// VIF1's DMA on it (the busy flag 0x287222 set), and Md_tag_trans_cb 0x130da0 (the DMA end interrupt) moves the queue's
// read pointer (G + 0x5be0) on, counts it down (0x287221) and starts the next tag, or marks the queue idle (0x287220,
// 0x287221 zero) at its end.
//
// Natively Md_tag_trans runs each queued chain at once on VIF1 and VU1 as runtime/oni_vu1.cpp has them (the translated
// microprograms, their GIF packets drawn) and completes the whole queue (as if every transfer had ended).
#include "oni_render.h"

#include "bozo/ee/dispatcher.h"
#include "oni_vu1.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oni::model
{
    namespace
    {
        constexpr uint32_t kG = 0x27de30;
        constexpr uint32_t kQueueRead = kG + 0x5be0;
        constexpr uint32_t kRunning = 0x287220, kPending = 0x287221, kBusy = 0x287222;

        uint32_t read32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x1ffffffu), 4);
            return v;
        }
        void write32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & 0x1ffffffu), &v, 4); }

        // Md_tag_trans(tag): this tag and every one queued after it "transferred" at once: what Md_tag_trans_cb does
        // for each.
        void tagTrans(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            uint8_t *spr = bozo::port::scratchpad(rt);
            oni::vu1::runChain(ram, spr, ctx->r[4].uw[0]);
            ram[kBusy] = 0;
            for (int guard = 0; guard < 4096; ++guard)
            {
                const uint32_t read = read32(ram, kQueueRead) + 4;
                write32(ram, kQueueRead, read);
                ram[kPending] = static_cast<uint8_t>(ram[kPending] - 1);
                const uint32_t next = read32(ram, read);
                if (next == 0)
                {
                    ram[kRunning] = 0;
                    ram[kPending] = 0;
                    break;
                }
                oni::vu1::runChain(ram, spr, next);
            }
            ctx->pc = ctx->r[31].uw[0];
        }
    } // namespace

    // sceDmaSend(channel, tag): the chains the game sends itself (sprites, effects, the screen blur, the background's
    // own); BozoEngine's libdma completes them without reading them (native graphics). ONI_DMA_TRACE=1 lists them.
    void dmaSend(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        static const bool trace = std::getenv("ONI_DMA_TRACE") != nullptr;
        if (trace)
        {
            const uint32_t handle = ctx->r[4].uw[0], tag = ctx->r[5].uw[0];
            std::printf("[oni:dma] sceDmaSend channel %x tag %x from %x\n", handle, tag, ctx->r[31].uw[0]);
        }
        // The GIF's chains drawn natively, except the background's (Bg_disp draws the picture itself).
        static const bool gif = [] { const char *e = std::getenv("ONI_GIF_CHAINS"); return !(e && *e == '0'); }();
        const uint32_t from = ctx->r[31].uw[0];
        if (gif && ctx->r[4].uw[0] == 0x1000a000u && !(from >= 0x122040u && from < 0x122188u))
            oni::vu1::runGifChain(ram, bozo::port::scratchpad(rt), ctx->r[5].uw[0]);
        bozo::port::callLibrary(rt, ctx, 0x101c50);
    }

    // sceDmaSendN(channel, address, qwc): a normal-mode transfer. On the GIF's channel the game sends packets it built
    // itself this way: StencilMapPolygon's (a room's animated water, the background movie kept in block 0x1a40 drawn
    // through the cut's polygons: stage 3 room 7's pond). BozoEngine's libdma completes it without reading it.
    void dmaSendN(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        static const bool trace = std::getenv("ONI_DMA_TRACE") != nullptr;
        const uint32_t handle = ctx->r[4].uw[0], address = ctx->r[5].uw[0], qwc = ctx->r[6].uw[0];
        if (trace)
            std::printf("[oni:dma] sceDmaSendN channel %x address %x qwc %u from %x\n", handle, address, qwc, ctx->r[31].uw[0]);
        static const bool gif = [] { const char *e = std::getenv("ONI_GIF_CHAINS"); return !(e && *e == '0'); }();
        if (gif && handle == 0x1000a000u && qwc > 0 && qwc < 0x10000u)
            oni::vu1::runGifData(ram, bozo::port::scratchpad(rt), address, qwc);
        bozo::port::callLibrary(rt, ctx, 0x101d28);
    }

    // ONI_CD_TRACE=1: every sceCdRead (sector, count, buffer, caller).
    void cdRead(uint8_t *ram, R5900Context *ctx, Runtime *rt)
    {
        std::fprintf(stderr, "[oni:cd] sceCdRead lsn %u sectors %u buffer %x from %x\n", ctx->r[4].uw[0], ctx->r[5].uw[0],
                    ctx->r[6].uw[0], ctx->r[31].uw[0]);
        bozo::port::callLibrary(rt, ctx, 0x1121c0);
    }

    void install(bozo::port::Registry &r)
    {
        if (std::getenv("ONI_CD_TRACE"))
            r.registerFunction(0x1121c0, cdRead); // sceCdRead
        r.registerFunction(0x101c50, dmaSend); // sceDmaSend
        r.registerFunction(0x101d28, dmaSendN); // sceDmaSendN
        r.registerFunction(0x130e40, tagTrans); // Md_tag_trans
    }
} // namespace oni::model
