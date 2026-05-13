#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <iomanip>
#include <cstring>
#include <filesystem>
#include <algorithm>
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

int main(int argc, char* argv[])
{
    std::cout << "BS1SDK - BSM File Tool v0.3.0\n\n";

    if (argc < 3) {
        std::cout << "Usage:\n";
        std::cout << "  bsm_tool analyze <file>            - Analyze header + validation\n";
        std::cout << "  bsm_tool names <file>              - Dump name table\n";
        std::cout << "  bsm_tool imports <file>            - Dump import table\n";
        std::cout << "  bsm_tool exports <file>            - Dump export table\n";
        std::cout << "  bsm_tool actors <file>             - Dump non-structural exports\n";
        std::cout << "  bsm_tool props <file> <export_idx>  - Dump properties of an export\n";
        std::cout << "  bsm_tool spawners <file>           - Find spawner actors with locations\n";
        std::cout << "  bsm_tool dump <file>               - Full dump (names+imports+exports)\n";
        std::cout << "  bsm_tool hexdump <file> [off] [len] - Hex dump\n";
        std::cout << "  bsm_tool compare <a> <b>           - Compare two packages\n";
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
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}
