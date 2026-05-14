#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <algorithm>
#include <map>
#include <cstdint>

/// BSM Tool - Standalone .bsm/.U file analyzer
/// 
/// Usage:
///   bsm_tool analyze <file>           - Analyze header structure
///   bsm_tool names <file>             - Dump all name table entries
///   bsm_tool imports <file>           - Dump import table
///   bsm_tool exports <file>           - Dump export table with resolved names
///   bsm_tool actors <file>            - Dump only Actor-derived exports
///   bsm_tool dump <file>              - Full dump (names + imports + exports)
///   bsm_tool hexdump <file> [off] [len] - Hex dump
///   bsm_tool compare <a> <b>          - Compare two file headers
///
/// Supports BioShock Remastered .bsm (maps) and .U (script) packages.
/// Package version 142, licensee 56 (UE2.5 Vengeance engine).

constexpr uint32_t UE_PACKAGE_MAGIC = 0x9E2A83C1;

// ─── Package Header ─────────────────────────────────────────────────────

struct PackageHeader {
    uint32_t magic;
    uint16_t fileVersion;
    uint16_t licenseeVersion;
    uint32_t packageFlags;
    int32_t  nameCount;
    int32_t  nameOffset;
    int32_t  exportCount;
    int32_t  exportOffset;
    int32_t  importCount;
    int32_t  importOffset;
    // GUID and generation info follow but are variable-length
};

struct NameEntry {
    std::string name;
    uint64_t    flags;
};

// FName in BioShock packages: compact_index (name table index) + int32 (number, stored as value+1)
struct FNameRef {
    int32_t index;   // name table index
    int32_t number;  // instance number (-1 = no suffix, 0+ = _N suffix)
};

struct ImportEntry {
    FNameRef classPackage;
    FNameRef className;
    int32_t  outerIndex;    // object reference (neg=import, pos=export, 0=none)
    FNameRef objectName;
};

struct ExportEntry {
    int32_t  classIndex;    // object reference (compact index)
    int32_t  superIndex;    // object reference (compact index)
    int32_t  outerIndex;    // object reference (int32, not compact)
    int32_t  unknownBS1;    // BioShock extra field (version >= 132)
    FNameRef objectName;    // FName (compact index + int32 number)
    uint64_t objectFlags;   // BioShock: uint64 (licensee >= 40)
    int32_t  serialSize;    // compact index
    int32_t  serialOffset;  // compact index (if serialSize > 0)
    int32_t  unknownBS2;    // BioShock extra field (version >= 130)
    std::vector<uint8_t> rawEntry;  // raw bytes of the export table entry (for round-trip)
};

struct ParsedPackage {
    PackageHeader header{};
    std::vector<NameEntry> names;
    std::vector<ImportEntry> imports;
    std::vector<ExportEntry> exports;
    std::vector<uint8_t> rawData;
    bool valid = false;
    
    std::string ResolveName(int32_t idx) const {
        if (idx >= 0 && idx < (int32_t)names.size()) return names[idx].name;
        return "<invalid_" + std::to_string(idx) + ">";
    }
    
    // Resolve FName to string (with optional instance number suffix)
    std::string ResolveFName(const FNameRef& fn) const {
        std::string base = ResolveName(fn.index);
        if (fn.number >= 0) base += "_" + std::to_string(fn.number);
        return base;
    }
    
    // Resolve an object reference to a human-readable string
    // Positive = export (1-based), negative = import (negated 1-based), 0 = none
    std::string ResolveObjRef(int32_t ref) const {
        if (ref == 0) return "None";
        if (ref > 0) {
            int idx = ref - 1;
            if (idx < (int32_t)exports.size())
                return ResolveFName(exports[idx].objectName) + " [Export#" + std::to_string(ref) + "]";
            return "<bad_export_" + std::to_string(ref) + ">";
        } else {
            int idx = -ref - 1;
            if (idx < (int32_t)imports.size())
                return ResolveFName(imports[idx].objectName) + "." + ResolveFName(imports[idx].className)
                       + " [Import#" + std::to_string(-ref) + "]";
            return "<bad_import_" + std::to_string(ref) + ">";
        }
    }
    
    // Get the class name for an object reference
    std::string ResolveClassName(int32_t ref) const {
        if (ref == 0) return "Class"; // Class=0 means UClass itself
        if (ref < 0) {
            int idx = -ref - 1;
            if (idx < (int32_t)imports.size())
                return ResolveFName(imports[idx].objectName);
        } else {
            int idx = ref - 1;
            if (idx < (int32_t)exports.size())
                return ResolveFName(exports[idx].objectName);
        }
        return "<unknown>";
    }
};

// ─── Compact Index Reader ───────────────────────────────────────────────
// UE2.5 version 142 uses compact indices (variable 1-5 bytes).
// Bit layout:
//   Byte 0: bit7=sign, bit6=continue, bits5-0 = 6 data bits
//   Bytes 1-3: bit7=continue, bits6-0 = 7 data bits each
//   Byte 4: all 8 bits are data

static int ReadCompactIndex(const uint8_t* data, size_t maxLen, size_t& bytesRead) {
    int output = 0;
    bool isNeg = false;
    bytesRead = 0;
    
    for (int i = 0; i < 5 && bytesRead < maxLen; i++) {
        uint8_t b = data[bytesRead++];
        
        if (i == 0) {
            isNeg = (b & 0x80) != 0;
            output = b & 0x3F;
            if ((b & 0x40) == 0) break;
        } else if (i == 4) {
            output |= (b & 0x1F) << (6 + 3 * 7);
        } else {
            output |= (b & 0x7F) << (6 + (i - 1) * 7);
            if ((b & 0x80) == 0) break;
        }
    }
    
    return isNeg ? -output : output;
}

void HexDump(const uint8_t* data, size_t size, size_t baseOffset = 0)
{
    for (size_t i = 0; i < size; i += 16) {
        std::printf("%08zX: ", baseOffset + i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < size) std::printf("%02X ", data[i + j]);
            else std::printf("   ");
            if (j == 7) std::printf(" ");
        }
        std::printf(" |");
        for (size_t j = 0; j < 16 && (i + j) < size; ++j) {
            char c = static_cast<char>(data[i + j]);
            std::printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        std::printf("|\n");
    }
}

// ─── Compact Index Writer ───────────────────────────────────────────────
// Inverse of ReadCompactIndex — encodes an int32 to 1-5 byte UE2.5 format.

static std::vector<uint8_t> WriteCompactIndex(int value)
{
    std::vector<uint8_t> buf;
    bool isNeg = value < 0;
    uint32_t absVal = isNeg ? (uint32_t)(-value) : (uint32_t)value;

    // Byte 0: bit7=sign, bit6=continue, bits5-0 = low 6 data bits
    uint8_t b0 = absVal & 0x3F;
    absVal >>= 6;
    if (isNeg) b0 |= 0x80;
    if (absVal > 0) b0 |= 0x40;
    buf.push_back(b0);

    // Bytes 1-3: bit7=continue, bits6-0 = 7 data bits each
    for (int i = 1; i <= 3 && absVal > 0; i++) {
        uint8_t bn = absVal & 0x7F;
        absVal >>= 7;
        if (absVal > 0) bn |= 0x80;
        buf.push_back(bn);
    }

    // Byte 4: remaining bits (up to 5 bits in byte 4)
    if (absVal > 0) {
        buf.push_back((uint8_t)(absVal & 0x1F));
    }

    return buf;
}

// ─── Export Entry Writer ────────────────────────────────────────────────
// Serializes an ExportEntry to bytes (BioShock v142 format).

