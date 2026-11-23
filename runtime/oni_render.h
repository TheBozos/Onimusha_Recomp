// Onimusha's native frame: what the game shows each frame, published to BozoEngine's renderer at the frame boundary.
#pragma once
#include "bozo/port.h"
#include "bozo/render/renderer.h"

#include <cstdint>
#include <memory>
#include <string>

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
    // The background picture, from the field buffers (Bg_disp draws it into the frame).
    std::shared_ptr<const bozo::render::Texture> picture(const uint8_t *ram);
    // After a save state's load: the pictures rebuilt from the loaded memory.
    void stateLoaded();
    // The room's depth Bgz_disp gave at the end of the last frame, as the new frame's first draw (none: nothing).
    void drawDepth();
    void install(bozo::port::Registry &registry);
} // namespace oni::bg

namespace oni::movie
{
    // The movie's picture while its display is on (nullptr otherwise).
    std::shared_ptr<const bozo::render::Texture> shown();
    // Why the state cannot be saved now (a movie is shown), or empty.
    std::string unsaveable();
    void install(bozo::port::Registry &registry);
} // namespace oni::movie

namespace oni::model
{
    void install(bozo::port::Registry &registry);
} // namespace oni::model

namespace oni::sprite
{
    // The 2D the game drew this frame, submitted to the overlay before it is published.
    void submit(uint8_t *ram);
    void install(bozo::port::Registry &registry);
} // namespace oni::sprite
