#include "app/settings.h"

#include <windows.h>
#include <shlobj.h>

#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "app/log.h"

namespace pm {
namespace {

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

std::string unquote(const std::string& json) {
    if (json.size() < 2 || json.front() != '"') return json;
    std::string out;
    for (size_t i = 1; i + 1 < json.size(); ++i) {
        char c = json[i];
        if (c != '\\' || i + 2 >= json.size()) {
            out += c;
            continue;
        }
        char e = json[++i];
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'u':
            if (i + 4 < json.size()) {
                out += static_cast<char>(std::stoi(json.substr(i + 1, 4), nullptr, 16));
                i += 4;
            }
            break;
        default: out += e;
        }
    }
    return out;
}

// Minimal reader for a flat object; nested values are skipped as raw text.
class Reader {
public:
    explicit Reader(std::string text) : s_(std::move(text)) {}

    bool parse(std::map<std::string, std::string>& out) {
        ws();
        if (!eat('{')) return false;
        ws();
        if (eat('}')) return true;
        for (;;) {
            ws();
            std::string key = string_token();
            if (key.empty()) return false;
            ws();
            if (!eat(':')) return false;
            ws();
            std::string value = value_token();
            if (value.empty()) return false;
            out[unquote(key)] = value;
            ws();
            if (eat(',')) continue;
            return eat('}');
        }
    }

private:
    void ws() {
        while (i_ < s_.size() && isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool eat(char c) {
        if (i_ < s_.size() && s_[i_] == c) {
            ++i_;
            return true;
        }
        return false;
    }
    std::string string_token() {
        if (i_ >= s_.size() || s_[i_] != '"') return {};
        size_t start = i_++;
        while (i_ < s_.size() && s_[i_] != '"') i_ += s_[i_] == '\\' ? 2 : 1;
        if (i_ >= s_.size()) return {};
        ++i_;
        return s_.substr(start, i_ - start);
    }
    std::string value_token() {
        if (i_ >= s_.size()) return {};
        if (s_[i_] == '"') return string_token();
        if (s_[i_] == '{' || s_[i_] == '[') {
            size_t start = i_;
            int depth = 0;
            bool in_string = false;
            for (; i_ < s_.size(); ++i_) {
                char c = s_[i_];
                if (in_string) {
                    if (c == '\\') ++i_;
                    else if (c == '"') in_string = false;
                } else if (c == '"') {
                    in_string = true;
                } else if (c == '{' || c == '[') {
                    ++depth;
                } else if ((c == '}' || c == ']') && --depth == 0) {
                    ++i_;
                    return s_.substr(start, i_ - start);
                }
            }
            return {};
        }
        size_t start = i_;
        while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && !isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
        return s_.substr(start, i_ - start);
    }

    std::string s_;
    size_t i_ = 0;
};

std::filesystem::path settings_path() { return Settings::data_dir() / L"settings.json"; }

}  // namespace

Settings& Settings::instance() {
    static Settings settings;
    return settings;
}

std::filesystem::path Settings::data_dir() {
    PWSTR appdata = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
        dir = std::filesystem::path(appdata) / L"PhoneMirror";
    }
    CoTaskMemFree(appdata);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void Settings::load() {
    std::ifstream in(settings_path(), std::ios::binary);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    std::map<std::string, std::string> parsed;
    if (Reader(ss.str()).parse(parsed)) {
        values_ = std::move(parsed);
    } else {
        log::error("settings.json is not a valid JSON object; using defaults");
    }
}

void Settings::save() const {
    std::filesystem::path path = settings_path();
    std::filesystem::path tmp = path;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << "{\n";
        size_t n = 0;
        for (const auto& [key, value] : values_) {
            out << "  " << quote(key) << ": " << value << (++n < values_.size() ? ",\n" : "\n");
        }
        out << "}\n";
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::string Settings::get_string(const std::string& key, const std::string& fallback) const {
    auto it = values_.find(key);
    return it != values_.end() && !it->second.empty() && it->second.front() == '"' ? unquote(it->second) : fallback;
}

double Settings::get_number(const std::string& key, double fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    double v = fallback;
    auto [ptr, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
    return ec == std::errc() ? v : fallback;
}

bool Settings::get_bool(const std::string& key, bool fallback) const {
    auto it = values_.find(key);
    if (it == values_.end()) return fallback;
    if (it->second == "true") return true;
    if (it->second == "false") return false;
    return fallback;
}

void Settings::set_string(const std::string& key, const std::string& value) { values_[key] = quote(value); }

void Settings::set_number(const std::string& key, double value) {
    char buf[32];
    auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
    values_[key] = std::string(buf, ptr);
}

void Settings::set_bool(const std::string& key, bool value) { values_[key] = value ? "true" : "false"; }

}  // namespace pm
