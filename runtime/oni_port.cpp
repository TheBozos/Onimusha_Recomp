// Onimusha: Warlords' native port boundary. No hardware execution or interpreter fallback.
//
// Native instruction sites: each `case 0x...u: // native site` below is listed for bozo-recomp by
// tools/oni_overrides_list.py (a "native 0xADDR" external): the instruction is replaced by this handler, inline.
#include "oni_port.h"
#include "bozo/ee/dispatcher.h"
#include "oni_render.h"
#include "oni_vu1.h"

#include <cstring>

namespace oni
{
    namespace
    {
        void returnZero(uint8_t *, R5900Context *ctx, Runtime *)
        {
            ctx->r[2].ud[0] = 0;
            ctx->pc = ctx->r[31].uw[0];
        }

        // sceGsSwapDBuffDc(dbuff, field): main calls it at the top of every frame, right after its vsync wait. There is
        // no GS to point at the other buffer: it is the frame boundary (the native renderer's frame is published here
        // once the game's drawing is answered natively).
        void swapBuffers(uint8_t *ram, R5900Context *ctx, Runtime *rt)
        {
            ctx->r[2].ud[0] = 0;
            oni::render::publish(ram);
            bozo::port::frameEnd(rt, ctx);
            ctx->pc = ctx->r[31].uw[0];
        }

        // The display circuits' DISPLAY1/DISPLAY2 words Quake_move last wrote (screen shake: its DX/DY move by the
        // offset at 0x287350/0x287352). Kept for the native renderer to offset the presented frame.
        uint64_t display[2] = {};

        // main's loop reads and writes the GS CSR register (0x12001000) itself; Quake_move writes DISPLAY1/2.
        uint32_t g_semiTag = 0; // Md_draw_semi's chain (its D2_TADR)

        bool nativeInstruction(uint8_t *ram, R5900Context *ctx, Runtime *rt, uint32_t pc)
        {
            switch (pc)
            {
            case 0x0012ddb8u: // native site: main+0x48, ld v0,0x1000(at): CSR before clearing VSINT
                ctx->r[2].ud[0] = 0;
                return true;
            case 0x0012ddc4u: // native site: main+0x54, sd v0,0x1000(at): clear VSINT; nothing to clear
                return true;
            case 0x0012e0bcu: // native site: main+0x34c, ld v0,0x1000(at): CSR.FIELD (bit 13), the field shown
                ctx->r[2].ud[0] = (bozo::port::vblankCount(rt) & 1u) << 13;
                return true;
            case 0x0012e0dcu: // native site: main+0x36c, ld v0,0x1000(at): the vsync wait polls CSR.VSINT (bit 3)
                // The frame waits at sceGsSwapDBuffDc's frame end instead: the vblank has come.
                ctx->r[2].ud[0] = 8;
                return true;
            case 0x001235c0u: // native site: Cut_set+0x130, sw v0,0x2010(at): IPU_CTRL reset before decodeIPU
            case 0x0012366cu: // native site: Cut_set_onmemory+0x7c, the same
                return true;
            case 0x001bd7a0u: // native site: termAll+0x30, lw v1,-0x6ab8(gp): the decoder thread's id
            {
                // termAll repeats TerminateThread / DeleteThread until DeleteThread returns the thread's id, as the PS2
                // kernel does; BozoEngine's returns 0 on success (its reference's value): 0 counts as the id.
                uint32_t id;
                std::memcpy(&id, ram + 0x21b170 - 0x6ab8, 4);
                ctx->r[3].ud[0] = ctx->r[2].sw[0] == 0 ? 0 : static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(id)));
                return true;
            }
            // pCalcLoadGmMatrix: VIF0's DMA sends the POLYCALC microprogram (the chain at 0x1ee990: an MPG to VU0
            // address 0). The program is native (tools/oni_vu0_gen.py, VCALLMS 0): nothing to send, the transfer done.
            case 0x001af520u: // native site: sw zero,(a2): D0_QWC
            case 0x001af528u: // native site: sw a0,(v1): D0_TADR
            case 0x001af534u: // native site: sw v1,(a0): D0_CHCR, start
                return true;
            // Md_draw_semi / Md_draw_semi_l: the semi-transparent models' parts (Samanosuke's hair, ...) as GS packets
            // the game builds and sends by GIF DMA itself (channel 2: QWC, MADR, ASR0, ASR1, TADR, then CHCR 0x105:
            // chain mode). The chain is read at the start and drawn through the GIF interpreter, as the chains
            // sceDmaSend sends (runtime/oni_vu1.cpp).
            case 0x0018df18u: // native site: Md_draw_semi, D2_TADR
            case 0x0018e4d8u: // native site: Md_draw_semi_l, D2_TADR
                g_semiTag = ctx->r[2].uw[0];
                return true;
            case 0x0018df34u: // native site: Md_draw_semi, D2_CHCR start
            case 0x0018e4f4u: // native site: Md_draw_semi_l, D2_CHCR start
                oni::vu1::runGifChain(ram, bozo::port::scratchpad(rt), g_semiTag);
                return true;
            case 0x0018deecu: // native site: Md_draw_semi, D2_QWC
            case 0x0018def4u: // native site: Md_draw_semi, D2_MADR
            case 0x0018defcu: // native site: Md_draw_semi, D2_ASR0
            case 0x0018df04u: // native site: Md_draw_semi, D2_ASR1
            case 0x0018df24u: // native site: Md_draw_semi, D2_CHCR
            case 0x0018e4acu: // native site: Md_draw_semi_l, D2_QWC
            case 0x0018e4b4u: // native site: Md_draw_semi_l, D2_MADR
            case 0x0018e4bcu: // native site: Md_draw_semi_l, D2_ASR0
            case 0x0018e4c4u: // native site: Md_draw_semi_l, D2_ASR1
            case 0x0018e4e4u: // native site: Md_draw_semi_l, D2_CHCR
                return true;
            case 0x001af538u: // native site: lw v1,(a0): D0_CHCR polled until STR clears
                ctx->r[3].ud[0] = 0;
                return true;
            case 0x0012fba8u: // native site: Quake_move+0xa8, sd v1,0x80(at): DISPLAY1
                display[0] = ctx->r[3].ud[0];
                return true;
            case 0x0012fc20u: // native site: Quake_move+0x120, sd v1,0xa0(at): DISPLAY2
                display[1] = ctx->r[3].ud[0];
                return true;
            default:
                return false;
            }
        }
    } // namespace

    void install(Runtime &, bozo::port::Registry &r)
    {
        // sceIpuInit (libipu): resets the IPU and its DMA channels (Bg_disp_init, decodeIPU, Movie_init,
        // DecodeIPU_Movie). There is no IPU: nothing to reset. The decoders that use it (backgrounds, BG movies) are
        // to be answered natively at decodeIPU / DecodeIPU_Movie.
        r.registerFunction(0x10aeb0, returnZero);
        // sceSifSetDChain (syscall 120): Snd_SdTransToIOP / Snd_SdTransToIOPBD arm the SIF DMA chain before their
        // sceSifSetDma (native: the data reaches the sound driver at once). Nothing to arm.
        r.registerFunction(0x10b980, returnZero);
        r.registerFunction(0x101868, swapBuffers); // sceGsSwapDBuffDc
        oni::render::install(r);
        installVu0(r);
        r.nativeGraphics(&nativeInstruction);
    }
} // namespace oni
