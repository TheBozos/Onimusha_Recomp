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

    void install(bozo::port::Registry &r)
    {
        r.registerFunction(0x130e40, tagTrans); // Md_tag_trans
    }
} // namespace oni::model
