#pragma once

#include <string>
#include <unordered_map>
#include <variant>
#include <optional>

namespace bs1sdk {

/// Simple TOML-like configuration system.
/// For MVP, uses a simplified key=value parser.
/// Will migrate to toml++ once external deps are integrated.
class Config {
public:
    using Value = std::variant<bool, int, double, std::string>;

    /// Load config from file
    static bool Load(const std::string& filepath);

    /// Save config to file
    static bool Save(const std::string& filepath);

    /// Get a string value
    static std::optional<std::string> GetString(const std::string& key);

    /// Get an integer value
    static std::optional<int> GetInt(const std::string& key);

    /// Get a boolean value
    static std::optional<bool> GetBool(const std::string& key);

    /// Get a float value
    static std::optional<double> GetFloat(const std::string& key);

    /// Set a value
    static void Set(const std::string& key, Value value);

    /// Check if a key exists
    static bool Has(const std::string& key);

private:
    static std::unordered_map<std::string, Value> s_Values;
};

} // namespace bs1sdk