static std::vector<uint8_t> WriteExportEntry(const ExportEntry& exp)
{
    std::vector<uint8_t> buf;
    auto append = [&](const std::vector<uint8_t>& v) { buf.insert(buf.end(), v.begin(), v.end()); };
    auto appendI32 = [&](int32_t v) {
        buf.push_back((uint8_t)(v)); buf.push_back((uint8_t)(v>>8));
        buf.push_back((uint8_t)(v>>16)); buf.push_back((uint8_t)(v>>24));
    };
    auto appendU32 = [&](uint32_t v) { appendI32((int32_t)v); };
    auto appendU64 = [&](uint64_t v) {
        appendU32((uint32_t)(v & 0xFFFFFFFF));
        appendU32((uint32_t)(v >> 32));
    };

    append(WriteCompactIndex(exp.classIndex));
    append(WriteCompactIndex(exp.superIndex));
    appendI32(exp.outerIndex);
    appendI32(exp.unknownBS1);
    // FName: compact_index + int32 (stored as number+1)
    append(WriteCompactIndex(exp.objectName.index));
    appendI32(exp.objectName.number + 1); // stored as value+1
    appendU64(exp.objectFlags);
    append(WriteCompactIndex(exp.serialSize));
    if (exp.serialSize > 0) {
        append(WriteCompactIndex(exp.serialOffset));
    }
    appendI32(exp.unknownBS2);
    return buf;
}

// ─── Full Package Parser ────────────────────────────────────────────────

ParsedPackage ParsePackage(const std::string& filepath)
{
    ParsedPackage pkg;
    
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open: " << filepath << "\n";
        return pkg;
    }
    
    size_t fileSize = file.tellg();
    file.seekg(0);
    pkg.rawData.resize(fileSize);
    file.read(reinterpret_cast<char*>(pkg.rawData.data()), fileSize);
    
    if (fileSize < 36) return pkg;
    
    // Parse header
    auto* d = pkg.rawData.data();
    pkg.header.magic            = *reinterpret_cast<uint32_t*>(d + 0);
    pkg.header.fileVersion      = *reinterpret_cast<uint16_t*>(d + 4);
    pkg.header.licenseeVersion  = *reinterpret_cast<uint16_t*>(d + 6);
    pkg.header.packageFlags     = *reinterpret_cast<uint32_t*>(d + 8);
    pkg.header.nameCount        = *reinterpret_cast<int32_t*>(d + 12);
    pkg.header.nameOffset       = *reinterpret_cast<int32_t*>(d + 16);
    pkg.header.exportCount      = *reinterpret_cast<int32_t*>(d + 20);
    pkg.header.exportOffset     = *reinterpret_cast<int32_t*>(d + 24);
    pkg.header.importCount      = *reinterpret_cast<int32_t*>(d + 28);
    pkg.header.importOffset     = *reinterpret_cast<int32_t*>(d + 32);
    
    if (pkg.header.magic != UE_PACKAGE_MAGIC) {
        std::cerr << "Error: Bad magic\n";
        return pkg;
    }
    
    // ─── Parse Name Table ────────────────────────────────────────
    // BioShock Vengeance (v>=135): ReadIndex then negate → always unicode
    // Format: compact_index (negated = wchar count), UTF-16LE chars, 8 bytes flags
    size_t pos = pkg.header.nameOffset;
    for (int i = 0; i < pkg.header.nameCount && pos < fileSize; ++i) {
        NameEntry ne;
        
        size_t br = 0;
        int rawLen = ReadCompactIndex(d + pos, fileSize - pos, br);
        pos += br;
        
        // Vengeance engine negates the length to force unicode
        int charCount = (rawLen < 0) ? -rawLen : rawLen;
        bool isUnicode = (rawLen < 0) || (pkg.header.fileVersion >= 135); // BioShock always unicode
        
        if (charCount <= 0 || charCount > 65536) {
            ne.name = "<invalid>";
            pkg.names.push_back(ne);
            break;
        }
        
        if (isUnicode) {
            // UTF-16LE
            for (int c = 0; c < charCount && pos + 1 < fileSize; ++c) {
                uint16_t wc = *reinterpret_cast<uint16_t*>(d + pos);
                pos += 2;
                if (wc == 0) { break; }
                ne.name += (wc < 128) ? static_cast<char>(wc) : '?';
            }
            // Skip null terminator if not consumed
            if (pos + 1 < fileSize && *reinterpret_cast<uint16_t*>(d + pos) == 0) {
                pos += 2;
            }
        } else {
            // ASCII (shouldn't happen for BioShock but kept for robustness)
            for (int c = 0; c < charCount && pos < fileSize; ++c) {
                char ch = static_cast<char>(d[pos++]);
                if (ch == 0) break;
                ne.name += ch;
            }
        }
        
        // Name flags: 8 bytes for version >= 141
        if (pos + 8 <= fileSize) {
            ne.flags = *reinterpret_cast<uint64_t*>(d + pos);
            pos += 8;
        }
        
        pkg.names.push_back(ne);
    }
    
    // Helper: read FName (compact index + int32 number) from BioShock package
    auto ReadFName = [&](size_t& p) -> FNameRef {
        FNameRef fn;
        size_t br;
        fn.index  = ReadCompactIndex(d + p, fileSize - p, br); p += br;
        fn.number = *reinterpret_cast<int32_t*>(d + p) - 1;    p += 4; // stored as value+1
        return fn;
    };
    
    // ─── Parse Import Table ──────────────────────────────────────
    // BioShock: ClassPackage(FName) + ClassName(FName) + OuterIndex(int32) + ObjectName(FName)
    pos = pkg.header.importOffset;
    for (int i = 0; i < pkg.header.importCount && pos < fileSize; ++i) {
        ImportEntry ie;
        
        ie.classPackage = ReadFName(pos);
        ie.className    = ReadFName(pos);
        ie.outerIndex   = *reinterpret_cast<int32_t*>(d + pos); pos += 4;
        ie.objectName   = ReadFName(pos);
        
        pkg.imports.push_back(ie);
    }
    
    // ─── Parse Export Table ──────────────────────────────────────
    // BioShock (v142, licensee 56): has 2 extra int32 fields and uint64 ObjectFlags
    pos = pkg.header.exportOffset;
    for (int i = 0; i < pkg.header.exportCount && pos < fileSize; ++i) {
        ExportEntry ee;
        size_t br;
        size_t entryStart = pos;
        
        ee.classIndex  = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        ee.superIndex  = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        ee.outerIndex  = *reinterpret_cast<int32_t*>(d + pos); pos += 4;
        ee.unknownBS1  = *reinterpret_cast<int32_t*>(d + pos); pos += 4; // BioShock extra (v>=132)
        ee.objectName  = ReadFName(pos);
        ee.objectFlags = *reinterpret_cast<uint64_t*>(d + pos); pos += 8; // BioShock: uint64
        ee.serialSize  = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        if (ee.serialSize > 0) {
            ee.serialOffset = ReadCompactIndex(d + pos, fileSize - pos, br); pos += br;
        } else {
            ee.serialOffset = 0;
        }
        ee.unknownBS2  = *reinterpret_cast<int32_t*>(d + pos); pos += 4; // BioShock extra (v>=130)
        
        // Capture raw entry bytes for round-trip writing
        ee.rawEntry.assign(d + entryStart, d + pos);
        
        pkg.exports.push_back(ee);
    }
    
    pkg.valid = true;
    return pkg;
}

