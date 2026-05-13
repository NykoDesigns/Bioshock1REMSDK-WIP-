#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace bs1sdk {

/// .BSM file format parser and editor.
///
/// FORMAT STATUS: CONFIRMED via EliotVU/Unreal-Library cross-reference + hex validation.
/// BioShock Remastered uses Unreal Engine 2.5 Vengeance package format:
///   Magic: 0x9E2A83C1, FileVersion: 142, LicenseeVersion: 56
///
/// BioShock-specific differences from standard UE2.5:
///   - FName in file = CompactIndex(name_index) + int32(number+1)  [always includes Number]
///   - Name table strings: CompactIndex length, negated for Vengeance → always UTF-16LE
///   - Export table has 2 extra int32 fields (unknown purpose, version >= 130/132)
///   - Export ObjectFlags is uint64 (not uint32) for licenseeVersion >= 40
///   - Compact indices (1-5 bytes) used for ClassIndex, SuperIndex, SerialSize, SerialOffset
///
/// Tested successfully on:
///   Entry.bsm (20KB):   153 names, 21 imports, 40 exports
///   1-Medical.bsm (204MB): 29295 names, 1381 imports, 49140 exports
///   0-Lighthouse.bsm (187MB): 24096 names, 596 imports, 22780 exports

// ─── Confirmed Header Structure ──────────────────────────────────────────

#pragma pack(push, 1)

struct BSMHeader {
    uint32_t Magic;              // 0x9E2A83C1 (standard UE package signature)
    uint16_t FileVersion;        // 142 for BioShock
    uint16_t LicenseeVersion;    // 56 for BioShock
    uint32_t PackageFlags;       // EPackageFlags
    
    int32_t  NameCount;          // Number of entries in name table
    int32_t  NameOffset;         // File offset to name table
    
    int32_t  ExportCount;        // Number of export entries
    int32_t  ExportOffset;       // File offset to export table
    
    int32_t  ImportCount;        // Number of import entries
    int32_t  ImportOffset;       // File offset to import table
    
    // GUID(16 bytes) + GenerationCount(4) + Generation entries follow
};

/// FName reference in BioShock package files.
/// Always stored as CompactIndex(name table index) + int32(number, stored as value+1).
struct BSMFNameRef {
    int32_t Index;    // Name table index
    int32_t Number;   // Instance number (-1 = no suffix, 0+ = _N display suffix)
};

struct BSMNameEntry {
    // BioShock Vengeance: CompactIndex(len, negated) → UTF-16LE chars, 8 bytes flags
    std::string Name;
    uint64_t Flags;     // 8 bytes for version >= 141
};

struct BSMImportEntry {
    BSMFNameRef ClassPackage; // Package containing the class (e.g. "Core", "Engine")
    BSMFNameRef ClassName;    // Class name (e.g. "Package", "Class", "Texture")
    int32_t     OuterIndex;   // Object reference: outer/package (int32, not compact)
    BSMFNameRef ObjectName;   // Name of the imported object
};

struct BSMExportEntry {
    int32_t     ClassIndex;    // Object reference (compact index): class of this object
    int32_t     SuperIndex;    // Object reference (compact index): parent class
    int32_t     OuterIndex;    // Object reference (int32): outer/package
    int32_t     UnknownBS1;    // BioShock extra field (version >= 132, purpose unknown)
    BSMFNameRef ObjectName;    // FName: name of this exported object
    uint64_t    ObjectFlags;   // uint64 for BioShock (licenseeVersion >= 40)
    int32_t     SerialSize;    // Compact index: size of serialized object data in bytes
    int32_t     SerialOffset;  // Compact index: file offset to serialized data (if size > 0)
    int32_t     UnknownBS2;    // BioShock extra field (version >= 130, purpose unknown)
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
    
    /// Resolve FName reference to display string (with instance number suffix)
    std::string ResolveFName(const BSMFNameRef& fname) const;

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
    
    /// Read a FCompactIndex (1-5 byte variable-length signed integer) from raw data
    static int ReadCompactIndex(const uint8_t* data, size_t maxLen, size_t& bytesRead);
    
    /// Read a FName reference (compact index + int32 number) from raw data
    static BSMFNameRef ReadFNameRef(const uint8_t* data, size_t maxLen, size_t& bytesRead);
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
