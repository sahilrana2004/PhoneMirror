#pragma once

#include <filesystem>
#include <map>
#include <string>

namespace pm {

// Persistent settings: a flat JSON object (string / number / bool values) in
// %APPDATA%\PhoneMirror\settings.json. Unknown keys are preserved; missing keys take the defaults below.
class Settings {
public:
    static Settings& instance();

    void load();
    void save() const;

    std::string get_string(const std::string& key, const std::string& fallback) const;
    double get_number(const std::string& key, double fallback) const;
    bool get_bool(const std::string& key, bool fallback) const;

    void set_string(const std::string& key, const std::string& value);
    void set_number(const std::string& key, double value);
    void set_bool(const std::string& key, bool value);

    static std::filesystem::path data_dir();  // %APPDATA%\PhoneMirror (created on demand)

private:
    Settings() = default;

    // Values are stored as their JSON text ("\"str\"", "1.5", "true").
    std::map<std::string, std::string> values_;
};

}  // namespace pm