void AnalyzeFile(const std::string& filepath)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) {
        std::cerr << "Error: Failed to parse package\n";
        return;
    }
    
    size_t fileSize = pkg.rawData.size();
    auto& h = pkg.header;
    
    std::cout << "================================================================\n";
    std::cout << " BSM File Analysis: " << std::filesystem::path(filepath).filename().string() << "\n";
    std::cout << "================================================================\n";
    std::cout << " File size: " << fileSize << " bytes (" 
              << (fileSize / 1024.0 / 1024.0) << " MB)\n";
    std::cout << " Magic: 0x" << std::hex << std::uppercase << h.magic << std::dec << "\n";
    std::cout << " File Version: " << h.fileVersion << "\n";
    std::cout << " Licensee Version: " << h.licenseeVersion << "\n";
    std::cout << " Package Flags: 0x" << std::hex << h.packageFlags << std::dec << "\n";
    std::cout << "\n Name Table:   " << h.nameCount   << " entries at 0x" << std::hex << h.nameOffset << std::dec
              << " (parsed: " << pkg.names.size() << ")\n";
    std::cout << " Import Table: " << h.importCount << " entries at 0x" << std::hex << h.importOffset << std::dec
              << " (parsed: " << pkg.imports.size() << ")\n";
    std::cout << " Export Table: " << h.exportCount << " entries at 0x" << std::hex << h.exportOffset << std::dec
              << " (parsed: " << pkg.exports.size() << ")\n";
    
    // Validation
    std::cout << "\n Validation:\n";
    bool nameOk = (int32_t)pkg.names.size() == h.nameCount;
    bool impOk  = (int32_t)pkg.imports.size() == h.importCount;
    bool expOk  = (int32_t)pkg.exports.size() == h.exportCount;
    std::cout << "   Names:   " << (nameOk ? "OK" : "MISMATCH") << "\n";
    std::cout << "   Imports: " << (impOk  ? "OK" : "MISMATCH") << "\n";
    std::cout << "   Exports: " << (expOk  ? "OK" : "MISMATCH") << "\n";
    
    // First 5 exports as sample
    if (!pkg.exports.empty()) {
        std::cout << "\n First 5 exports:\n";
        for (int i = 0; i < std::min(5, (int)pkg.exports.size()); ++i) {
            auto& e = pkg.exports[i];
            std::cout << "   [" << i << "] " << pkg.ResolveClassName(e.classIndex) 
                      << " '" << pkg.ResolveFName(e.objectName) << "'"
                      << " size=" << e.serialSize << " offset=0x" << std::hex << e.serialOffset << std::dec
                      << " flags=0x" << std::hex << e.objectFlags << std::dec << "\n";
        }
    }
}

void CompareFiles(const std::string& file1, const std::string& file2)
{
    auto pkg1 = ParsePackage(file1);
    auto pkg2 = ParsePackage(file2);
    
    if (!pkg1.valid || !pkg2.valid) {
        std::cerr << "Error: Cannot parse one or both files\n";
        return;
    }
    
    std::cout << "Comparing packages:\n";
    std::cout << "  File 1: " << file1 << "\n";
    std::cout << "  File 2: " << file2 << "\n\n";
    
    std::cout << "Field          | File 1        | File 2        | Match\n";
    std::cout << "---------------|---------------|---------------|------\n";
    
    auto cmp = [](const char* label, auto v1, auto v2) {
        bool match = (v1 == v2);
        std::cout << " " << std::left << std::setw(14) << label 
                  << "| " << std::setw(14) << v1 
                  << "| " << std::setw(14) << v2 
                  << "| " << (match ? "YES" : "NO <<<") << "\n";
    };
    
    cmp("Version",  pkg1.header.fileVersion, pkg2.header.fileVersion);
    cmp("Licensee", pkg1.header.licenseeVersion, pkg2.header.licenseeVersion);
    cmp("Names",    pkg1.header.nameCount, pkg2.header.nameCount);
    cmp("Imports",  pkg1.header.importCount, pkg2.header.importCount);
    cmp("Exports",  pkg1.header.exportCount, pkg2.header.exportCount);
}

// ─── Dump Commands ────────────────────────────────────────────────────────

void DumpNames(const std::string& filepath)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }
    
    std::cout << "Name Table: " << pkg.names.size() << " entries\n\n";
    for (int i = 0; i < (int)pkg.names.size(); ++i) {
        std::printf("[%5d] %s\n", i, pkg.names[i].name.c_str());
    }
}

void DumpImports(const std::string& filepath)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }
    
    std::cout << "Import Table: " << pkg.imports.size() << " entries\n\n";
    std::cout << std::left
              << std::setw(6)  << "#"
              << std::setw(30) << "ClassPackage"
              << std::setw(30) << "ClassName"
              << std::setw(10) << "Package"
              << std::setw(30) << "ObjectName" << "\n";
    std::cout << std::string(106, '-') << "\n";
    
    for (int i = 0; i < (int)pkg.imports.size(); ++i) {
        auto& imp = pkg.imports[i];
        std::cout << std::setw(6)  << i
                  << std::setw(30) << pkg.ResolveFName(imp.classPackage)
                  << std::setw(30) << pkg.ResolveFName(imp.className)
                  << std::setw(10) << imp.outerIndex
                  << std::setw(30) << pkg.ResolveFName(imp.objectName) << "\n";
    }
}

void DumpExports(const std::string& filepath)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }
    
    std::cout << "Export Table: " << pkg.exports.size() << " entries\n\n";
    std::cout << std::left
              << std::setw(6)  << "#"
              << std::setw(30) << "Class"
              << std::setw(30) << "ObjectName"
              << std::setw(10) << "Size"
              << std::setw(12) << "Offset"
              << std::setw(10) << "Outer" << "\n";
    std::cout << std::string(98, '-') << "\n";
    
    for (int i = 0; i < (int)pkg.exports.size(); ++i) {
        auto& exp = pkg.exports[i];
        std::string className = pkg.ResolveClassName(exp.classIndex);
        std::string objName = pkg.ResolveFName(exp.objectName);
        
        char offsetBuf[16];
        std::snprintf(offsetBuf, sizeof(offsetBuf), "0x%X", exp.serialOffset);
        
        std::string outer = (exp.outerIndex != 0) 
            ? pkg.ResolveObjRef(exp.outerIndex) : "";
        
        std::cout << std::setw(6)  << i
                  << std::setw(30) << className
                  << std::setw(30) << objName
                  << std::setw(10) << exp.serialSize
                  << std::setw(12) << offsetBuf
                  << outer << "\n";
    }
}

void DumpActors(const std::string& filepath)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }
    
    std::cout << "Actor-like exports in: " << std::filesystem::path(filepath).filename().string() << "\n\n";
    
    int count = 0;
    for (int i = 0; i < (int)pkg.exports.size(); ++i) {
        auto& exp = pkg.exports[i];
        std::string className = pkg.ResolveClassName(exp.classIndex);
        std::string objName = pkg.ResolveFName(exp.objectName);
        
        // Skip structural/metadata classes
        if (className == "Class" || className == "Function" || className == "Struct" ||
            className == "State" || className == "Enum" || className == "Const" ||
            className == "ScriptStruct" || className == "ByteProperty" || 
            className == "IntProperty" || className == "FloatProperty" ||
            className == "StrProperty" || className == "NameProperty" ||
            className == "BoolProperty" || className == "ObjectProperty" ||
            className == "ClassProperty" || className == "ArrayProperty" ||
            className == "StructProperty" || className == "DelegateProperty" ||
            className == "ComponentProperty" || className == "InterfaceProperty")
            continue;
        
        std::printf("[%5d] %-35s %s  (size=%d, offset=0x%X)\n",
                    i, className.c_str(), objName.c_str(), exp.serialSize, exp.serialOffset);
        count++;
    }
    std::cout << "\nTotal non-structural exports: " << count << "\n";
}

void DumpFull(const std::string& filepath)
{
    std::cout << "=== NAMES ==="  << "\n";
    DumpNames(filepath);
    std::cout << "\n=== IMPORTS ==="  << "\n";
    DumpImports(filepath);
    std::cout << "\n=== EXPORTS ==="  << "\n";
    DumpExports(filepath);
}

// ─── Property Deserialization ────────────────────────────────────────────
// Ported from TheWarInRapture bsm_parser.py parse_properties
// Reads UE1-style property tags from serialized object data.

