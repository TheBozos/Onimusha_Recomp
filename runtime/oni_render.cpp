// Onimusha's native frame: the background, then the 2D, published to BozoEngine's renderer at each frame boundary.
#include "oni_render.h"

#include <cstdio>
#include <cstdlib>

namespace oni::render
{
    namespace
    {
        bool trace()
        {
            static const bool on = [] { const char *e = std::getenv("ONI_RENDER_TRACE"); return e && *e == '1'; }();
            return on;
        }
        bool g_open = false;
        uint64_t g_frames = 0;
    } // namespace

    bozo::render::Screen screen()
    {
        bozo::render::Screen s;
        s.width = 640.0f;
        s.height = 448.0f;
        s.originX = 2048.0f - 320.0f;
        s.originY = 2048.0f - 224.0f;
        return s;
    }

    void publish(uint8_t *ram)
    {
        auto &r = bozo::render::Renderer::instance();
        const bozo::render::Screen s = screen();
        if (!g_open)
            r.beginFrame(s);
        const auto background = oni::bg::takeShown(ram);
        if (const auto picture = oni::movie::shown()) // a movie: its picture centred in the frame
        {
            const float w = static_cast<float>(picture->width), h = static_cast<float>(picture->height);
            r.setBackdrop(picture, s, s.originX + (s.width - w) * 0.5f, s.originY + (s.height - h) * 0.5f, w, h);
        }
        else if (background)
            r.setBackdrop(background, s, s.originX, s.originY, s.width, s.height);
        else
            r.clearBackdrop();
        oni::sprite::submit(ram);
        r.publishOverlay(s);
        r.endFrame();
        ++g_frames;
        if (trace())
            std::printf("[oni:render] frame %llu background %d\n", (unsigned long long)g_frames, background ? 1 : 0);
        r.beginFrame(s);
        g_open = true;
    }

    void install(bozo::port::Registry &registry)
    {
        auto &r = bozo::render::Renderer::instance();
        r.setEnabled(true);
        r.setPresentsAlone(true); // the window shows the port's frame: the background, the 2D
        r.setClearColour(0);
        oni::bg::install(registry);
        oni::sprite::install(registry);
        oni::movie::install(registry);
    }
} // namespace oni::render
