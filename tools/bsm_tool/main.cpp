#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <iomanip>
#include <cstring>
#include <filesystem>

/// BSM Tool - Standalone .bsm file analyzer
/// 
/// Usage:
///   bsm_tool analyze <file.bsm>       - Analyze file structure
///   bsm_tool dump <file.bsm>          - Dump all tables
///   bsm_tool compare <a.bsm> <b.bsm> - Compare two files
///   bsm_tool hexdump <file.bsm> [offset] [length] - Hex dump
///
/// This tool works offline (no game running) to study .bsm format.

// Known Unreal Engine package magic
constexpr uint32_t UE_PACKAGE_MAGIC = 0x9E2A83C1;

struct RawHeader {
    uint32_t field[64]; // Read first 256 bytes as uint32 array for analysis
};

void HexDump(const uint8_t* data, size_t size, size_t baseOffset = 0)
{
    for (size_t i = 0; i < size; i += 16) {
        std::printf("%08zX: ", baseOffset + i);
        
        // Hex bytes
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < size)
                std::printf("%02X ", data[i + j]);
            else
                std::printf("   ");
            if (j == 7) std::printf(" ");
        }
        
        std::printf(" |");
        
        // ASCII
        for (size_t j = 0; j < 16 && (i + j) < size; ++j) {
            char c = static_cast<char>(data[i + j]);
            std::printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        
        std::printf("|\n");
    }
}

void AnalyzeFile(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open file: " << filepath << "\n";
        return;
    }

    size_t fileSize = file.tellg();
    file.seekg(0);

    std::cout << "═══════════════════════════════════════════════\n";
    std::cout << " BSM File Analysis: " << std::filesystem::path(filepath).filename().string() << "\n";
    std::cout << "═══════════════════════════════════════════════\n";
    std::cout << " File size: " << fileSize << " bytes (" 
              << (fileSize / 1024.0 / 1024.0) << " MB)\n\n";

    // Read header area
    std::vector<uint8_t> headerData(std::min(fileSize, size_t(1024)));
    file.read(reinterpret_cast<char*>(headerData.data()), headerData.size());

    // Check for Unreal package magic
    uint32_t magic = *reinterpret_cast<uint32_t*>(headerData.data());
    std::cout << " Magic: 0x" << std::hex << std::uppercase << magic << std::dec << "\n";
    
    if (magic == UE_PACKAGE_MAGIC) {
        std::cout << " → Standard Unreal Engine package signature detected!\n";
        
        // Parse as standard UE package header
        uint16_t fileVersion = *reinterpret_cast<uint16_t*>(headerData.data() + 4);
        uint16_t licenseeVer = *reinterpret_cast<uint16_t*>(headerData.data() + 6);
        uint32_t pkgFlags = *reinterpret_cast<uint32_t*>(headerData.data() + 8);
        
        std::cout << " File Version: " << fileVersion << "\n";
        std::cout << " Licensee Version: " << licenseeVer << "\n";
        std::cout << " Package Flags: 0x" << std::hex << pkgFlags << std::dec << "\n";
        
        // Name table
        int32_t nameCount = *reinterpret_cast<int32_t*>(headerData.data() + 12);
        int32_t nameOffset = *reinterpret_cast<int32_t*>(headerData.data() + 16);
        std::cout << "\n Name Table: " << nameCount << " entries at offset 0x" 
                  << std::hex << nameOffset << std::dec << "\n";
        
        // Export table
        int32_t exportCount = *reinterpret_cast<int32_t*>(headerData.data() + 20);
        int32_t exportOffset = *reinterpret_cast<int32_t*>(headerData.data() + 24);
        std::cout << " Export Table: " << exportCount << " entries at offset 0x" 
                  << std::hex << exportOffset << std::dec << "\n";
        
        // Import table
        int32_t importCount = *reinterpret_cast<int32_t*>(headerData.data() + 28);
        int32_t importOffset = *reinterpret_cast<int32_t*>(headerData.data() + 32);
        std::cout << " Import Table: " << importCount << " entries at offset 0x" 
                  << std::hex << importOffset << std::dec << "\n";
                  
        // Sanity checks
        std::cout << "\n Validation:\n";
        if (nameOffset > 0 && nameOffset < (int32_t)fileSize)
            std::cout << "   ✓ Name offset within file bounds\n";
        else
            std::cout << "   ✗ Name offset out of bounds!\n";
            
        if (exportOffset > 0 && exportOffset < (int32_t)fileSize)
            std::cout << "   ✓ Export offset within file bounds\n";
        else
            std::cout << "   ✗ Export offset out of bounds!\n";
            
        if (importOffset > 0 && importOffset < (int32_t)fileSize)
            std::cout << "   ✓ Import offset within file bounds\n";
        else
            std::cout << "   ✗ Import offset out of bounds!\n";
    } else {
        std::cout << " → Non-standard magic. Checking for known patterns...\n";
        
        // Check for compression signatures
        if (headerData[0] == 0x78 && (headerData[1] == 0x9C || headerData[1] == 0x01 || headerData[1] == 0xDA)) {
            std::cout << " → File appears to be zlib compressed!\n";
        } else if (headerData[0] == 0x1F && headerData[1] == 0x8B) {
            std::cout << " → File appears to be gzip compressed!\n";
        }
        
        // Look for UE magic deeper in the file (compressed package)
        file.seekg(0);
        std::vector<uint8_t> fullFile(std::min(fileSize, size_t(64 * 1024)));
        file.read(reinterpret_cast<char*>(fullFile.data()), fullFile.size());
        
        for (size_t i = 0; i < fullFile.size() - 4; ++i) {
            if (*reinterpret_cast<uint32_t*>(fullFile.data() + i) == UE_PACKAGE_MAGIC) {
                std::cout << " → Found UE magic at offset 0x" << std::hex << i << std::dec << "\n";
                std::cout << "   (File may have a custom header before standard package data)\n";
                break;
            }
        }
    }

    // Always show hex dump of first 256 bytes
    std::cout << "\n First 256 bytes:\n";
    std::cout << "───────────────────────────────────────────────\n";
    HexDump(headerData.data(), std::min(headerData.size(), size_t(256)));
    
    // Look for readable strings in header
    std::cout << "\n Readable strings in first 1KB:\n";
    std::cout << "───────────────────────────────────────────────\n";
    std::string current;
    for (size_t i = 0; i < headerData.size(); ++i) {
        char c = static_cast<char>(headerData[i]);
        if (c >= 32 && c < 127) {
            current += c;
        } else {
            if (current.size() >= 4) {
                std::cout << "   @0x" << std::hex << (i - current.size()) << std::dec 
                          << ": \"" << current << "\"\n";
            }
            current.clear();
        }
    }
    if (current.size() >= 4) {
        std::cout << "   \"" << current << "\"\n";
    }
}

