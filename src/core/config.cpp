#include "config.h"
#include "log.h"

#include <fstream>
#include <sstream>
#include <algorithm>

namespace bs1sdk {

std::unordered_map<std::string, Config::Value> Config::s_Values;

static std::string Trim(const std::string& str)
{
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

bool Config::Load(const std::string& filepath)
{
    std::ifstream file(filepath);
    if (!file.is_open()) {
        LOG_WARN("Config file not found: {}", filepath);
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        
        // Skip comments and empty lines
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        // Skip section headers for now
        if (line[0] == '[') continue;

        auto eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;

        std::string key = Trim(line.substr(0, eqPos));
        std::string val = Trim(line.substr(eqPos + 1));

        // Try to parse as bool
        if (val == "true" || val == "false") {
            s_Values[key] = (val == "true");
        }
        // Try to parse as int
        else if (!val.empty() && (std::isdigit(val[0]) || val[0] == '-')) {
            if (val.find('.') != std::string::npos) {
                s_Values[key] = std::stod(val);
            } else {
                s_Values[key] = std::stoi(val);
            }
        }
        // String (strip quotes if present)
        else {
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }
            s_Values[key] = val;
        }
    }

    LOG_INFO("Loaded config: {} ({} values)", filepath, s_Values.size());
    return true;
}

bool Config::Save(const std::string& filepath)
{
    std::ofstream file(filepath);
    if (!file.is_open()) return false;

    file << "# BS1SDK Configuration\n\n";

    for (const auto& [key, value] : s_Values) {
        file << key << " = ";
        std::visit([&file](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                file << (v ? "true" : "false");
            } else if constexpr (std::is_same_v<T, std::string>) {
                file << "\"" << v << "\"";
            } else {
                file << v;
            }
        }, value);
        file << "\n";
    }

    return true;
}

std::optional<std::string> Config::GetString(const std::string& key)
{
    auto it = s_Values.find(key);
    if (it == s_Values.end()) return std::nullopt;
    if (auto* val = std::get_if<std::string>(&it->second)) return *val;
    return std::nullopt;
}

std::optional<int> Config::GetInt(const std::string& key)
{
    auto it = s_Values.find(key);
    if (it == s_Values.end()) return std::nullopt;
    if (auto* val = std::get_if<int>(&it->second)) return *val;
    return std::nullopt;
}

std::optional<bool> Config::GetBool(const std::string& key)
{
    auto it = s_Values.find(key);
    if (it == s_Values.end()) return std::nullopt;
    if (auto* val = std::get_if<bool>(&it->second)) return *val;
    return std::nullopt;
}

std::optional<double> Config::GetFloat(const std::string& key)
{
    auto it = s_Values.find(key);
    if (it == s_Values.end()) return std::nullopt;
    if (auto* val = std::get_if<double>(&it->second)) return *val;
    return std::nullopt;
}

void Config::Set(const std::string& key, Value value)
{
    s_Values[key] = std::move(value);
}

bool Config::Has(const std::string& key)
{
    return s_Values.contains(key);
}

} // namespace bs1sdk
