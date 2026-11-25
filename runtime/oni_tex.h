// Onimusha's textures as the game uploads them (runtime/oni_tex.cpp).
#pragma once
#include "bozo/port.h"
#include "bozo/render/texture.h"
#include "bozo/state.h"

#include <cstdint>
#include <cstddef>
#include <memory>

namespace oni::tex
{
    // The texture a GS TEX0 value names, decoded from the uploads (nullptr: its texels were never uploaded).
    std::shared_ptr<const bozo::render::Texture> fromTex0(uint64_t tex0);
    // A GIF IMAGE transfer to local memory (the registers BITBLTBUF, TRXPOS, TRXREG set before it), kept as an upload.
    void upload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, const uint8_t *texels, size_t bytes);
    // A GIF local-to-local transfer (TRXDIR 2) between two kept images of the same format: Texmove_entry's texture
    // scrolls (a texture's rows wrapped round through block 0x1a40). False when it could not be made.
    bool copyLocal(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg);
    // The uploads in save states; the decoded textures are dropped on a load.
    void state(bozo::state::Archive &a);
    void install(bozo::port::Registry &registry);
} // namespace oni::tex
