#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

// Entry from Catalog.bdc: maps a texture object name to a .blk file + offset
struct CatalogEntry {
    std::string objectName;   // e.g. "Texture_49"
    std::string packageName;  // BSM package that owns this texture
    std::string blkFilename;  // e.g. "BulkChunk1_38.blk"
    int dataOffset = 0;       // byte offset into the .blk file
    int dataSize = 0;         // bytes to read (e.g. 696320 for 4-mip DXT1 1024x1024)
};

// Parses BioShock's Catalog.bdc and provides lookup from texture name to .blk location.
// Used to extract lightmap textures that UEViewer can't export (CachedBulkDataSize = -1).
class CatalogParser {
public:
    // Parse a Catalog.bdc file. Returns true on success.
    bool Load(const std::string& bdcPath);

    // Look up a texture by object name. Returns nullptr if not found.
    const CatalogEntry* Find(const std::string& objectName) const;

    // Read raw pixel data for a texture from its .blk file.
    // bulkContentDir = directory containing the .blk files.
    // Returns empty vector on failure.
    std::vector<uint8_t> ReadBulkData(const std::string& objectName,
                                       const std::string& bulkContentDir) const;

    int GetEntryCount() const { return (int)m_Entries.size(); }
    const CatalogEntry* GetEntry(int idx) const { return (idx >= 0 && idx < (int)m_Entries.size()) ? &m_Entries[idx] : nullptr; }

    // Count entries whose objectName starts with a given prefix
    int CountEntriesWithPrefix(const std::string& prefix) const;

    // Get all entries whose packageName contains a substring (e.g. "LightMaps")
    std::vector<const CatalogEntry*> FindByPackage(const std::string& packageSubstr) const;

    // Get the bulk content directory (derived from the .bdc path)
    const std::string& GetBulkDir() const { return m_BulkDir; }

private:
    std::vector<CatalogEntry> m_Entries;
    std::unordered_map<std::string, int> m_NameToIndex; // objectName → index in m_Entries
    std::string m_BulkDir; // directory containing .blk files
};