void CompareFiles(const std::string& file1, const std::string& file2)
{
    std::ifstream f1(file1, std::ios::binary);
    std::ifstream f2(file2, std::ios::binary);
    
    if (!f1.is_open() || !f2.is_open()) {
        std::cerr << "Error: Cannot open one or both files\n";
        return;
    }

    // Read first 256 bytes of each
    uint8_t h1[256], h2[256];
    f1.read(reinterpret_cast<char*>(h1), 256);
    f2.read(reinterpret_cast<char*>(h2), 256);

    std::cout << "Comparing headers:\n";
    std::cout << "  File 1: " << file1 << "\n";
    std::cout << "  File 2: " << file2 << "\n\n";

    std::cout << "Offset  | File1    | File2    | Match\n";
    std::cout << "--------|----------|----------|------\n";

    for (int i = 0; i < 256; i += 4) {
        uint32_t v1 = *reinterpret_cast<uint32_t*>(h1 + i);
        uint32_t v2 = *reinterpret_cast<uint32_t*>(h2 + i);
        bool match = (v1 == v2);
        
        std::printf("0x%04X  | %08X | %08X | %s\n", i, v1, v2, match ? "YES" : "NO <<<");
    }
}

// ─── Name Table Dumper ─────────────────────────────────────────────────────

void DumpNames(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open: " << filepath << "\n";
        return;
    }

    size_t fileSize = file.tellg();
    file.seekg(0);

    // Read header
    uint8_t hdr[64];
    file.read(reinterpret_cast<char*>(hdr), 64);

    uint32_t magic = *reinterpret_cast<uint32_t*>(hdr);
    if (magic != UE_PACKAGE_MAGIC) {
        std::cerr << "Error: Not a valid UE package (bad magic)\n";
        return;
    }

    int32_t nameCount  = *reinterpret_cast<int32_t*>(hdr + 12);
    int32_t nameOffset = *reinterpret_cast<int32_t*>(hdr + 16);

    std::cout << "Dumping " << nameCount << " names from offset 0x"
              << std::hex << nameOffset << std::dec << "\n\n";

    // Read from name table offset to end of file (or a reasonable chunk)
    size_t readSize = std::min(fileSize - nameOffset, size_t(1024 * 1024));
    std::vector<uint8_t> nameData(readSize);
    file.seekg(nameOffset);
    file.read(reinterpret_cast<char*>(nameData.data()), readSize);

    // Parse name entries: uint8 length, wchar_t[length] (null-term), 6 flag bytes
    size_t pos = 0;
    for (int i = 0; i < nameCount && pos < readSize; ++i) {
        uint8_t strLen = nameData[pos++];
        if (strLen == 0 || pos + strLen * 2 > readSize) {
            std::printf("[%4d] <invalid at offset 0x%zX>\n", i, nameOffset + pos - 1);
            break;
        }

        // Read UTF-16LE string
        std::string name;
        for (int c = 0; c < strLen; ++c) {
            if (pos + 1 >= readSize) break;
            uint16_t wc = *reinterpret_cast<uint16_t*>(nameData.data() + pos);
            pos += 2;
            if (wc == 0) break;  // null terminator
            if (wc < 128) {
                name += static_cast<char>(wc);
            } else {
                // Non-ASCII: show as escape
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04X", wc);
                name += buf;
            }
        }

        // Skip null terminator if we didn't already consume it
        if (pos + 1 < readSize && *reinterpret_cast<uint16_t*>(nameData.data() + pos) == 0) {
            pos += 2;
        }

        // Read flag bytes (8 bytes per entry: confirmed from hex analysis)
        uint8_t flags[8] = {};
        size_t flagBytes = std::min(size_t(8), readSize - pos);
        memcpy(flags, nameData.data() + pos, flagBytes);
        pos += flagBytes;

        std::printf("[%4d] %-50s  flags: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                    i, name.c_str(), flags[0], flags[1], flags[2], flags[3], 
                    flags[4], flags[5], flags[6], flags[7]);
    }
}

