#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace bs1sdk {

/// .BSM file format parser and editor.
/// 
/// RESEARCH STATUS: The .bsm format is undocumented and requires reverse engineering.
/// This header defines the EXPECTED structure based on UE2.5 package format knowledge.
/// All structures are provisional and will be updated as RE progresses.
///
/// Strategy:
/// 1. Dump .bsm file headers and compare across multiple maps
/// 2. Hook file I/O to observe how the engine reads .bsm data
/// 3. Set breakpoints on package loading code to trace deserialization
/// 4. Cross-reference with known UE2.5 .u/.utx/.unr formats
///
/// The .bsm format likely shares heritage with Unreal Package format:
/// - Same magic number (0x9E2A83C1) or BioShock-specific variant
/// - Similar table structure (names, imports, exports)
/// - Potentially compressed with zlib
/// - May have additional BioShock-specific sections (streaming hints, etc.)

// ─── Provisional Header Structure ─────────────────────────────────────────

#pragma pack(push, 1)

struct BSMHeader {
    uint32_t Magic;              // Expected: 0x9E2A83C1 or custom
    uint16_t FileVersion;        // Package format version
    uint16_t LicenseeVersion;    // BioShock-specific version
    uint32_t PackageFlags;       // EPackageFlags
    
    int32_t  NameCount;          // Number of entries in name table
    int32_t  NameOffset;         // File offset to name table
    
    int32_t  ExportCount;        // Number of export entries
    int32_t  ExportOffset;       // File offset to export table
    
    int32_t  ImportCount;        // Number of import entries
    int32_t  ImportOffset;       // File offset to import table
    
    // Possibly additional fields:
    // uint32_t GUID[4];         // Package GUID
    // int32_t  GenerationCount; // Generation info
    // uint32_t EngineVersion;
    // uint32_t CookerVersion;
    // int32_t  CompressionFlags;
    // int32_t  CompressedChunkCount;
    // int32_t  CompressedChunkOffset;
};

struct BSMNameEntry {
    // In UE2.5, name entries are length-prefixed strings
    // Format: int32 Length, char[Length], uint32 Flags
    std::string Name;
    uint32_t Flags;
};

struct BSMImportEntry {
    int32_t ClassPackage;    // FName index - package containing the class
    int32_t ClassName;       // FName index - name of the class
    int32_t PackageIndex;    // Index of the package this import is from
    int32_t ObjectName;      // FName index - name of this import
};

struct BSMExportEntry {
    int32_t ClassIndex;      // Object's class (index into import/export table)
    int32_t SuperIndex;      // Object's parent class
    int32_t PackageIndex;    // Outer/package (index in export table, or 0 for root)
    int32_t ObjectName;      // FName index
    uint32_t ObjectFlags;    // EObjectFlags
    int32_t SerialSize;      // Size of serialized object data
    int32_t SerialOffset;    // File offset to serialized data
    // Possibly more fields depending on version
};

#pragma pack(pop)

// ─── High-Level Parser ────────────────────────────────────────────────────

class BSMFile {
public:
    /// Load a .bsm file from disk
    static std::unique_ptr<BSMFile> Load(const std::string& filepath);

    /// Save modifications back to disk
    bool Save(const std::string& filepath) const;

    /// Get header info
    const BSMHeader& GetHeader() const { return m_Header; }

    /// Get all name entries
    const std::vector<BSMNameEntry>& GetNames() const { return m_Names; }

    /// Get all import entries  
    const std::vector<BSMImportEntry>& GetImports() const { return m_Imports; }

    /// Get all export entries
    const std::vector<BSMExportEntry>& GetExports() const { return m_Exports; }

    /// Resolve a name index to string
    std::string ResolveName(int32_t index) const;

    /// Get raw serialized data for an export
    std::vector<uint8_t> GetExportData(int32_t exportIndex) const;

    /// Find exports by class name
    std::vector<int32_t> FindExportsByClass(const std::string& className) const;

    /// Dump human-readable summary
    std::string DumpSummary() const;

    /// Dump all exports with their class and name
    std::string DumpExports() const;

    /// Check if the file was loaded successfully
    bool IsValid() const { return m_Valid; }

    /// Get the raw file data
    const std::vector<uint8_t>& GetRawData() const { return m_RawData; }

private:
    BSMFile() = default;

    bool ParseHeader();
    bool ParseNames();
    bool ParseImports();
    bool ParseExports();

    BSMHeader m_Header{};
    std::vector<BSMNameEntry> m_Names;
    std::vector<BSMImportEntry> m_Imports;
    std::vector<BSMExportEntry> m_Exports;
    std::vector<uint8_t> m_RawData;
    bool m_Valid = false;
};

// ─── BSM Format Detection ─────────────────────────────────────────────────

/// Utility to analyze unknown .bsm files and determine format details
class BSMFormatAnalyzer {
public:
    struct AnalysisResult {
        bool isValidPackage = false;
        uint32_t magic = 0;
        uint16_t version = 0;
        bool isCompressed = false;
        std::string compressionType;
        size_t headerSize = 0;
        std::vector<std::string> observations;
    };

    /// Analyze a .bsm file and report findings
    static AnalysisResult Analyze(const std::string& filepath);

    /// Compare two .bsm files to identify common structure
    static std::string CompareFiles(const std::string& file1, const std::string& file2);
};

} // namespace bs1sdk
