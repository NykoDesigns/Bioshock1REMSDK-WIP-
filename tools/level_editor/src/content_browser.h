#pragma once

#include <string>
#include <vector>

class ThumbnailRenderer;

// Content browser panel — lists available meshes from UEViewer export directory
// Displays mesh thumbnails in a grid (like UE4's Content Browser) or as a list.
class ContentBrowser {
public:
    void Render();
    void ScanDirectory(const std::string& exportDir);

    // Set thumbnail renderer (owned externally, e.g. by App)
    void SetThumbnailRenderer(ThumbnailRenderer* renderer) { m_ThumbRenderer = renderer; }

    // Selection
    bool HasSelection() const { return m_SelectedIndex >= 0; }
    const std::string& GetSelectedMesh() const { return m_Meshes[m_SelectedIndex]; }
    int GetSelectedIndex() const { return m_SelectedIndex; }
    void ClearSelection() { m_SelectedIndex = -1; }

    // Drag-and-drop from content browser into viewport
    bool IsDragging() const { return m_IsDragging; }
    const std::string& GetDragMeshName() const { return m_DragMeshName; }
    void ClearDrag() { m_IsDragging = false; m_DragMeshName.clear(); }

    // Legacy spawn (double-click fallback)
    bool WantsSpawn() const { return m_WantsSpawn; }
    void ConsumeSpawn() { m_WantsSpawn = false; }
    const std::string& GetSpawnMesh() const { return m_SpawnMeshName; }

    const std::vector<std::string>& GetMeshNames() const { return m_Meshes; }

private:
    void RenderGridView(const std::string& filterLower);
    void RenderListView(const std::string& filterLower);

    std::vector<std::string> m_Meshes;
    std::string m_ExportDir;
    char m_Filter[256] = "";
    int m_SelectedIndex = -1;
    bool m_WantsSpawn = false;
    std::string m_SpawnMeshName;
    bool m_IsDragging = false;
    std::string m_DragMeshName;
    bool m_GridMode = true; // true = thumbnail grid, false = list
    float m_ThumbDisplaySize = 96.0f; // thumbnail display size in pixels
    ThumbnailRenderer* m_ThumbRenderer = nullptr;
};
