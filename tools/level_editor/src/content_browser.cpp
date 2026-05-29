#include "content_browser.h"
#include "thumbnail_renderer.h"
#include <imgui.h>
#include <filesystem>
#include <algorithm>
#include <cstring>

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

    // Header: mesh count + view toggle + thumbnail size slider
    ImGui::Text("%d meshes", (int)m_Meshes.size());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200);
    if (ImGui::Button(m_GridMode ? "List View" : "Grid View", ImVec2(80, 0))) {
        m_GridMode = !m_GridMode;
    }
    if (m_GridMode) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        ImGui::SliderFloat("##sz", &m_ThumbDisplaySize, 48.0f, 192.0f, "%.0f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Thumbnail size");
    }

    // Progress bar for thumbnail generation
    if (m_ThumbRenderer && m_ThumbRenderer->GetGeneratedCount() < m_ThumbRenderer->GetTotalCount()) {
        float pct = (float)m_ThumbRenderer->GetGeneratedCount() / (float)m_ThumbRenderer->GetTotalCount();
        char buf[64];
        snprintf(buf, sizeof(buf), "Generating thumbnails... %d/%d",
                 m_ThumbRenderer->GetGeneratedCount(), m_ThumbRenderer->GetTotalCount());
        ImGui::ProgressBar(pct, ImVec2(-1, 0), buf);
    }

    ImGui::InputText("Search", m_Filter, sizeof(m_Filter));
    ImGui::Separator();

    // Selection info + drag hint
    if (m_SelectedIndex >= 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "Selected: %s", m_Meshes[m_SelectedIndex].c_str());
        ImGui::TextDisabled("Drag into viewport to place, or double-click");
        ImGui::Separator();
    }

    // Build filter string
    std::string filterStr(m_Filter);
    std::string filterLower = filterStr;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

    // Render content
    if (m_GridMode)
        RenderGridView(filterLower);
    else
        RenderListView(filterLower);

    ImGui::End();
}

void ContentBrowser::RenderGridView(const std::string& filterLower)
{
    ImGui::BeginChild("MeshGrid", ImVec2(0, 0), true);

    float cellSize = m_ThumbDisplaySize + 8.0f; // padding
    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columns = (std::max)(1, (int)(panelWidth / cellSize));
    float cellWidth = panelWidth / (float)columns;

    int col = 0;
    for (int i = 0; i < (int)m_Meshes.size(); i++) {
        // Filter
        if (!filterLower.empty()) {
            std::string nameLower = m_Meshes[i];
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            if (nameLower.find(filterLower) == std::string::npos) continue;
        }

        if (col > 0) ImGui::SameLine();

        ImGui::BeginGroup();
        ImGui::PushID(i);

        bool selected = (i == m_SelectedIndex);

        // Thumbnail image or placeholder
        unsigned int texId = 0;
        if (m_ThumbRenderer) {
            texId = m_ThumbRenderer->GetThumbnailByName(m_Meshes[i]);
        }

        // Selection highlight
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        if (selected) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                cursorPos,
                ImVec2(cursorPos.x + m_ThumbDisplaySize, cursorPos.y + m_ThumbDisplaySize + 20),
                IM_COL32(50, 120, 200, 80), 4.0f);
        }

        if (texId) {
            // Render the thumbnail
            if (ImGui::ImageButton("##thumb", (ImTextureID)(intptr_t)texId,
                                   ImVec2(m_ThumbDisplaySize, m_ThumbDisplaySize))) {
                m_SelectedIndex = i;
            }
        } else {
            // Placeholder: dark box with mesh initial
            ImVec2 sz(m_ThumbDisplaySize, m_ThumbDisplaySize);
            if (ImGui::Button("##ph", sz)) {
                m_SelectedIndex = i;
            }
            // Draw text initial centered
            ImVec2 btnMin = ImGui::GetItemRectMin();
            ImVec2 btnMax = ImGui::GetItemRectMax();
            char initial[2] = { m_Meshes[i][0], 0 };
            ImVec2 textSz = ImGui::CalcTextSize(initial);
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(btnMin.x + (btnMax.x - btnMin.x - textSz.x) * 0.5f,
                       btnMin.y + (btnMax.y - btnMin.y - textSz.y) * 0.5f),
                IM_COL32(180, 180, 180, 200), initial);
        }

        // Drag-and-drop source
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const char* name = m_Meshes[i].c_str();
            ImGui::SetDragDropPayload("MESH_ASSET", name, strlen(name) + 1);
            // Preview tooltip while dragging
            if (texId) {
                ImGui::Image((ImTextureID)(intptr_t)texId, ImVec2(64, 64));
                ImGui::SameLine();
            }
            ImGui::Text("%s", m_Meshes[i].c_str());
            m_IsDragging = true;
            m_DragMeshName = m_Meshes[i];
            ImGui::EndDragDropSource();
        }

        // Double-click to spawn (fallback)
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            m_WantsSpawn = true;
            m_SpawnMeshName = m_Meshes[i];
            m_SelectedIndex = i;
        }

        // Hover tooltip with full name
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s\n(drag into viewport or double-click)", m_Meshes[i].c_str());
        }

        // Mesh name below thumbnail (truncated)
        std::string displayName = m_Meshes[i];
        float maxTextWidth = m_ThumbDisplaySize;
        ImVec2 nameSz = ImGui::CalcTextSize(displayName.c_str());
        if (nameSz.x > maxTextWidth) {
            // Truncate with ellipsis
            while (displayName.size() > 3 && ImGui::CalcTextSize(displayName.c_str()).x > maxTextWidth - 12) {
                displayName.pop_back();
            }
            displayName += "...";
        }
        ImGui::TextUnformatted(displayName.c_str());

        ImGui::PopID();
        ImGui::EndGroup();

        col++;
        if (col >= columns) col = 0;
    }

    ImGui::EndChild();
}

void ContentBrowser::RenderListView(const std::string& filterLower)
{
    ImGui::BeginChild("MeshList", ImVec2(0, 0), true);

    for (int i = 0; i < (int)m_Meshes.size(); i++) {
        // Filter
        if (!filterLower.empty()) {
            std::string nameLower = m_Meshes[i];
            std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
            if (nameLower.find(filterLower) == std::string::npos) continue;
        }

        bool selected = (i == m_SelectedIndex);
        ImGui::PushID(i);

        // Small thumbnail next to name in list view
        unsigned int texId = 0;
        if (m_ThumbRenderer) {
            texId = m_ThumbRenderer->GetThumbnailByName(m_Meshes[i]);
        }
        if (texId) {
            ImGui::Image((ImTextureID)(intptr_t)texId, ImVec2(24, 24));
            ImGui::SameLine();
        }

        if (ImGui::Selectable(m_Meshes[i].c_str(), selected, 0, ImVec2(0, 24))) {
            m_SelectedIndex = i;
        }
        // Drag-and-drop source (list view)
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            const char* name = m_Meshes[i].c_str();
            ImGui::SetDragDropPayload("MESH_ASSET", name, strlen(name) + 1);
            if (texId) {
                ImGui::Image((ImTextureID)(intptr_t)texId, ImVec2(48, 48));
                ImGui::SameLine();
            }
            ImGui::Text("%s", m_Meshes[i].c_str());
            m_IsDragging = true;
            m_DragMeshName = m_Meshes[i];
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            m_WantsSpawn = true;
            m_SpawnMeshName = m_Meshes[i];
            m_SelectedIndex = i;
        }

        ImGui::PopID();
    }

    ImGui::EndChild();
}
