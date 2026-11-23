// Onimusha's textures as the game uploads them (runtime/oni_tex.cpp).
#pragma once
#include "bozo/port.h"
#include "bozo/render/texture.h"

#include <cstdint>
#include <memory>

namespace oni::tex
{
    // The texture a GS TEX0 value names, decoded from the uploads (nullptr: its texels were never uploaded).
    std::shared_ptr<const bozo::render::Texture> fromTex0(uint64_t tex0);
    void install(bozo::port::Registry &registry);
} // namespace oni::tex