enum PropType {
    PT_None = 0, PT_Byte = 1, PT_Int = 2, PT_Bool = 3, PT_Float = 4,
    PT_Object = 5, PT_Name = 6, PT_String = 7, PT_Class = 8, PT_Array = 9,
    PT_Struct = 10, PT_Vector = 11, PT_Rotator = 12, PT_Str = 13,
    PT_Map = 14, PT_FixedArray = 15
};

static const char* PropTypeName(int t) {
    static const char* names[] = {
        "None","Byte","Int","Bool","Float","Object","Name","String",
        "Class","Array","Struct","Vector","Rotator","Str","Map","FixedArray"
    };
    return (t >= 0 && t <= 15) ? names[t] : "Unknown";
}

struct SerialProp {
    std::string name;
    int type;
    int size;
    int arrayIndex;
    size_t valueOffset;  // file offset of value data
    std::vector<uint8_t> value;
    bool boolValue;
    std::string structName;
};

static int DecodePackedSize(const uint8_t* d, size_t& pos, int sizeBits) {
    switch (sizeBits) {
        case 0: return 1;   case 1: return 2;
        case 2: return 4;   case 3: return 12;
        case 4: return 16;
        case 5: return d[pos++];
        case 6: { uint16_t v; memcpy(&v, d+pos, 2); pos += 2; return v; }
        case 7: { uint32_t v; memcpy(&v, d+pos, 4); pos += 4; return (int)v; }
        default: return 0;
    }
}

// Read compact index for property parser (same encoding, simpler signature)
static int ReadCI(const uint8_t* d, size_t& pos) {
    uint8_t b0 = d[pos++];
    bool sign = (b0 & 0x80) != 0;
    bool more = (b0 & 0x40) != 0;
    int val = b0 & 0x3F;
    if (more) {
        uint8_t b1 = d[pos++]; val |= (b1 & 0x7F) << 6;
        if (b1 & 0x80) { uint8_t b2 = d[pos++]; val |= (b2 & 0x7F) << 13;
            if (b2 & 0x80) { uint8_t b3 = d[pos++]; val |= (b3 & 0x7F) << 20;
                if (b3 & 0x80) { uint8_t b4 = d[pos++]; val |= (b4 & 0x7F) << 27; }
            }
        }
    }
    return sign ? -val : val;
}

// Read a BioShock name ref for property parser: compact_index + INT32 number
struct PropNameRef { int idx; uint32_t num; };
static PropNameRef ReadPropNameRef(const uint8_t* d, size_t& pos) {
    PropNameRef r;
    r.idx = ReadCI(d, pos);
    memcpy(&r.num, d + pos, 4); pos += 4;
    return r;
}

std::vector<SerialProp> ParseProperties(const uint8_t* data, size_t pos,
                                         const std::vector<NameEntry>& names,
                                         size_t endPos)
{
    std::vector<SerialProp> props;

    while (pos < endPos - 5) {
        SerialProp p;
        p.boolValue = false;
        p.arrayIndex = 0;

        auto nr = ReadPropNameRef(data, pos);
        if (nr.idx == 0) {
            p.name = "None"; p.type = PT_None; p.size = 0;
            props.push_back(p);
            break;
        }
        if (nr.idx < 0 || nr.idx >= (int)names.size()) break;

        p.name = names[nr.idx].name;
        if (nr.num > 0) p.name += "_" + std::to_string(nr.num);

        if (pos >= endPos) break;

        uint8_t info = data[pos++];
        p.type = info & 0x0F;
        int sizeBits = (info >> 4) & 0x07;
        int arrayFlag = (info >> 7) & 1;

        // Struct: extra name ref for struct type
        if (p.type == PT_Struct) {
            auto sn = ReadPropNameRef(data, pos);
            if (sn.idx >= 0 && sn.idx < (int)names.size())
                p.structName = names[sn.idx].name;
        }

        p.size = DecodePackedSize(data, pos, sizeBits);

        // Bool: value in flag bit. Otherwise array index.
        if (p.type == PT_Bool) {
            p.boolValue = (arrayFlag != 0);
        } else if (arrayFlag) {
            uint8_t b = data[pos++];
            if ((b & 0x80) == 0) p.arrayIndex = b;
            else if ((b & 0xC0) == 0x80) { uint8_t c = data[pos++]; p.arrayIndex = ((b&0x7F)<<8)+c; }
            else { uint8_t c=data[pos++],d2=data[pos++],e=data[pos++]; p.arrayIndex=((b&0x3F)<<24)+(c<<16)+(d2<<8)+e; }
        }

        p.valueOffset = pos;
        if (p.size > 0 && pos + p.size <= endPos)
            p.value.assign(data + pos, data + pos + p.size);
        pos += p.size;

        props.push_back(p);
    }
    return props;
}

// Auto-detect header skip bytes before properties start
// Ported from TheWarInRapture bsm_spawn_patcher.py detect_header_skip
int DetectHeaderSkip(const uint8_t* data, size_t offset, int size,
                     const std::vector<NameEntry>& names)
{
    int bestSkip = 57;
    int bestScore = 0;
    static const char* knownProps[] = {
        "Tag","Location","Rotation","Label","Region","Level",
        "SpawnZones","RepopulationPatrol","RepopulationAITypes",
        "PhysicsVolume","CheckpointTypePadding","DrawScale3D",
        "StaticMesh","CollisionRadius","CollisionHeight"
    };

    for (int skip = 4; skip < 80 && skip < size; skip++) {
        try {
            auto props = ParseProperties(data, offset + skip, names, offset + size);
            int score = 0;
            for (auto& p : props) {
                if (p.type == PT_None) continue;
                for (auto& kp : knownProps)
                    if (p.name == kp) score++;
            }
            if (score > bestScore) { bestScore = score; bestSkip = skip; }
        } catch (...) { continue; }
    }
    return bestSkip;
}

void DumpProps(const std::string& filepath, int exportIdx)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }

    if (exportIdx < 0 || exportIdx >= (int)pkg.exports.size()) {
        std::cerr << "Error: Export index out of range (0-" << pkg.exports.size()-1 << ")\n";
        return;
    }

    auto& exp = pkg.exports[exportIdx];
    std::string className = pkg.ResolveClassName(exp.classIndex);
    std::string objName = pkg.ResolveFName(exp.objectName);

    std::cout << "Properties for export [" << exportIdx << "] "
              << className << " '" << objName << "'"
              << " (serial size=" << exp.serialSize << " offset=0x" << std::hex << exp.serialOffset << std::dec << ")\n\n";

    if (exp.serialSize <= 0) {
        std::cout << "  No serial data.\n";
        return;
    }

    int skip = DetectHeaderSkip(pkg.rawData.data(), exp.serialOffset, exp.serialSize, pkg.names);
    std::cout << "  Header skip: " << skip << " bytes\n\n";

    auto props = ParseProperties(pkg.rawData.data(), exp.serialOffset + skip,
                                  pkg.names, exp.serialOffset + exp.serialSize);

    for (auto& p : props) {
        if (p.type == PT_None) { std::cout << "  [None] (end)\n"; break; }

        std::printf("  %-30s %-10s size=%-4d", p.name.c_str(), PropTypeName(p.type), p.size);

        if (!p.structName.empty())
            std::printf(" struct=%s", p.structName.c_str());
        if (p.arrayIndex > 0)
            std::printf(" [%d]", p.arrayIndex);

        // Print value interpretation
        if (p.type == PT_Bool) {
            std::printf(" = %s", p.boolValue ? "true" : "false");
        } else if (p.type == PT_Int && p.value.size() == 4) {
            int32_t v; memcpy(&v, p.value.data(), 4);
            std::printf(" = %d", v);
        } else if (p.type == PT_Float && p.value.size() == 4) {
            float v; memcpy(&v, p.value.data(), 4);
            std::printf(" = %.4f", v);
        } else if (p.type == PT_Object && p.value.size() >= 1) {
            size_t vpos = 0;
            int ref = ReadCI(p.value.data(), vpos);
            std::printf(" = %s", pkg.ResolveObjRef(ref).c_str());
        } else if (p.type == PT_Name && p.value.size() >= 5) {
            size_t vpos = 0;
            auto nr = ReadPropNameRef(p.value.data(), vpos);
            if (nr.idx >= 0 && nr.idx < (int)pkg.names.size())
                std::printf(" = '%s'", pkg.names[nr.idx].name.c_str());
        } else if ((p.type == PT_Struct || p.type == PT_Vector) && p.value.size() == 12) {
            float x, y, z;
            memcpy(&x, p.value.data(), 4);
            memcpy(&y, p.value.data()+4, 4);
            memcpy(&z, p.value.data()+8, 4);
            std::printf(" = (%.1f, %.1f, %.1f)", x, y, z);
        } else if (p.type == PT_Rotator && p.value.size() == 12) {
            int32_t pitch, yaw, roll;
            memcpy(&pitch, p.value.data(), 4);
            memcpy(&yaw, p.value.data()+4, 4);
            memcpy(&roll, p.value.data()+8, 4);
            std::printf(" = (P=%d, Y=%d, R=%d)", pitch, yaw, roll);
        } else if (p.type == PT_Byte && p.value.size() == 1) {
            std::printf(" = %d", p.value[0]);
        }

        std::printf("\n");
    }
}

