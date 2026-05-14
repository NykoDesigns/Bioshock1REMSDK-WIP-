#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <map>
#include <cstdint>
#include <cstring>

/// INI Tool — Parse, display, and patch BioShock INI configuration files
///
/// BioShock stores all gameplay balance data in INI files packed inside
/// ConfigINI.IBF. When loose INI files exist in ContentBaked/pc/System/,
/// the engine loads those instead — this is how we apply mods.
///
/// Supported files: Weapons.ini, Ai.ini, LootTables.ini, Spawning.ini,
///                  Difficulty.ini, Plasmids.ini
///
/// Usage:
///   ini_tool dump <file>                     - Display all sections/keys
///   ini_tool get <file> <section> <key>      - Get a specific value
///   ini_tool set <file> <section> <key> <val> - Set a value (writes file)
///   ini_tool diff <file_a> <file_b>          - Show differences

// ─── INI Entry ─────────────────────────────────────────────────────────

struct IniEntry {
    std::string section;
    std::string key;      // empty for comments/blank lines/section headers
    std::string value;
    std::string rawLine;  // preserved for round-trip writing
};

// ─── Parser ────────────────────────────────────────────────────────────

static std::vector<IniEntry> ParseIni(const std::string& filepath)
{
    std::vector<IniEntry> entries;
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open " << filepath << "\n";
        return entries;
    }

    std::string currentSection;
    std::string line;

    while (std::getline(file, line)) {
        std::string rawLine = line + "\n";
        std::string stripped = line;
        // Trim whitespace
        size_t start = stripped.find_first_not_of(" \t\r\n");
        size_t end = stripped.find_last_not_of(" \t\r\n");
        if (start != std::string::npos) stripped = stripped.substr(start, end - start + 1);
        else stripped.clear();

        // Blank line or comment
        if (stripped.empty() || stripped[0] == ';' || stripped[0] == '#') {
            entries.push_back({currentSection, "", "", rawLine});
            continue;
        }

        // Section header
        if (stripped[0] == '[' && stripped.back() == ']') {
            currentSection = stripped.substr(1, stripped.size() - 2);
            entries.push_back({currentSection, "", "", rawLine});
            continue;
        }

        // Key=Value
        size_t eq = stripped.find('=');
        if (eq != std::string::npos) {
            std::string k = stripped.substr(0, eq);
            std::string v = stripped.substr(eq + 1);
            // Trim key and value
            size_t ks = k.find_first_not_of(" \t");
            size_t ke = k.find_last_not_of(" \t");
            if (ks != std::string::npos) k = k.substr(ks, ke - ks + 1);
            size_t vs = v.find_first_not_of(" \t");
            size_t ve = v.find_last_not_of(" \t");
            if (vs != std::string::npos) v = v.substr(vs, ve - vs + 1);
            else v.clear();
            entries.push_back({currentSection, k, v, rawLine});
        } else {
            entries.push_back({currentSection, "", "", rawLine});
        }
    }

    return entries;
}

// ─── Writer ────────────────────────────────────────────────────────────

static void WriteIni(const std::string& filepath, const std::vector<IniEntry>& entries)
{
    std::ofstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot write to " << filepath << "\n";
        return;
    }
    for (auto& e : entries) {
        file << e.rawLine;
    }
}

// ─── Commands ──────────────────────────────────────────────────────────

static void CmdDump(const std::string& filepath)
{
    auto entries = ParseIni(filepath);
    if (entries.empty()) return;

    std::string lastSection;
    int sectionCount = 0, kvCount = 0;

    for (auto& e : entries) {
        if (e.section != lastSection && !e.key.empty()) {
            lastSection = e.section;
        }
        if (!e.key.empty()) {
            std::printf("  [%s] %s = %s\n", e.section.c_str(), e.key.c_str(), e.value.c_str());
            kvCount++;
        } else if (!e.section.empty() && e.rawLine.find('[') != std::string::npos) {
            sectionCount++;
        }
    }

    std::printf("\n  %d sections, %d key-value pairs\n", sectionCount, kvCount);
}

