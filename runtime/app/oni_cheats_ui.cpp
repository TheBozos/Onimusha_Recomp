// The window's Cheats tab (F2, beside Controls and Video): ImGui widgets for oni_cheats.h. Only in the window build.
#include "oni_cheats.h"

#include "bozo/app/settings_tabs.h"
#include "imgui.h"

namespace oni::cheats
{
    void registerMenu()
    {
        bozo::app::ui::registerTab("Cheats", [] {
            auto toggle = [](const char *label, Cheat cheat, const char *description) {
                bool on = enabled(cheat);
                if (ImGui::Checkbox(label, &on))
                    setEnabled(cheat, on);
                ImGui::TextDisabled("%s", description);
                ImGui::Spacing();
            };
            toggle("Infinite health", Cheat::InfiniteHealth, "Keeps Samanosuke's life at its maximum.");
        });
    }
} // namespace oni::cheats