void DumpSpawners(const std::string& filepath)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }

    std::cout << "Spawners in: " << std::filesystem::path(filepath).filename().string() << "\n\n";

    static const char* spawnerClasses[] = {
        "AggressorSpawner", "ProtectorSpawner", "SecurityBotSpawner",
        "ProtectorControls", "ActionSpawnAI"
    };

    int total = 0;
    for (int i = 0; i < (int)pkg.exports.size(); ++i) {
        auto& exp = pkg.exports[i];
        std::string cn = pkg.ResolveClassName(exp.classIndex);

        bool isSpawner = false;
        for (auto& sc : spawnerClasses)
            if (cn == sc) { isSpawner = true; break; }
        if (!isSpawner) continue;

        std::string objName = pkg.ResolveFName(exp.objectName);
        std::printf("[%5d] %-25s %-25s", i, cn.c_str(), objName.c_str());

        if (exp.serialSize > 0) {
            int skip = DetectHeaderSkip(pkg.rawData.data(), exp.serialOffset, exp.serialSize, pkg.names);
            auto props = ParseProperties(pkg.rawData.data(), exp.serialOffset + skip,
                                          pkg.names, exp.serialOffset + exp.serialSize);
            for (auto& p : props) {
                if (p.name == "Location" && p.value.size() == 12) {
                    float x, y, z;
                    memcpy(&x, p.value.data(), 4);
                    memcpy(&y, p.value.data()+4, 4);
                    memcpy(&z, p.value.data()+8, 4);
                    std::printf("  loc=(%.0f, %.0f, %.0f)", x, y, z);
                }
            }
        }
        std::printf("\n");
        total++;
    }
    std::cout << "\nTotal spawners: " << total << "\n";
}

// ═══════════════════════════════════════════════════════════════════════
// Goal 1: BSM Spawn Patcher — duplicate spawners to multiply enemy count
// ═══════════════════════════════════════════════════════════════════════
// Port of TheWarInRapture/core/bsm_spawn_patcher.py to C++.
// Strategy:
//   1. Copy all data up to name table (header + original serial data)
//   2. Append cloned serial data with offset positions
//   3. Append name table (unchanged)
//   4. Append import table (unchanged)
//   5. Append original export entries (raw bytes) + new export entries
//   6. Patch header (counts, table offsets, generation)

void PatchSpawners(const std::string& filepath, int multiplier, bool dryRun = false)
{
    if (multiplier < 2 || multiplier > 10) {
        std::cerr << "Error: multiplier must be 2-10\n";
        return;
    }

    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }

    std::string mapName = std::filesystem::path(filepath).filename().string();
    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << "  PATCH: " << mapName << " (x" << multiplier << ")\n";
    std::cout << "═══════════════════════════════════════════════════════════\n";

    const auto& names = pkg.names;
    const auto& d = pkg.rawData.data();

    static const char* spawnerClasses[] = {
        "AggressorSpawner", "ProtectorSpawner", "SecurityBotSpawner"
    };

    // Find spawners and their locations
    struct SpawnerInfo {
        int exportIdx;
        std::string className;
        float x, y, z;
        int headerSkip;
        int locationValuePos; // offset of Location value within serial data (relative to serial start)
    };

    std::vector<SpawnerInfo> spawners;

    for (int i = 0; i < (int)pkg.exports.size(); i++) {
        auto& exp = pkg.exports[i];
        std::string cn = pkg.ResolveClassName(exp.classIndex);

        bool isSpawner = false;
        for (auto& sc : spawnerClasses)
            if (cn == sc) { isSpawner = true; break; }
        if (!isSpawner || exp.serialSize <= 0) continue;

        int skip = DetectHeaderSkip(d, exp.serialOffset, exp.serialSize, names);
        auto props = ParseProperties(d, exp.serialOffset + skip, names, exp.serialOffset + exp.serialSize);

        SpawnerInfo si{};
        si.exportIdx = i;
        si.className = cn;
        si.headerSkip = skip;
        si.locationValuePos = -1;

        for (auto& p : props) {
            if (p.name == "Location" && p.value.size() == 12) {
                memcpy(&si.x, p.value.data(), 4);
                memcpy(&si.y, p.value.data()+4, 4);
                memcpy(&si.z, p.value.data()+8, 4);
                // p.valueOffset is the absolute file position of the value data.
                si.locationValuePos = (int)(p.valueOffset - exp.serialOffset);
            }
        }

        if (si.locationValuePos < 0) continue; // skip if no Location

        spawners.push_back(si);
        std::printf("  [%5d] %-22s loc=(%.0f, %.0f, %.0f)\n",
                     i, cn.c_str(), si.x, si.y, si.z);
    }

    std::cout << "\n  Found " << spawners.size() << " spawners\n";

    int dupsPerSpawner = multiplier - 1;
    int totalNew = (int)spawners.size() * dupsPerSpawner;
    std::cout << "  Creating " << totalNew << " clones (" << dupsPerSpawner << " per spawner)\n\n";

    if (spawners.empty() || totalNew == 0) {
        std::cout << "  Nothing to patch.\n";
        return;
    }

    // Find max name_num per class for unique numbering
    std::map<std::string, int32_t> maxNameNum;
    for (auto& sp : spawners) {
        auto& exp = pkg.exports[sp.exportIdx];
        auto it = maxNameNum.find(sp.className);
        if (it == maxNameNum.end() || exp.objectName.number > it->second)
            maxNameNum[sp.className] = exp.objectName.number;
    }

    // Build new exports
    struct NewExport {
        ExportEntry entry;
        std::vector<uint8_t> serialData;
    };
    std::vector<NewExport> newExports;

    constexpr float OFFSET_DIST = 150.0f;
    constexpr float PI = 3.14159265358979f;

    for (auto& sp : spawners) {
        auto& origExp = pkg.exports[sp.exportIdx];
        int32_t& nextNum = maxNameNum[sp.className];

        for (int dup = 0; dup < dupsPerSpawner; dup++) {
            nextNum++;

            // Calculate offset position (radial distribution)
            float angle = (2.0f * PI * dup) / (float)dupsPerSpawner;
            float nx = sp.x + OFFSET_DIST * cosf(angle);
            float ny = sp.y + OFFSET_DIST * sinf(angle);
            float nz = sp.z;

            // Clone serial data
            std::vector<uint8_t> serial(d + origExp.serialOffset,
                                        d + origExp.serialOffset + origExp.serialSize);

            // Patch Location in cloned data
            if (sp.locationValuePos >= 0 && sp.locationValuePos + 12 <= (int)serial.size()) {
                memcpy(serial.data() + sp.locationValuePos,     &nx, 4);
                memcpy(serial.data() + sp.locationValuePos + 4, &ny, 4);
                memcpy(serial.data() + sp.locationValuePos + 8, &nz, 4);
            }

            // Build new export entry
            ExportEntry ne{};
            ne.classIndex = origExp.classIndex;
            ne.superIndex = origExp.superIndex;
            ne.outerIndex = origExp.outerIndex;
            ne.unknownBS1 = origExp.unknownBS1;
            ne.objectName.index = origExp.objectName.index;
            ne.objectName.number = nextNum;
            ne.objectFlags = origExp.objectFlags;
            ne.serialSize = (int32_t)serial.size();
            ne.serialOffset = 0; // will be set during write
            ne.unknownBS2 = origExp.unknownBS2;

            newExports.push_back({ne, std::move(serial)});

            std::printf("  NEW %-22s #%-4d from #%-4d  loc=(%.0f, %.0f, %.0f)\n",
                         sp.className.c_str(), nextNum, origExp.objectName.number, nx, ny, nz);
        }
    }

    std::cout << "\n  Total: " << spawners.size() << " original + "
              << newExports.size() << " clones = "
              << spawners.size() + newExports.size() << " spawners\n";

    if (dryRun) {
        std::cout << "\n  [DRY RUN — no files modified]\n";
        return;
    }

    // === WRITE PATCHED FILE ===
    std::cout << "\n  WRITING PATCHED FILE...\n";

    // 1. Copy everything up to name table (header + all original serial data)
    std::vector<uint8_t> output(d, d + pkg.header.nameOffset);

    // 2. Append new serial data (record offsets)
    for (auto& ne : newExports) {
        ne.entry.serialOffset = (int32_t)output.size();
        output.insert(output.end(), ne.serialData.begin(), ne.serialData.end());
    }

    // Pad to 4 bytes
    while (output.size() % 4 != 0) output.push_back(0);

    // 3. Append name table (unchanged)
    int32_t newNameOffset = (int32_t)output.size();
    output.insert(output.end(), d + pkg.header.nameOffset, d + pkg.header.importOffset);

    // 4. Append import table (unchanged)
    int32_t newImportOffset = (int32_t)output.size();
    output.insert(output.end(), d + pkg.header.importOffset, d + pkg.header.exportOffset);

    // 5. Append export table
    int32_t newExportOffset = (int32_t)output.size();

    // Original entries (raw bytes for exact round-trip)
    for (auto& exp : pkg.exports) {
        output.insert(output.end(), exp.rawEntry.begin(), exp.rawEntry.end());
    }

    // New entries (serialized from struct)
    for (auto& ne : newExports) {
        auto bytes = WriteExportEntry(ne.entry);
        output.insert(output.end(), bytes.begin(), bytes.end());
    }

    int32_t newExportCount = (int32_t)(pkg.exports.size() + newExports.size());

    // 6. Patch header
    memcpy(output.data() + 16, &newNameOffset, 4);    // NameOffset
    memcpy(output.data() + 20, &newExportCount, 4);   // ExportCount
    memcpy(output.data() + 24, &newExportOffset, 4);  // ExportOffset
    memcpy(output.data() + 32, &newImportOffset, 4);  // ImportOffset

    // Patch generation table (offset 56 = Gen[0].ExportCount)
    memcpy(output.data() + 56, &newExportCount, 4);

    // Backup original
    std::filesystem::path backupDir = std::filesystem::path(filepath).parent_path() / "backups";
    std::filesystem::create_directories(backupDir);
    std::filesystem::path backupPath = backupDir / mapName;
    if (!std::filesystem::exists(backupPath)) {
        std::filesystem::copy_file(filepath, backupPath);
        std::cout << "  Backed up: " << backupPath.string() << "\n";
    }

    // Write
    std::ofstream outFile(filepath, std::ios::binary);
    if (!outFile.is_open()) {
        std::cerr << "Error: Cannot write to " << filepath << "\n";
        return;
    }
    outFile.write(reinterpret_cast<char*>(output.data()), output.size());
    outFile.close();

    size_t origSize = pkg.rawData.size();
    std::cout << "  Written: " << filepath << "\n";
    std::cout << "  Size: " << origSize << " -> " << output.size()
              << " (+" << (output.size() - origSize) << " bytes)\n";
}

