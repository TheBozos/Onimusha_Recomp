// Onimusha's VU1: the models' microprograms translated to C++ (tools/oni_vu1_gen.py) and what runs them natively
// (runtime/oni_vu1.cpp): VIF1's chain from Md_tag_trans (its unpacks into VU1 memory, MPG uploads, MSCAL / MSCNT), and
// the GIF packets the programs kick, turned into the frame's draws.
#pragma once
#include "bozo/ops_vu0.h"

#include <emmintrin.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace oni::vu1
{
    using bozo::vu0::Vec;

    // What a translated program sees besides its registers: VU1's data memory (16 KB), VIF1's TOP / ITOP, and where
    // its XGKICKs go.
    struct Vu1Io
    {
        uint8_t *mem = nullptr;
        uint32_t top = 0, itop = 0;
        void (*kick)(Vu1Io &, uint32_t qword) = nullptr;
        void *user = nullptr;
        uint64_t cycles = 0;
        uint32_t fault = ~0u; // a pc the translation does not have (an MSCNT into the middle of nowhere)
    };

    inline uint8_t *quad(Vu1Io &io, uint32_t index) { return io.mem + ((index & 0x3FFu) << 4); }
    inline void vu1Lq(Vu1Io &io, Vec &d, uint32_t index, uint32_t dest)
    {
        const uint8_t *s = quad(io, index);
        for (int k = 0; k < 4; ++k)
            if (bozo::vu0::fieldOn(dest, k))
                std::memcpy(&d.u[k], s + 4 * k, 4);
    }
    inline void vu1Sq(Vu1Io &io, const Vec &v, uint32_t index, uint32_t dest)
    {
        uint8_t *d = quad(io, index);
        for (int k = 0; k < 4; ++k)
            if (bozo::vu0::fieldOn(dest, k))
                std::memcpy(d + 4 * k, &v.u[k], 4);
    }
    inline uint32_t vu1Ilw(Vu1Io &io, uint32_t index, uint32_t dest)
    {
        const uint8_t *s = quad(io, index);
        for (int k = 0; k < 4; ++k)
            if (bozo::vu0::fieldOn(dest, k))
            {
                uint32_t w;
                std::memcpy(&w, s + 4 * k, 4);
                return w & 0xFFFFu;
            }
        return 0;
    }
    inline void vu1Isw(Vu1Io &io, uint32_t value, uint32_t index, uint32_t dest)
    {
        uint8_t *d = quad(io, index);
        const uint32_t w = value & 0xFFFFu;
        for (int k = 0; k < 4; ++k)
            if (bozo::vu0::fieldOn(dest, k))
                std::memcpy(d + 4 * k, &w, 4);
    }

    // The flag-free arithmetic for programs that never read the MAC flags (oni_vu1_gen.py): four fields at once in the
    // host's floats, the result held to the PS2's range (no infinity, no NaN: the largest magnitude instead). The
    // exponent-255 operands and the denormals the exact path treats as the PS2 does are not expected in vertex work.
    inline __m128 fastLoad(const Vec &v) { return _mm_loadu_ps(reinterpret_cast<const float *>(v.u)); }
    inline __m128 fastRange(__m128 r)
    {
        const __m128 most = _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF));
        r = _mm_min_ps(r, most); // a NaN becomes the largest value (min returns its second operand)
        return _mm_max_ps(r, _mm_xor_ps(most, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u)))));
    }
    inline void fastStore(Vec &d, __m128 r, uint32_t dest)
    {
        if (dest == 15)
        {
            _mm_storeu_ps(reinterpret_cast<float *>(d.u), r);
            return;
        }
        const __m128 on = _mm_castsi128_ps(_mm_set_epi32(dest & 1 ? -1 : 0, dest & 2 ? -1 : 0, dest & 4 ? -1 : 0, dest & 8 ? -1 : 0));
        _mm_storeu_ps(reinterpret_cast<float *>(d.u), _mm_or_ps(_mm_and_ps(on, r), _mm_andnot_ps(on, fastLoad(d))));
    }
    inline void fast_add(Vec &d, const Vec &s, const Vec &t, uint32_t dest) { fastStore(d, fastRange(_mm_add_ps(fastLoad(s), fastLoad(t))), dest); }
    inline void fast_sub(Vec &d, const Vec &s, const Vec &t, uint32_t dest) { fastStore(d, fastRange(_mm_sub_ps(fastLoad(s), fastLoad(t))), dest); }
    inline void fast_mul(Vec &d, const Vec &s, const Vec &t, uint32_t dest) { fastStore(d, fastRange(_mm_mul_ps(fastLoad(s), fastLoad(t))), dest); }
    inline void fast_madd(Vec &d, const Vec &a, const Vec &s, const Vec &t, uint32_t dest)
    {
        fastStore(d, fastRange(_mm_add_ps(fastLoad(a), fastRange(_mm_mul_ps(fastLoad(s), fastLoad(t))))), dest);
    }
    inline void fast_msub(Vec &d, const Vec &a, const Vec &s, const Vec &t, uint32_t dest)
    {
        fastStore(d, fastRange(_mm_sub_ps(fastLoad(a), fastRange(_mm_mul_ps(fastLoad(s), fastLoad(t))))), dest);
    }

    // One translated image: its ELF address, its pair count and words' hash (how an MPG is recognised), its function
    // (runs from pc to the pair after the next E bit; returns where an MSCNT goes on).
    struct Program
    {
        uint32_t address, pairs;
        uint64_t hash;
        const char *name;
        uint32_t (*run)(R5900Context *, Vu1Io &, uint32_t pc);
    };
    extern const Program kPrograms[];
    extern const size_t kProgramCount;

    // Md_tag_trans's chain (a VIF1 source chain at tag, TTE on) run natively: VIF1's commands, the programs, the draws.
    void runChain(uint8_t *ram, uint8_t *scratchpad, uint32_t tag);
    // The frame's counts (ONI_VU1_TRACE=1), reset for the next frame.
    void frameStats();
} // namespace oni::vu1
