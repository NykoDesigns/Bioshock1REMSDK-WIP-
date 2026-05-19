#include "content_browser.h"
#include <imgui.h>
#include <filesystem>
#include <algorithm>

void ContentBrowser::ScanDirectory(const std::string& exportDir)
{
    m_ExportDir = exportDir;
    m_Meshes.clear();
    m_SelectedIndex = -1;

    namespace fs = std::filesystem;
    std::string meshDir = exportDir + "\\StaticMesh";
    if (!fs::is_directory(meshDir)) return;

    for (auto& entry : fs::directory_iterator(meshDir)) {
        if (entry.path().extension() == ".gltf") {
            m_Meshes.push_back(entry.path().stem().string());
        }
    }
    std::sort(m_Meshes.begin(), m_Meshes.end());
    printf("[ContentBrowser] Found %d meshes in %s\n", (int)m_Meshes.size(), meshDir.c_str());
}

void ContentBrowser::Render()
{
    ImGui::Begin("Content Browser");

    if (m_Meshes.empty()) {
        ImGui::TextDisabled("No content loaded. Open a map first.");
        ImGui::End();
        return;
    }

    ImGui::Text("%d meshes available", (int)m_Meshes.size());
    ImGui::InputText("Search", m_Filter, sizeof(m_Filter));
    ImGui::Separator();

    // Spawn button
    if (m_SelectedIndex >= 0) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        if (ImGui::Button("Add to Scene", ImVec2(-1, 30))) {
            m_WantsSpawn = true;
            m_SpawnMeshName = m_Meshes[m_SelectedIndex];
        }
        ImGui::PopStyleColor();
        ImGui::Text("Selected: %s", m_Meshes[m_SelectedIndex].c_str());
        ImGui::Separator();
    }

    // Mesh list with grid-style layout
    std::string filterStr(m_Filter);
    // Convert filter to lowercase for case-insensitive matching
    std::string filterLower = filterStr;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

    ImGui::BeginChild("MeshList", ImVec2(0, 0), true);

    int columns = std::max(1, (int)(ImGui::GetContentRegionAvail().x / 180.0f));
    ImGui::Columns(columns, nullptr, false);

    int shown = 0;
    for (int i = 0; i < (int)m_Meshes.size(); i++) {
        // Filter
        if (!filterLower.empty()) {
            std::string nameLower = m_Meshes[i];
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            if (nameLower.find(filterLower) == std::string::npos) continue;
        }

        bool selected = (i == m_SelectedIndex);
        ImGui::PushID(i);

        // Simple text button styled as a card
        if (ImGui::Selectable(m_Meshes[i].c_str(), selected, 0, ImVec2(0, 20))) {
            m_SelectedIndex = i;
        }

        // Double-click to spawn
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            m_WantsSpawn = true;
            m_SpawnMeshName = m_Meshes[i];
            m_SelectedIndex = i;
        }

        ImGui::PopID();
        ImGui::NextColumn();
        shown++;
    }

    ImGui::Columns(1);
    ImGui::EndChild();

    ImGui::End();
}