static void CmdGet(const std::string& filepath, const std::string& section, const std::string& key)
{
    auto entries = ParseIni(filepath);
    for (auto& e : entries) {
        if (e.section == section && e.key == key) {
            std::cout << e.value << "\n";
            return;
        }
    }
    std::cerr << "Not found: [" << section << "] " << key << "\n";
}

static void CmdSet(const std::string& filepath, const std::string& section,
                    const std::string& key, const std::string& newValue)
{
    auto entries = ParseIni(filepath);
    bool found = false;

    for (auto& e : entries) {
        if (e.section == section && e.key == key) {
            std::string oldValue = e.value;
            e.value = newValue;
            e.rawLine = key + "=" + newValue + "\n";
            found = true;
            std::cout << "  [" << section << "] " << key << ": " << oldValue << " -> " << newValue << "\n";
            break;
        }
    }

    if (!found) {
        std::cerr << "Not found: [" << section << "] " << key << "\n";
        return;
    }

    // Backup
    std::filesystem::path backupPath = std::filesystem::path(filepath).string() + ".bak";
    if (!std::filesystem::exists(backupPath)) {
        std::filesystem::copy_file(filepath, backupPath);
        std::cout << "  Backed up: " << backupPath.string() << "\n";
    }

    WriteIni(filepath, entries);
    std::cout << "  Written: " << filepath << "\n";
}

static void CmdDiff(const std::string& fileA, const std::string& fileB)
{
    auto entriesA = ParseIni(fileA);
    auto entriesB = ParseIni(fileB);

    // Build maps: section+key -> value
    std::map<std::string, std::string> mapA, mapB;
    for (auto& e : entriesA) {
        if (!e.key.empty()) mapA[e.section + "." + e.key] = e.value;
    }
    for (auto& e : entriesB) {
        if (!e.key.empty()) mapB[e.section + "." + e.key] = e.value;
    }

    int diffCount = 0;
    // Changed or removed in B
    for (auto& [k, vA] : mapA) {
        auto it = mapB.find(k);
        if (it == mapB.end()) {
            std::printf("  REMOVED: %s = %s\n", k.c_str(), vA.c_str());
            diffCount++;
        } else if (it->second != vA) {
            std::printf("  CHANGED: %s = %s -> %s\n", k.c_str(), vA.c_str(), it->second.c_str());
            diffCount++;
        }
    }
    // Added in B
    for (auto& [k, vB] : mapB) {
        if (mapA.find(k) == mapA.end()) {
            std::printf("  ADDED: %s = %s\n", k.c_str(), vB.c_str());
            diffCount++;
        }
    }

    if (diffCount == 0) std::cout << "  Files are identical.\n";
    else std::printf("  %d differences found.\n", diffCount);
}

// ─── IBF Archive Support ─────────────────────────────────────────────
// BioShock stores INI files in ConfigINI.IBF
// Format: repeating [uint8 name_char_count] [UTF-16LE name] [uint32 size] [content]

