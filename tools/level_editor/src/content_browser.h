#pragma once

#include <string>
#include <vector>

// Content browser panel — lists available meshes from UEViewer export directory
class ContentBrowser {
public:
    void Render();
    void ScanDirectory(const std::string& exportDir);

    // Selection
    bool HasSelection() const { return m_SelectedIndex >= 0; }
    const std::string& GetSelectedMesh() const { return m_Meshes[m_SelectedIndex]; }
    int GetSelectedIndex() const { return m_SelectedIndex; }
    void ClearSelection() { m_SelectedIndex = -1; }

    // Drag-and-drop or spawn request
    bool WantsSpawn() const { return m_WantsSpawn; }
    void ConsumeSpawn() { m_WantsSpawn = false; }
    const std::string& GetSpawnMesh() const { return m_SpawnMeshName; }

    const std::vector<std::string>& GetMeshNames() const { return m_Meshes; }

private:
    std::vector<std::string> m_Meshes;
    std::string m_ExportDir;
    char m_Filter[256] = "";
    int m_SelectedIndex = -1;
    bool m_WantsSpawn = false;
    std::string m_SpawnMeshName;
};
