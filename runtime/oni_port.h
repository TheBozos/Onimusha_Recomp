// Onimusha: Warlords' native port boundary.
#pragma once
#include "bozo/libs/libraries.h"
#include "bozo/port.h"
#include <cstddef>
#include <cstdint>

namespace oni
{
    // The port's overrides (registerFunction at resident addresses only: the overlay slots are shared).
    void install(Runtime &rt, bozo::port::Registry &registry);
    // Every overlay module (enemy slots 0 and 1, rooms, em090), selected by its resident identity
    // (generated: tools/oni_modules_gen.py). Returns how many.
    size_t registerModules(Runtime &rt);
    // The IOP sound modules (runtime/oni_audio.cpp): module loads, SIF transfers and the mixer.
    void installAudio(bozo::libs::Libraries &libraries, uint8_t *eeRam);
    // The VU0 microprogram by entry (generated: tools/oni_vu0_gen.py).
    void installVu0(bozo::port::Registry &registry);
} // namespace oni