static void CmdExtractIBF(const std::string& ibfPath, const std::string& outputDir)
{
    std::ifstream file(ibfPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open " << ibfPath << "\n";
        return;
    }

    size_t fileSize = file.tellg();
    file.seekg(0);
    std::vector<uint8_t> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);

    std::filesystem::create_directories(outputDir);

    size_t offset = 0;
    int count = 0;

    while (offset < fileSize) {
        if (offset + 1 > fileSize) break;

        // Read filename length (char count including null terminator)
        uint8_t nameLen = data[offset++];
        if (nameLen == 0 || offset + (size_t)nameLen * 2 > fileSize) break;

        // Read UTF-16LE filename
        std::string filename;
        for (int i = 0; i < nameLen; i++) {
            uint16_t wc;
            std::memcpy(&wc, data.data() + offset + i * 2, 2);
            if (wc == 0) break;
            filename += (wc < 128) ? static_cast<char>(wc) : '?';
        }
        offset += (size_t)nameLen * 2;

        if (offset + 4 > fileSize) break;

        uint32_t contentSize;
        std::memcpy(&contentSize, data.data() + offset, 4);
        offset += 4;

        if (offset + contentSize > fileSize) {
            std::fprintf(stderr, "  WARNING: %s truncated\n", filename.c_str());
            contentSize = (uint32_t)(fileSize - offset);
        }

        std::string outPath = (std::filesystem::path(outputDir) / filename).string();
        std::ofstream out(outPath, std::ios::binary);
        out.write(reinterpret_cast<char*>(data.data() + offset), contentSize);
        out.close();

        std::printf("  Extracted: %-30s (%u bytes)\n", filename.c_str(), contentSize);
        offset += contentSize;
        count++;
    }

    std::printf("\n  Extracted %d files to %s\n", count, outputDir.c_str());
}

static void CmdRepackIBF(const std::string& inputDir, const std::string& ibfPath)
{
    std::vector<std::string> files;
    for (auto& entry : std::filesystem::directory_iterator(inputDir)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c){ return (char)std::tolower(c); });
            if (ext == ".ini")
                files.push_back(entry.path().filename().string());
        }
    }
    std::sort(files.begin(), files.end());

    // Backup original IBF if it exists
    if (std::filesystem::exists(ibfPath)) {
        std::string bakPath = ibfPath + ".bak";
        if (!std::filesystem::exists(bakPath)) {
            std::filesystem::copy_file(ibfPath, bakPath);
            std::printf("  Backed up: %s\n", bakPath.c_str());
        }
    }

    std::ofstream out(ibfPath, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "Error: Cannot write to " << ibfPath << "\n";
        return;
    }

    for (auto& filename : files) {
        std::string filepath = (std::filesystem::path(inputDir) / filename).string();
        std::ifstream in(filepath, std::ios::binary | std::ios::ate);
        if (!in.is_open()) continue;
        size_t contentSize = in.tellg();
        in.seekg(0);
        std::vector<uint8_t> content(contentSize);
        in.read(reinterpret_cast<char*>(content.data()), contentSize);

        // Name length including null terminator
        std::string nameWithNull = filename;
        nameWithNull.push_back('\0');
        uint8_t nameLen = (uint8_t)nameWithNull.size();
        out.write(reinterpret_cast<char*>(&nameLen), 1);

        // UTF-16LE filename
        for (char c : nameWithNull) {
            uint16_t wc = (uint16_t)(unsigned char)c;
            out.write(reinterpret_cast<char*>(&wc), 2);
        }

        // Content size + content
        uint32_t cs = (uint32_t)contentSize;
        out.write(reinterpret_cast<char*>(&cs), 4);
        out.write(reinterpret_cast<char*>(content.data()), contentSize);

        std::printf("  Packed: %-30s (%zu bytes)\n", filename.c_str(), contentSize);
    }

    out.close();
    std::printf("\n  Repacked %zu files to %s\n", files.size(), ibfPath.c_str());
}

// ─── Additional INI Commands ─────────────────────────────────────────

static void CmdSections(const std::string& filepath)
{
    auto entries = ParseIni(filepath);
    std::string lastSection;
    int sectionCount = 0;

    for (auto& e : entries) {
        if (!e.section.empty() && e.section != lastSection) {
            lastSection = e.section;
            int keys = 0;
            for (auto& e2 : entries)
                if (e2.section == lastSection && !e2.key.empty()) keys++;
            std::printf("  [%s]  (%d keys)\n", lastSection.c_str(), keys);
            sectionCount++;
        }
    }
    std::printf("\n  %d sections\n", sectionCount);
}