// ─── Export Table Dumper ───────────────────────────────────────────────────

void DumpExports(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open: " << filepath << "\n";
        return;
    }

    size_t fileSize = file.tellg();
    file.seekg(0);

    uint8_t hdr[64];
    file.read(reinterpret_cast<char*>(hdr), 64);

    uint32_t magic = *reinterpret_cast<uint32_t*>(hdr);
    if (magic != UE_PACKAGE_MAGIC) {
        std::cerr << "Error: Not a valid UE package\n";
        return;
    }

    int32_t nameCount    = *reinterpret_cast<int32_t*>(hdr + 12);
    int32_t nameOffset   = *reinterpret_cast<int32_t*>(hdr + 16);
    int32_t exportCount  = *reinterpret_cast<int32_t*>(hdr + 20);
    int32_t exportOffset = *reinterpret_cast<int32_t*>(hdr + 24);

    // First, load all names into a vector for resolution
    size_t nameReadSize = std::min(fileSize - nameOffset, size_t(2 * 1024 * 1024));
    std::vector<uint8_t> nameData(nameReadSize);
    file.seekg(nameOffset);
    file.read(reinterpret_cast<char*>(nameData.data()), nameReadSize);

    std::vector<std::string> names;
    size_t pos = 0;
    for (int i = 0; i < nameCount && pos < nameReadSize; ++i) {
        uint8_t strLen = nameData[pos++];
        std::string name;
        for (int c = 0; c < strLen && pos + 1 < nameReadSize; ++c) {
            uint16_t wc = *reinterpret_cast<uint16_t*>(nameData.data() + pos);
            pos += 2;
            if (wc == 0) break;
            name += (wc < 128) ? static_cast<char>(wc) : '?';
        }
        if (pos + 1 < nameReadSize && *reinterpret_cast<uint16_t*>(nameData.data() + pos) == 0)
            pos += 2;
        pos += 8; // flags (8 bytes confirmed)
        names.push_back(name);
    }

    // Now read export table
    // UE2 export entry: ClassIndex(compact), SuperIndex(compact), Package(int32),
    //                   ObjectName(compact), ObjectFlags(uint32), SerialSize(compact),
    //                   SerialOffset(compact)
    // "Compact" = FCompactIndex (variable-length encoded integer)
    // For now, dump raw bytes and see the structure
    std::cout << "Export Table: " << exportCount << " entries at offset 0x"
              << std::hex << exportOffset << std::dec << "\n";
    std::cout << "Names loaded: " << names.size() << "\n\n";

    // Dump raw export table area
    size_t exportReadSize = std::min(fileSize - exportOffset, size_t(1024 * 1024));
    std::vector<uint8_t> exportData(exportReadSize);
    file.seekg(exportOffset);
    file.read(reinterpret_cast<char*>(exportData.data()), exportReadSize);

    std::cout << "First 256 bytes of export table:\n";
    HexDump(exportData.data(), std::min(exportReadSize, size_t(256)), exportOffset);
}

int main(int argc, char* argv[])
{
    std::cout << "BS1SDK - BSM File Tool v0.1.0\n\n";

    if (argc < 3) {
        std::cout << "Usage:\n";
        std::cout << "  bsm_tool analyze <file.bsm>   - Analyze header structure\n";
        std::cout << "  bsm_tool names <file>          - Dump all name table entries\n";
        std::cout << "  bsm_tool exports <file>        - Dump export table\n";
        std::cout << "  bsm_tool hexdump <file> [off] [len] - Hex dump\n";
        std::cout << "  bsm_tool compare <a> <b>       - Compare two file headers\n";
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
    } else if (command == "exports") {
        DumpExports(argv[2]);
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }

    return 0;
}
