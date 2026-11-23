// Onimusha's native frame: what the game shows each frame, published to BozoEngine's renderer at the frame boundary.
#pragma once
#include "bozo/port.h"
#include "bozo/render/renderer.h"

#include <cstdint>
#include <memory>

namespace oni::render
{
    // The game's screen: 640 x 448, drawn around the GS's XYOFFSET origin (2048 - 320, 2048 - 224).
    bozo::render::Screen screen();
    // The frame boundary (sceGsSwapDBuffDc): publishes the frame the game built since the last one.
    void publish(uint8_t *ram);
    void install(bozo::port::Registry &registry);
} // namespace oni::render

namespace oni::bg
{
    // The background Bg_disp showed this frame (nullptr: none), from the field buffers.
    std::shared_ptr<const bozo::render::Texture> takeShown(const uint8_t *ram);
    void install(bozo::port::Registry &registry);
} // namespace oni::bg

namespace oni::sprite
{
    // The 2D the game drew this frame, submitted to the overlay before it is published.
    void submit(uint8_t *ram);
    void install(bozo::port::Registry &registry);
} // namespace oni::sprite