// ═══════════════════════════════════════════════════════════════════════
// Goal 4: BSM Property Editor — modify property values in export serial data
// ═══════════════════════════════════════════════════════════════════════
// Patches a specific property value in a specific export entry, in-place.
// Supports: Int, Float, Vector (Location/Rotation), Name, Bool, Byte
// For vectors: provide "x,y,z" as the value string.

void SetExportProp(const std::string& filepath, int exportIdx,
                   const std::string& propName, const std::string& valueStr)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }

    if (exportIdx < 0 || exportIdx >= (int)pkg.exports.size()) {
        std::cerr << "Error: Export index out of range (0-" << pkg.exports.size()-1 << ")\n";
        return;
    }

    auto& exp = pkg.exports[exportIdx];
    std::string className = pkg.ResolveClassName(exp.classIndex);
    std::string objName = pkg.ResolveFName(exp.objectName);

    if (exp.serialSize <= 0) {
        std::cerr << "Error: Export has no serial data\n";
        return;
    }

    int skip = DetectHeaderSkip(pkg.rawData.data(), exp.serialOffset, exp.serialSize, pkg.names);
    auto props = ParseProperties(pkg.rawData.data(), exp.serialOffset + skip,
                                  pkg.names, exp.serialOffset + exp.serialSize);

    bool found = false;
    for (auto& p : props) {
        if (p.name != propName) continue;
        found = true;

        size_t valuePos = p.valueOffset; // absolute file position of value data

        if ((p.type == PT_Struct || p.type == PT_Vector) && p.value.size() == 12) {
            // Parse "x,y,z"
            float x, y, z;
            char comma1, comma2;
            std::istringstream iss(valueStr);
            if (!(iss >> x >> comma1 >> y >> comma2 >> z) || comma1 != ',' || comma2 != ',') {
                std::cerr << "Error: Vector format must be 'x,y,z' e.g. '100.0,200.0,300.0'\n";
                return;
            }
            float oldX, oldY, oldZ;
            memcpy(&oldX, pkg.rawData.data() + valuePos, 4);
            memcpy(&oldY, pkg.rawData.data() + valuePos + 4, 4);
            memcpy(&oldZ, pkg.rawData.data() + valuePos + 8, 4);

            memcpy(pkg.rawData.data() + valuePos,     &x, 4);
            memcpy(pkg.rawData.data() + valuePos + 4, &y, 4);
            memcpy(pkg.rawData.data() + valuePos + 8, &z, 4);

            std::printf("  [%d] %s.%s '%s': (%.1f, %.1f, %.1f) -> (%.1f, %.1f, %.1f)\n",
                         exportIdx, className.c_str(), objName.c_str(), propName.c_str(),
                         oldX, oldY, oldZ, x, y, z);

        } else if (p.type == PT_Float && p.value.size() == 4) {
            float newVal = std::stof(valueStr);
            float oldVal;
            memcpy(&oldVal, pkg.rawData.data() + valuePos, 4);
            memcpy(pkg.rawData.data() + valuePos, &newVal, 4);
            std::printf("  [%d] %s.%s '%s': %.4f -> %.4f\n",
                         exportIdx, className.c_str(), objName.c_str(), propName.c_str(),
                         oldVal, newVal);

        } else if (p.type == PT_Int && p.value.size() == 4) {
            int32_t newVal = std::stoi(valueStr);
            int32_t oldVal;
            memcpy(&oldVal, pkg.rawData.data() + valuePos, 4);
            memcpy(pkg.rawData.data() + valuePos, &newVal, 4);
            std::printf("  [%d] %s.%s '%s': %d -> %d\n",
                         exportIdx, className.c_str(), objName.c_str(), propName.c_str(),
                         oldVal, newVal);

        } else if (p.type == PT_Byte && p.value.size() == 1) {
            uint8_t newVal = (uint8_t)std::stoi(valueStr);
            uint8_t oldVal = pkg.rawData[valuePos];
            pkg.rawData[valuePos] = newVal;
            std::printf("  [%d] %s.%s '%s': %d -> %d\n",
                         exportIdx, className.c_str(), objName.c_str(), propName.c_str(),
                         oldVal, newVal);

        } else {
            std::cerr << "Error: Unsupported property type for editing (type=" << p.type
                      << " size=" << p.value.size() << ")\n";
            return;
        }
        break;
    }

    if (!found) {
        std::cerr << "Error: Property '" << propName << "' not found in export " << exportIdx << "\n";
        return;
    }

    // Backup
    std::string mapName = std::filesystem::path(filepath).filename().string();
    std::filesystem::path backupDir = std::filesystem::path(filepath).parent_path() / "backups";
    std::filesystem::create_directories(backupDir);
    std::filesystem::path backupPath = backupDir / mapName;
    if (!std::filesystem::exists(backupPath)) {
        std::filesystem::copy_file(filepath, backupPath);
        std::cout << "  Backed up: " << backupPath.string() << "\n";
    }

    // Write the modified raw data back
    std::ofstream outFile(filepath, std::ios::binary);
    if (!outFile.is_open()) {
        std::cerr << "Error: Cannot write to " << filepath << "\n";
        return;
    }
    outFile.write(reinterpret_cast<char*>(pkg.rawData.data()), pkg.rawData.size());
    outFile.close();
    std::cout << "  Written: " << filepath << "\n";
}

