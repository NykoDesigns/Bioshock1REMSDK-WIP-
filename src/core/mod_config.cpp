#include "mod_config.h"
#include "log.h"

#include <Windows.h>
#include <fstream>
#include <sstream>
#include <cstring>

namespace bs1sdk {

// ─── Minimal JSON helpers (no external dependency) ─────────────────────
// Only supports flat key-value pairs (bool, int, float, string).

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\"");
    size_t b = s.find_last_not_of(" \t\r\n\"");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

static std::string GetJsonValue(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos++;
    // Skip whitespace
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    // Find value end (comma, }, or newline)
    size_t end = json.find_first_of(",}\n", pos);
    if (end == std::string::npos) end = json.size();
    return Trim(json.substr(pos, end - pos));
}

static bool ToBool(const std::string& v, bool def) {
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    return def;
}

static int ToInt(const std::string& v, int def) {
    if (v.empty()) return def;
    try { return std::stoi(v); } catch (...) { return def; }
}

static float ToFloat(const std::string& v, float def) {
    if (v.empty()) return def;
    try { return std::stof(v); } catch (...) { return def; }
}

// ─── GetModDirectory ───────────────────────────────────────────────────

std::string GetModDirectory()
{
    char path[MAX_PATH];
    HMODULE hm = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&GetModDirectory, &hm);
    GetModuleFileNameA(hm, path, MAX_PATH);
    std::string dir(path);
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    return dir;
}

// ─── Load / Save ───────────────────────────────────────────────────────

static std::string ConfigPath() {
    return GetModDirectory() + "\\mod_config.json";
}

ModConfig LoadModConfig()
{
    ModConfig cfg;
    std::string path = ConfigPath();

    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_INFO("[Config] No mod_config.json found — using defaults");
        return cfg;
    }

    std::stringstream buf;
    buf << file.rdbuf();
    std::string json = buf.str();

    cfg.decoyTeleport      = ToBool(GetJsonValue(json, "decoyTeleport"), cfg.decoyTeleport);
    cfg.friendlyBots       = ToBool(GetJsonValue(json, "friendlyBots"), cfg.friendlyBots);
    cfg.friendlyBotLimit   = ToInt(GetJsonValue(json, "friendlyBotLimit"), cfg.friendlyBotLimit);
    cfg.rivetPistol        = ToBool(GetJsonValue(json, "rivetPistol"), cfg.rivetPistol);
    cfg.splicerFactions    = ToBool(GetJsonValue(json, "splicerFactions"), cfg.splicerFactions);
    cfg.chainLightning     = ToBool(GetJsonValue(json, "chainLightning"), cfg.chainLightning);
    cfg.chainRadius        = ToFloat(GetJsonValue(json, "chainRadius"), cfg.chainRadius);
    cfg.chainMaxJumps      = ToInt(GetJsonValue(json, "chainMaxJumps"), cfg.chainMaxJumps);
    cfg.chainDamageFalloff = ToFloat(GetJsonValue(json, "chainDamageFalloff"), cfg.chainDamageFalloff);
    cfg.autoInitMods       = ToBool(GetJsonValue(json, "autoInitMods"), cfg.autoInitMods);
    cfg.showOverlay        = ToBool(GetJsonValue(json, "showOverlay"), cfg.showOverlay);

    LOG_INFO("[Config] Loaded mod_config.json");
    return cfg;
}

void SaveModConfig(const ModConfig& cfg)
{
    std::string path = ConfigPath();
    std::ofstream file(path);
    if (!file.is_open()) {
        LOG_ERROR("[Config] Cannot write {}", path);
        return;
    }

    file << "{\n";
    file << "  \"_comment\": \"BS1SDK Mod Configuration — edit values and restart game\",\n\n";
    file << "  \"autoInitMods\": "       << (cfg.autoInitMods ? "true" : "false") << ",\n";
    file << "  \"showOverlay\": "        << (cfg.showOverlay ? "true" : "false") << ",\n\n";
    file << "  \"decoyTeleport\": "      << (cfg.decoyTeleport ? "true" : "false") << ",\n\n";
    file << "  \"friendlyBots\": "       << (cfg.friendlyBots ? "true" : "false") << ",\n";
    file << "  \"friendlyBotLimit\": "   << cfg.friendlyBotLimit << ",\n\n";
    file << "  \"rivetPistol\": "        << (cfg.rivetPistol ? "true" : "false") << ",\n\n";
    file << "  \"splicerFactions\": "    << (cfg.splicerFactions ? "true" : "false") << ",\n\n";
    file << "  \"chainLightning\": "     << (cfg.chainLightning ? "true" : "false") << ",\n";
    file << "  \"chainRadius\": "        << cfg.chainRadius << ",\n";
    file << "  \"chainMaxJumps\": "      << cfg.chainMaxJumps << ",\n";
    file << "  \"chainDamageFalloff\": " << cfg.chainDamageFalloff << "\n";
    file << "}\n";

    LOG_INFO("[Config] Saved mod_config.json");
}

} // namespace bs1sdk
