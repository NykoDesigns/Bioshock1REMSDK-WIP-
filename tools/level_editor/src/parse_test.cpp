// Standalone test for BSM StaticMesh + BSP parsing (no GUI/SDL required)
#include "bsm_document.h"
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <algorithm>

int main(int argc, char* argv[])
{
    const char* mapPath = "D:\\SteamLibrary\\steamapps\\common\\BioShock Remastered\\ContentBaked\\pc\\Maps\\1-Medical.bsm";
    if (argc > 1) mapPath = argv[1];

    printf("=== BSM Parse Test ===\n");
    printf("Loading: %s\n", mapPath);

    BSMDocument doc;
    bool ok = doc.Load(mapPath);

    if (!ok) {
        printf("FAILED to load BSM file!\n");
        return 1;
    }

    printf("OK! Names=%d, Imports=%d, Exports=%d\n",
           doc.GetNameCount(), doc.GetImportCount(), doc.GetExportCount());
    printf("Actors: %d\n", doc.GetActorCount());
    printf("StaticMeshes parsed: %d\n", doc.GetMeshCount());

    // Summarize mesh stats
    int totalVerts = 0, totalTris = 0;
    for (auto& m : doc.GetMeshes()) {
        totalVerts += (int)m.vertices.size();
        totalTris += (int)m.triangles.size();
    }
    printf("Total vertices: %d, Total triangles: %d\n", totalVerts, totalTris);

    // BSP stats
    if (doc.HasBSP()) {
        printf("BSP chunks: %d\n", (int)doc.GetBSPMeshes().size());
        for (auto& b : doc.GetBSPMeshes()) {
            printf("  BSP: %d verts, %d tris\n", (int)b.vertices.size(), (int)b.triangles.size());
        }
    } else {
        printf("No BSP geometry parsed\n");
    }

    // Show first 5 meshes
    printf("\nFirst 5 meshes:\n");
    for (int i = 0; i < 5 && i < doc.GetMeshCount(); i++) {
        auto& m = doc.GetMeshes()[i];
        printf("  [%d] '%s': %d verts, %d tris, uv=(%.3f,%.3f)\n", i, m.name.c_str(),
               (int)m.vertices.size(), (int)m.triangles.size(),
               m.vertices.empty() ? 0 : m.vertices[0].u,
               m.vertices.empty() ? 0 : m.vertices[0].v);
    }

    // Actor linking stats
    int linked = 0, unlinked = 0;
    for (auto& a : doc.GetActors()) {
        if (a.meshIndex >= 0) linked++;
        else unlinked++;
    }
    printf("\nActors: %d linked, %d unlinked\n", linked, unlinked);

    // Debug: find specific gameplay actors
    printf("\n=== GAMEPLAY ACTOR SEARCH ===\n");
    for (auto& a : doc.GetActors()) {
        if (a.className.find("Vending") != std::string::npos ||
            a.className.find("HealthStation") != std::string::npos ||
            a.className.find("GeneBank") != std::string::npos ||
            a.className.find("Circus") != std::string::npos ||
            a.className.find("Container") != std::string::npos ||
            a.className.find("ScriptableMover") != std::string::npos) {
            printf("  [%d] class='%s' name='%s' loc=(%.0f,%.0f,%.0f) hasLoc=%d mesh=%d meshRef='%s'\n",
                   a.exportIndex, a.className.c_str(), a.objectName.c_str(),
                   a.location.x, a.location.y, a.location.z, a.hasLocation,
                   a.meshIndex, a.meshRefName.c_str());
        }
    }

    // Actors with location but no mesh (visible in-game but missing geometry)
    printf("\n=== ACTORS WITH LOCATION BUT NO MESH ===\n");
    std::unordered_map<std::string, int> missingByClass;
    std::unordered_map<std::string, std::string> missingMeshRef;
    for (auto& a : doc.GetActors()) {
        if (a.meshIndex >= 0) continue;
        if (!a.hasLocation) continue;
        missingByClass[a.className]++;
        if (!a.meshRefName.empty() && missingMeshRef.find(a.className) == missingMeshRef.end())
            missingMeshRef[a.className] = a.meshRefName;
    }
    // Sort by count
    std::vector<std::pair<std::string, int>> sorted(missingByClass.begin(), missingByClass.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (auto& [cls, cnt] : sorted) {
        auto it = missingMeshRef.find(cls);
        if (it != missingMeshRef.end())
            printf("  %4d  %-40s meshRef='%s'\n", cnt, cls.c_str(), it->second.c_str());
        else
            printf("  %4d  %s\n", cnt, cls.c_str());
    }

    // ─── Spatial Diagnostics ───
    printf("\n=== SPATIAL DIAGNOSTICS ===\n");

    // BSP bounding box
    if (doc.HasBSP()) {
        float bMin[3] = {1e9f, 1e9f, 1e9f}, bMax[3] = {-1e9f, -1e9f, -1e9f};
        for (auto& b : doc.GetBSPMeshes()) {
            for (auto& v : b.vertices) {
                if (v.x < bMin[0]) bMin[0] = v.x; if (v.x > bMax[0]) bMax[0] = v.x;
                if (v.y < bMin[1]) bMin[1] = v.y; if (v.y > bMax[1]) bMax[1] = v.y;
                if (v.z < bMin[2]) bMin[2] = v.z; if (v.z > bMax[2]) bMax[2] = v.z;
            }
        }
        printf("BSP bounds: (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f)\n",
               bMin[0],bMin[1],bMin[2], bMax[0],bMax[1],bMax[2]);
        printf("BSP size: %.0f x %.0f x %.0f\n",
               bMax[0]-bMin[0], bMax[1]-bMin[1], bMax[2]-bMin[2]);
    }

    // Actor position bounding box (only actors with mesh)
    {
        float aMin[3] = {1e9f, 1e9f, 1e9f}, aMax[3] = {-1e9f, -1e9f, -1e9f};
        int count = 0;
        for (auto& a : doc.GetActors()) {
            if (a.meshIndex < 0) continue;
            if (fabsf(a.location.x) > 500000) continue;
            if (a.location.x < aMin[0]) aMin[0] = a.location.x;
            if (a.location.x > aMax[0]) aMax[0] = a.location.x;
            if (a.location.y < aMin[1]) aMin[1] = a.location.y;
            if (a.location.y > aMax[1]) aMax[1] = a.location.y;
            if (a.location.z < aMin[2]) aMin[2] = a.location.z;
            if (a.location.z > aMax[2]) aMax[2] = a.location.z;
            count++;
        }
        printf("Actor(mesh) bounds: (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f) [%d actors]\n",
               aMin[0],aMin[1],aMin[2], aMax[0],aMax[1],aMax[2], count);
        printf("Actor(mesh) size: %.0f x %.0f x %.0f\n",
               aMax[0]-aMin[0], aMax[1]-aMin[1], aMax[2]-aMin[2]);
    }

    // Sample mesh local-space bounds (first 5 meshes)
    printf("\nMesh local-space bounds (first 10):\n");
    for (int i = 0; i < 10 && i < doc.GetMeshCount(); i++) {
        auto& m = doc.GetMeshes()[i];
        float mMin[3] = {1e9f, 1e9f, 1e9f}, mMax[3] = {-1e9f, -1e9f, -1e9f};
        for (auto& v : m.vertices) {
            if (v.x < mMin[0]) mMin[0] = v.x; if (v.x > mMax[0]) mMax[0] = v.x;
            if (v.y < mMin[1]) mMin[1] = v.y; if (v.y > mMax[1]) mMax[1] = v.y;
            if (v.z < mMin[2]) mMin[2] = v.z; if (v.z > mMax[2]) mMax[2] = v.z;
        }
        printf("  [%d] '%s': (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) size=%.0f\n", i, m.name.c_str(),
               mMin[0],mMin[1],mMin[2], mMax[0],mMax[1],mMax[2],
               fmaxf(mMax[0]-mMin[0], fmaxf(mMax[1]-mMin[1], mMax[2]-mMin[2])));
    }

    // First 10 actors with meshes (show position, rotation, scale, mesh name)
    printf("\nFirst 10 meshed actors:\n");
    int shown = 0;
    for (auto& a : doc.GetActors()) {
        if (a.meshIndex < 0) continue;
        auto& m = doc.GetMeshes()[a.meshIndex];
        printf("  '%s' mesh='%s' pos=(%.0f,%.0f,%.0f) rot=(%.1f,%.1f,%.1f) scale=(%.2f,%.2f,%.2f)\n",
               a.objectName.c_str(), m.name.c_str(),
               a.location.x, a.location.y, a.location.z,
               a.rotation.x, a.rotation.y, a.rotation.z,
               a.scale.x, a.scale.y, a.scale.z);
        if (++shown >= 10) break;
    }

    // ─── BSP Texture Resolution Test ───
    {
        // Derive map name and export dir
        std::string path = mapPath;
        size_t sl = path.find_last_of("\\/");
        std::string fname = (sl != std::string::npos) ? path.substr(sl + 1) : path;
        size_t dp = fname.find_last_of('.');
        if (dp != std::string::npos) fname = fname.substr(0, dp);
        std::string exportDir = "Z:\\UEViewer\\export\\" + fname;
        printf("\n=== TEXTURE RESOLUTION ===\n");
        printf("Export dir: %s\n", exportDir.c_str());
        doc.ResolveTextures(exportDir);

        // Show BSP chunk texture results
        if (doc.HasBSP()) {
            int bspHasTex = 0;
            for (auto& b : doc.GetBSPMeshes())
                if (!b.textureName.empty()) bspHasTex++;
            printf("BSP chunks with texName: %d/%d\n", bspHasTex, (int)doc.GetBSPMeshes().size());
            for (int i = 0; i < 10 && i < (int)doc.GetBSPMeshes().size(); i++)
                printf("  BSP[%d] tex='%s'\n", i, doc.GetBSPMeshes()[i].textureName.c_str());
        }
    }

    printf("\n=== DONE ===\n");
    return 0;
}