// ═══════════════════════════════════════════════════════════════════════
// Goal 4: Custom Splicer Types — RepopulationAITypes array editing
// ═══════════════════════════════════════════════════════════════════════
// RepopulationAITypes is array<class<Pawn>> on AggressorSpawner.
// Serialized as: Array property tag + value(INT32 count + count*INT32 refs)
// Each ref is an object reference (positive=export, negative=import, 0=none)

void DumpAITypes(const std::string& filepath, int exportIdx)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }

    if (exportIdx < 0 || exportIdx >= (int)pkg.exports.size()) {
        std::cerr << "Error: Export index out of range (0-" << pkg.exports.size()-1 << ")\n";
        return;
    }

    auto& exp = pkg.exports[exportIdx];
    std::string className = pkg.ResolveClassName(exp.classIndex);
    std::string objName = pkg.ResolveFName(exp.objectName);

    std::printf("RepopulationAITypes for [%d] %s '%s'\n\n", exportIdx,
                 className.c_str(), objName.c_str());

    if (exp.serialSize <= 0) {
        std::cout << "  No serial data.\n";
        return;
    }

    int skip = DetectHeaderSkip(pkg.rawData.data(), exp.serialOffset, exp.serialSize, pkg.names);
    auto props = ParseProperties(pkg.rawData.data(), exp.serialOffset + skip,
                                  pkg.names, exp.serialOffset + exp.serialSize);

    bool found = false;
    for (auto& p : props) {
        if (p.name == "RepopulationAITypes" && p.type == PT_Array) {
            found = true;
            if (p.value.size() < 4) {
                std::cout << "  Array data too small\n";
                break;
            }

            // Read count as INT32
            int32_t count;
            memcpy(&count, p.value.data(), 4);

            // Validate: count * 4 + 4 should equal p.value.size()
            size_t expectedSize = 4 + (size_t)count * 4;
            if (expectedSize != p.value.size()) {
                std::printf("  WARNING: Array size mismatch (count=%d, expected %zu bytes, got %zu)\n",
                             count, expectedSize, p.value.size());
                // Try compact index interpretation
                size_t ciPos = 0;
                int ciCount = ReadCI(p.value.data(), ciPos);
                size_t ciExpected = ciPos + (size_t)ciCount * 4;
                if (ciExpected == p.value.size()) {
                    std::printf("  (Using compact-index count: %d)\n", ciCount);
                    count = ciCount;
                    std::printf("\n  RepopulationAITypes: %d entries\n\n", count);
                    for (int i = 0; i < count && ciPos + 4 <= p.value.size(); i++) {
                        int32_t ref;
                        memcpy(&ref, p.value.data() + ciPos, 4);
                        ciPos += 4;
                        std::printf("    [%d] ref=%-6d  %s\n", i, ref,
                                     pkg.ResolveObjRef(ref).c_str());
                    }
                    break;
                }
            }

            std::printf("  RepopulationAITypes: %d entries (value_offset=0x%zX)\n\n",
                         count, p.valueOffset);

            for (int i = 0; i < count; i++) {
                size_t elemOff = 4 + (size_t)i * 4;
                if (elemOff + 4 > p.value.size()) break;
                int32_t ref;
                memcpy(&ref, p.value.data() + elemOff, 4);
                std::printf("    [%d] ref=%-6d  %s\n", i, ref,
                             pkg.ResolveObjRef(ref).c_str());
            }
            break;
        }
    }

    if (!found) {
        std::cout << "  RepopulationAITypes property not found on this export.\n";
        std::cout << "  Available properties:\n";
        for (auto& p : props) {
            if (p.type != PT_None)
                std::printf("    %-30s %s (size=%d)\n",
                             p.name.c_str(), PropTypeName(p.type), p.size);
        }
    }
}

void SetAIType(const std::string& filepath, int exportIdx, int arrayIdx, int32_t newRef)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }

    if (exportIdx < 0 || exportIdx >= (int)pkg.exports.size()) {
        std::cerr << "Error: Export index out of range\n";
        return;
    }

    auto& exp = pkg.exports[exportIdx];
    if (exp.serialSize <= 0) { std::cerr << "Error: No serial data\n"; return; }

    int skip = DetectHeaderSkip(pkg.rawData.data(), exp.serialOffset, exp.serialSize, pkg.names);
    auto props = ParseProperties(pkg.rawData.data(), exp.serialOffset + skip,
                                  pkg.names, exp.serialOffset + exp.serialSize);

    for (auto& p : props) {
        if (p.name != "RepopulationAITypes" || p.type != PT_Array) continue;
        if (p.value.size() < 4) { std::cerr << "Error: Array too small\n"; return; }

        int32_t count;
        memcpy(&count, p.value.data(), 4);

        // Detect format: INT32 count vs compact-index count
        size_t headerSize = 4; // default: INT32 count
        size_t expectedSize = 4 + (size_t)count * 4;
        if (expectedSize != p.value.size()) {
            // Try compact-index count
            size_t ciPos = 0;
            int ciCount = ReadCI(p.value.data(), ciPos);
            if (ciPos + (size_t)ciCount * 4 == p.value.size()) {
                count = ciCount;
                headerSize = ciPos;
            } else {
                std::cerr << "Error: Cannot determine array format\n";
                return;
            }
        }

        if (arrayIdx < 0 || arrayIdx >= count) {
            std::cerr << "Error: Array index " << arrayIdx << " out of range (0-"
                      << count - 1 << ")\n";
            return;
        }

        // Read old value
        size_t elemFileOff = p.valueOffset + headerSize + (size_t)arrayIdx * 4;
        int32_t oldRef;
        memcpy(&oldRef, pkg.rawData.data() + elemFileOff, 4);

        // Write new value
        memcpy(pkg.rawData.data() + elemFileOff, &newRef, 4);

        std::string className = pkg.ResolveClassName(exp.classIndex);
        std::string objName = pkg.ResolveFName(exp.objectName);
        std::printf("  [%d] %s '%s' RepopulationAITypes[%d]:\n",
                     exportIdx, className.c_str(), objName.c_str(), arrayIdx);
        std::printf("    OLD: ref=%-6d  %s\n", oldRef, pkg.ResolveObjRef(oldRef).c_str());
        std::printf("    NEW: ref=%-6d  %s\n", newRef, pkg.ResolveObjRef(newRef).c_str());

        // Backup
        std::string mapName = std::filesystem::path(filepath).filename().string();
        auto backupDir = std::filesystem::path(filepath).parent_path() / "backups";
        std::filesystem::create_directories(backupDir);
        auto backupPath = backupDir / mapName;
        if (!std::filesystem::exists(backupPath)) {
            std::filesystem::copy_file(filepath, backupPath);
            std::cout << "  Backed up: " << backupPath.string() << "\n";
        }

        // Write
        std::ofstream outFile(filepath, std::ios::binary);
        if (!outFile.is_open()) { std::cerr << "Error: Cannot write\n"; return; }
        outFile.write(reinterpret_cast<char*>(pkg.rawData.data()), pkg.rawData.size());
        outFile.close();
        std::cout << "  Written: " << filepath << "\n";
        return;
    }

    std::cerr << "Error: RepopulationAITypes not found on export " << exportIdx << "\n";
}

