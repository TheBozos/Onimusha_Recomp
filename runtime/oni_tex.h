// Onimusha's textures as the game uploads them (runtime/oni_tex.cpp).
#pragma once
#include "bozo/port.h"
#include "bozo/render/texture.h"

#include <cstdint>
#include <cstddef>
#include <memory>

namespace oni::tex
{
    // The texture a GS TEX0 value names, decoded from the uploads (nullptr: its texels were never uploaded).
    std::shared_ptr<const bozo::render::Texture> fromTex0(uint64_t tex0);
    // A GIF IMAGE transfer to local memory (the registers BITBLTBUF, TRXPOS, TRXREG set before it), kept as an upload.
    void upload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, const uint8_t *texels, size_t bytes);
    void install(bozo::port::Registry &registry);
} // namespace oni::tex