static void CmdSearch(const std::string& filepath, const std::string& pattern)
{
    auto entries = ParseIni(filepath);
    int matchCount = 0;

    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        return s;
    };
    std::string lp = toLower(pattern);

    for (auto& e : entries) {
        if (e.key.empty()) continue;
        if (toLower(e.key).find(lp) != std::string::npos ||
            toLower(e.value).find(lp) != std::string::npos ||
            toLower(e.section).find(lp) != std::string::npos) {
            std::printf("  [%s] %s = %s\n", e.section.c_str(), e.key.c_str(), e.value.c_str());
            matchCount++;
        }
    }
    std::printf("\n  %d matches\n", matchCount);
}

static void CmdAdd(const std::string& filepath, const std::string& section,
                    const std::string& key, const std::string& value)
{
    auto entries = ParseIni(filepath);

    // Find last entry in the target section
    int insertIdx = -1;
    for (int i = 0; i < (int)entries.size(); i++) {
        if (entries[i].section == section) insertIdx = i;
    }

    if (insertIdx < 0) {
        // Section doesn't exist — create it
        entries.push_back({section, "", "", "\n[" + section + "]\n"});
        entries.push_back({section, key, value, key + "=" + value + "\n"});
    } else {
        IniEntry ne{section, key, value, key + "=" + value + "\n"};
        entries.insert(entries.begin() + insertIdx + 1, ne);
    }

    // Backup
    std::filesystem::path bakPath = std::filesystem::path(filepath).string() + ".bak";
    if (!std::filesystem::exists(bakPath))
        std::filesystem::copy_file(filepath, bakPath);

    WriteIni(filepath, entries);
    std::printf("  Added: [%s] %s = %s\n", section.c_str(), key.c_str(), value.c_str());
    std::printf("  Written: %s\n", filepath.c_str());
}

// ─── Main ──────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    std::cout << "BS1SDK - INI Tool v2.0.0\n\n";

    if (argc < 3) {
        std::cout << "INI commands:\n";
        std::cout << "  ini_tool dump <file>                       - Display all sections/keys\n";
        std::cout << "  ini_tool sections <file>                   - List sections with key counts\n";
        std::cout << "  ini_tool search <file> <pattern>            - Search keys/values/sections\n";
        std::cout << "  ini_tool get <file> <section> <key>        - Get a specific value\n";
        std::cout << "  ini_tool set <file> <section> <key> <val>  - Set a value\n";
        std::cout << "  ini_tool add <file> <section> <key> <val>  - Add a new key-value pair\n";
        std::cout << "  ini_tool diff <file_a> <file_b>            - Show differences\n";
        std::cout << "\nIBF archive commands:\n";
        std::cout << "  ini_tool extract <ibf_file> [output_dir]   - Extract INI files from IBF\n";
        std::cout << "  ini_tool repack <input_dir> <output_ibf>   - Repack INI files into IBF\n";
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "dump") {
        CmdDump(argv[2]);
    } else if (cmd == "sections") {
        CmdSections(argv[2]);
    } else if (cmd == "search" && argc >= 4) {
        CmdSearch(argv[2], argv[3]);
    } else if (cmd == "get" && argc >= 5) {
        CmdGet(argv[2], argv[3], argv[4]);
    } else if (cmd == "set" && argc >= 6) {
        CmdSet(argv[2], argv[3], argv[4], argv[5]);
    } else if (cmd == "add" && argc >= 6) {
        CmdAdd(argv[2], argv[3], argv[4], argv[5]);
    } else if (cmd == "diff" && argc >= 4) {
        CmdDiff(argv[2], argv[3]);
    } else if (cmd == "extract") {
        std::string outDir = (argc >= 4) ? argv[3] : "extracted_ini";
        CmdExtractIBF(argv[2], outDir);
    } else if (cmd == "repack" && argc >= 4) {
        CmdRepackIBF(argv[2], argv[3]);
    } else {
        std::cerr << "Unknown command or missing args: " << cmd << "\n";
        return 1;
    }

    return 0;
}
