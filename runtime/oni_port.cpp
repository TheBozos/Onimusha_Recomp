// Onimusha: Warlords' native port boundary. No hardware execution or interpreter fallback.
//
// Native instruction sites: each `case 0x...u: // native site` below is listed for bozo-recomp by
// tools/oni_overrides_list.py (a "native 0xADDR" external): the instruction is replaced by this handler, inline.
#include "oni_port.h"
#include "bozo/ee/dispatcher.h"
#include "oni_render.h"

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
        bool nativeInstruction(uint8_t *, R5900Context *ctx, Runtime *rt, uint32_t pc)
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
        r.nativeGraphics(&nativeInstruction);
    }
} // namespace oni
