#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <filesystem>
#include <algorithm>
#include <map>

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

// ─── Main ──────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    std::cout << "BS1SDK - INI Tool v1.0.0\n\n";

    if (argc < 3) {
        std::cout << "Usage:\n";
        std::cout << "  ini_tool dump <file>                       - Display all sections/keys\n";
        std::cout << "  ini_tool get <file> <section> <key>        - Get a specific value\n";
        std::cout << "  ini_tool set <file> <section> <key> <val>  - Set a value\n";
        std::cout << "  ini_tool diff <file_a> <file_b>            - Show differences\n";
        return 1;
    }

    std::string cmd = argv[1];

    if (cmd == "dump") {
        CmdDump(argv[2]);
    } else if (cmd == "get" && argc >= 5) {
        CmdGet(argv[2], argv[3], argv[4]);
    } else if (cmd == "set" && argc >= 6) {
        CmdSet(argv[2], argv[3], argv[4], argv[5]);
    } else if (cmd == "diff" && argc >= 4) {
        CmdDiff(argv[2], argv[3]);
    } else {
        std::cerr << "Unknown command or missing args: " << cmd << "\n";
        return 1;
    }

    return 0;
}
