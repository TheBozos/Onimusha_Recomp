// The window's Cheats tab (F2, beside Controls and Video): ImGui widgets for oni_cheats.h. Only in the window build.
#include "oni_cheats.h"

#include "bozo/app/settings_tabs.h"
#include "imgui.h"

#include <cstdio>
#include <string>

namespace oni::cheats
{
    namespace
    {
        std::string warpName(const Warp &w)
        {
            char text[96];
            std::snprintf(text, sizeof text, "Stage %u, room %u (cut %u; the door from room %u-%u)", w.stage, w.room, w.cut,
                          w.fromStage, w.fromRoom);
            return text;
        }
    } // namespace

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
            toggle("Infinite magic", Cheat::InfiniteMagic, "Keeps the three orbs' magic at its maximum.");
            toggle("Infinite souls", Cheat::InfiniteSouls, "Keeps 99,999 souls to spend on the gauntlet's upgrades.");
            toggle("Infinite items", Cheat::InfiniteItems,
                   "Herbs, medicine, arrows and bullets never run out (a used item comes back).");
            toggle("One-hit kills", Cheat::OneHitKills, "Every enemy dies at the first hit (bosses included; not the castle gate's scripted first fight).");

            ImGui::SeparatorText("Room warp");
            int stage = -1, room = -1, cut = -1;
            place(stage, room, cut);
            if (stage >= 0)
                ImGui::Text("Now: stage %d, room %d, cut %d", stage, room, cut);
            static int selected = 0;
            if (selected >= static_cast<int>(kWarpCount))
                selected = 0;
            if (kWarpCount == 0)
                ImGui::TextDisabled("No destinations (the room files were not found at build time).");
            else
            {
                ImGui::SetNextItemWidth(420);
                if (ImGui::BeginCombo("Destination", warpName(kWarps[selected]).c_str()))
                {
                    for (int i = 0; i < static_cast<int>(kWarpCount); ++i)
                        if (ImGui::Selectable(warpName(kWarps[i]).c_str(), i == selected))
                            selected = i;
                    ImGui::EndCombo();
                }
                if (ImGui::Button("Warp"))
                    requestWarp(static_cast<size_t>(selected));
            }
            ImGui::TextDisabled("Through the game's own door: made at the next moment in play (not during an event).");
            ImGui::TextDisabled("Warping ahead of the story can leave its events out of order; save first.");
            const std::string status = warpStatus();
            if (!status.empty())
                ImGui::TextWrapped("%s", status.c_str());
        });
    }
} // namespace oni::cheats