void FindImportByName(const std::string& filepath, const std::string& pattern)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }

    auto toLower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    std::string lp = toLower(pattern);

    std::printf("Imports matching '%s':\n\n", pattern.c_str());
    int count = 0;
    for (int i = 0; i < (int)pkg.imports.size(); i++) {
        auto& imp = pkg.imports[i];
        std::string objName = pkg.ResolveFName(imp.objectName);
        std::string clsName = pkg.ResolveFName(imp.className);
        std::string pkgName = pkg.ResolveFName(imp.classPackage);

        if (toLower(objName).find(lp) != std::string::npos ||
            toLower(clsName).find(lp) != std::string::npos) {
            // Import refs are negative 1-based
            int ref = -(i + 1);
            std::printf("  [Import %d]  ref=%-6d  %s.%s  (pkg: %s)\n",
                         i, ref, clsName.c_str(), objName.c_str(), pkgName.c_str());
            count++;
        }
    }
    std::printf("\n  %d matches\n", count);
}

void FindExportByName(const std::string& filepath, const std::string& pattern)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }

    auto toLower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    std::string lp = toLower(pattern);

    std::printf("Exports matching '%s':\n\n", pattern.c_str());
    int count = 0;
    for (int i = 0; i < (int)pkg.exports.size(); i++) {
        auto& exp = pkg.exports[i];
        std::string objName = pkg.ResolveFName(exp.objectName);
        std::string clsName = pkg.ResolveClassName(exp.classIndex);

        if (toLower(objName).find(lp) != std::string::npos ||
            toLower(clsName).find(lp) != std::string::npos) {
            int ref = i + 1; // Export refs are positive 1-based
            std::printf("  [Export %d]  ref=%-6d  %s '%s'  (size=%d)\n",
                         i, ref, clsName.c_str(), objName.c_str(), exp.serialSize);
            count++;
        }
    }
    std::printf("\n  %d matches\n", count);
}

void FindNameInTable(const std::string& filepath, const std::string& pattern)
{
    auto pkg = ParsePackage(filepath);
    if (!pkg.valid) { std::cerr << "Error: Failed to parse\n"; return; }

    auto toLower = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    std::string lp = toLower(pattern);

    std::printf("Names matching '%s':\n\n", pattern.c_str());
    int count = 0;
    for (int i = 0; i < (int)pkg.names.size(); i++) {
        if (toLower(pkg.names[i].name).find(lp) != std::string::npos) {
            std::printf("  [%5d] %s\n", i, pkg.names[i].name.c_str());
            count++;
        }
    }
    std::printf("\n  %d matches\n", count);
}

int main(int argc, char* argv[])
{
    std::cout << "BS1SDK - BSM File Tool v0.5.0\n\n";

    if (argc < 3) {
        std::cout << "Package analysis:\n";
        std::cout << "  bsm_tool analyze <file>                    - Header + validation\n";
        std::cout << "  bsm_tool names <file>                      - Dump name table\n";
        std::cout << "  bsm_tool imports <file>                    - Dump import table\n";
        std::cout << "  bsm_tool exports <file>                    - Dump export table\n";
        std::cout << "  bsm_tool actors <file>                     - Non-structural exports\n";
        std::cout << "  bsm_tool props <file> <export_idx>         - Properties of an export\n";
        std::cout << "  bsm_tool spawners <file>                   - Spawner actors + locations\n";
        std::cout << "  bsm_tool dump <file>                       - Full dump\n";
        std::cout << "  bsm_tool hexdump <file> [off] [len]        - Hex dump\n";
        std::cout << "  bsm_tool compare <a> <b>                   - Compare two packages\n";
        std::cout << "\nSearch:\n";
        std::cout << "  bsm_tool findname <file> <pattern>         - Search name table\n";
        std::cout << "  bsm_tool findimport <file> <pattern>       - Search imports by name\n";
        std::cout << "  bsm_tool findexport <file> <pattern>       - Search exports by name\n";
        std::cout << "\nSpawn patching:\n";
        std::cout << "  bsm_tool patch <file> <mult> [dry]         - Duplicate spawners (x2-x10)\n";
        std::cout << "  bsm_tool setprop <file> <idx> <prop> <val> - Edit export property\n";
        std::cout << "\nCustom splicer types:\n";
        std::cout << "  bsm_tool aitypes <file> <export_idx>       - List RepopulationAITypes\n";
        std::cout << "  bsm_tool setaitype <file> <exp> <arr> <ref> - Replace one AI type entry\n";
        return 1;
    }

    std::string command = argv[1];
    
    if (command == "analyze") {
        AnalyzeFile(argv[2]);
    } else if (command == "compare" && argc >= 4) {
        CompareFiles(argv[2], argv[3]);
    } else if (command == "hexdump") {
        std::ifstream file(argv[2], std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            std::cerr << "Cannot open: " << argv[2] << "\n";
            return 1;
        }
        size_t fileSize = file.tellg();
        size_t offset = (argc > 3) ? std::stoull(argv[3], nullptr, 0) : 0;
        size_t length = (argc > 4) ? std::stoull(argv[4], nullptr, 0) : 512;
        
        file.seekg(offset);
        std::vector<uint8_t> data(std::min(length, fileSize - offset));
        file.read(reinterpret_cast<char*>(data.data()), data.size());
        
        HexDump(data.data(), data.size(), offset);
    } else if (command == "names") {
        DumpNames(argv[2]);
    } else if (command == "imports") {
        DumpImports(argv[2]);
    } else if (command == "exports") {
        DumpExports(argv[2]);
    } else if (command == "actors") {
        DumpActors(argv[2]);
    } else if (command == "props" && argc >= 4) {
        DumpProps(argv[2], std::stoi(argv[3]));
    } else if (command == "spawners") {
        DumpSpawners(argv[2]);
    } else if (command == "dump") {
        DumpFull(argv[2]);
    } else if (command == "patch" && argc >= 4) {
        int mult = std::stoi(argv[3]);
        bool dry = (argc >= 5 && std::string(argv[4]) == "dry");
        PatchSpawners(argv[2], mult, dry);
    } else if (command == "setprop" && argc >= 6) {
        SetExportProp(argv[2], std::stoi(argv[3]), argv[4], argv[5]);
    } else if (command == "aitypes" && argc >= 4) {
        DumpAITypes(argv[2], std::stoi(argv[3]));
    } else if (command == "setaitype" && argc >= 6) {
        SetAIType(argv[2], std::stoi(argv[3]), std::stoi(argv[4]), std::stoi(argv[5]));
    } else if (command == "findimport" && argc >= 4) {
        FindImportByName(argv[2], argv[3]);
    } else if (command == "findexport" && argc >= 4) {
        FindExportByName(argv[2], argv[3]);
    } else if (command == "findname" && argc >= 4) {
        FindNameInTable(argv[2], argv[3]);
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}
