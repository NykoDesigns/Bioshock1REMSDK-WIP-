#include "properties_panel.h"
#include <imgui.h>

void PropertiesPanel::Render(BSMDocument& doc, int& selectedActor)
{
    ImGui::Begin("Properties");

    if (!doc.IsLoaded() || selectedActor < 0 || selectedActor >= doc.GetActorCount()) {
        ImGui::TextDisabled("No actor selected");
        ImGui::End();
        return;
    }

    auto& actor = doc.GetActors()[selectedActor];

    // Header
    ImGui::Text("Class: %s", actor.className.c_str());
    ImGui::Text("Name: %s", actor.objectName.c_str());
    ImGui::Text("Export #%d | Size: %d bytes", actor.exportIndex, actor.serialSize);
    ImGui::Separator();

    // Visibility toggle
    ImGui::Checkbox("Visible", &actor.visible);

    // Transform
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        float loc[3] = {actor.location.x, actor.location.y, actor.location.z};
        if (ImGui::DragFloat3("Location", loc, 10.0f)) {
            Vec3 newLoc = {loc[0], loc[1], loc[2]};
            doc.SetActorLocation(selectedActor, newLoc);
        }

        float rot[3] = {actor.rotation.x, actor.rotation.y, actor.rotation.z};
        if (ImGui::DragFloat3("Rotation", rot, 1.0f)) {
            Vec3 newRot = {rot[0], rot[1], rot[2]};
            doc.SetActorRotation(selectedActor, newRot);
        }

        float sc[3] = {actor.scale.x, actor.scale.y, actor.scale.z};
        if (ImGui::DragFloat3("Scale", sc, 0.05f, 0.01f, 100.0f)) {
            actor.scale = {sc[0], sc[1], sc[2]};
        }
    }

    // Mesh info
    if (ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (actor.meshIndex >= 0 && actor.meshIndex < (int)doc.GetMeshes().size()) {
            auto& mesh = doc.GetMeshes()[actor.meshIndex];
            ImGui::Text("Mesh: %s", mesh.name.c_str());
            ImGui::Text("Vertices: %d | Triangles: %d",
                        (int)mesh.vertices.size(), (int)mesh.triangles.size());
            if (!mesh.textureName.empty())
                ImGui::Text("Texture: %s", mesh.textureName.c_str());
        } else {
            ImGui::TextDisabled("No mesh linked");
        }
    }

    // Info
    if (ImGui::CollapsingHeader("Info")) {
        ActorCategory cat = CategorizeActor(actor.className);
        Vec3 col = CategoryColor(cat);
        ImGui::Text("Category: ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(col.x, col.y, col.z, 1.0f), "%s",
            cat == ActorCategory::Spawner ? "Spawner" :
            cat == ActorCategory::Trigger ? "Trigger" :
            cat == ActorCategory::Light ? "Light" :
            cat == ActorCategory::Door ? "Door" :
            cat == ActorCategory::Pickup ? "Pickup" :
            cat == ActorCategory::Effect ? "Effect" :
            cat == ActorCategory::PlayerStart ? "Player Start" :
            cat == ActorCategory::StaticMesh ? "Static Mesh" : "Other");

        ImGui::Text("Has Location: %s", actor.hasLocation ? "Yes" : "No");
        if (actor.exportIndex >= 0)
            ImGui::Text("Export #%d | Offset: 0x%X", actor.exportIndex, actor.serialOffset);
        else
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "User-created actor");
    }

    // Delete button
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
    if (ImGui::Button("Delete Actor", ImVec2(-1, 28))) {
        doc.GetActors().erase(doc.GetActors().begin() + selectedActor);
        selectedActor = -1;
    }
    ImGui::PopStyleColor();

    ImGui::End();
}
