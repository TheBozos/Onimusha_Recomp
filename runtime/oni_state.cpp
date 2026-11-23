// Onimusha's save-state section (bozo/state.h): what the engine's own sections do not hold. The engine saves the EE
// runtime, the kernel, the libraries and the host; the port adds the sound modules' IOP host (compiled IRX), the
// texture uploads (GS memory as the game filled it) and VIF1 / VU1 / GS state (the models' programs keep theirs between
// frames). The pictures derived from EE memory (the room's picture and depth) are rebuilt after a load.
#include "oni_port.h"
#include "oni_render.h"
#include "oni_tex.h"
#include "oni_vu1.h"

#include <string>

namespace oni
{
    void audioState(bozo::state::Archive &a);
    std::string audioUnsaveable();

    void installState(bozo::state::Sections &sections)
    {
        sections.add({"oni",
                      [](bozo::state::Archive &a)
                      {
                          audioState(a);
                          oni::tex::state(a);
                          oni::vu1::state(a);
                          if (a.loading())
                              oni::bg::stateLoaded();
                      },
                      []
                      {
                          if (std::string why = audioUnsaveable(); !why.empty())
                              return why;
                          return oni::movie::unsaveable();
                      }});
    }
} // namespace oni
