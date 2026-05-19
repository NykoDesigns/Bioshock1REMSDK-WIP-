#include "scene_tree.h"
#include <imgui.h>
#include <algorithm>
#include <cstring>

void SceneTree::Render(BSMDocument& doc, int& selectedActor)
{
    ImGui::Begin("Scene Tree");

    if (!doc.IsLoaded()) {
        ImGui::TextDisabled("No map loaded");
        ImGui::End();
        return;
    }

    // Stats
    ImGui::Text("%s", doc.GetMapName().c_str());
    ImGui::Text("Actors: %d | Names: %d | Exports: %d",
                doc.GetActorCount(), doc.GetNameCount(), doc.GetExportCount());
    ImGui::Separator();

    // Filter
    ImGui::InputText("Filter", filterText, sizeof(filterText));

    // Category toggles
    ImGui::Checkbox("Spawners", &showSpawners); ImGui::SameLine();
    ImGui::Checkbox("Triggers", &showTriggers); ImGui::SameLine();
    ImGui::Checkbox("Lights", &showLights); ImGui::SameLine();
    ImGui::Checkbox("Doors", &showDoors);
    ImGui::Checkbox("Meshes", &showMeshes); ImGui::SameLine();
    ImGui::Checkbox("Other", &showOther);
    ImGui::Separator();

    // Actor list
    auto& actors = doc.GetActors();
    std::string filterStr(filterText);

    ImGui::BeginChild("ActorList", ImVec2(0, 0), true);
    for (int i = 0; i < (int)actors.size(); i++) {
        auto& a = actors[i];

        // Category filter
        ActorCategory cat = CategorizeActor(a.className);
        switch (cat) {
            case ActorCategory::Spawner: if (!showSpawners) continue; break;
            case ActorCategory::Trigger: if (!showTriggers) continue; break;
            case ActorCategory::Light:   if (!showLights) continue; break;
            case ActorCategory::Door:    if (!showDoors) continue; break;
            case ActorCategory::StaticMesh: if (!showMeshes) continue; break;
            default: if (!showOther) continue; break;
        }

        // Text filter
        if (filterStr.length() > 0) {
            bool matchClass = a.className.find(filterStr) != std::string::npos;
            bool matchName = a.objectName.find(filterStr) != std::string::npos;
            if (!matchClass && !matchName) continue;
        }

        // Color-code by category
        Vec3 col = CategoryColor(cat);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(col.x, col.y, col.z, 1.0f));

        char label[256];
        snprintf(label, sizeof(label), "[%d] %s - %s", i, a.className.c_str(), a.objectName.c_str());

        if (ImGui::Selectable(label, selectedActor == i)) {
            selectedActor = i;
        }

        // Right-click context menu
        if (ImGui::IsItemClicked(1)) {
            selectedActor = i;
        }
        if (selectedActor == i && ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Focus (F)")) {
                // Handled by app via key
            }
            if (ImGui::MenuItem("Delete")) {
                doc.GetActors().erase(doc.GetActors().begin() + i);
                selectedActor = -1;
                ImGui::EndPopup();
                ImGui::PopStyleColor();
                break;
            }
            ImGui::EndPopup();
        }

        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::End();
}
