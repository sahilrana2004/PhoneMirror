#pragma once

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace pm::log {

void init(const std::filesystem::path& dir);  // writes <dir>/phonemirror.log
void write(std::string_view level, std::string_view message);

std::string utf8(std::wstring_view text);

template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    write("INFO", std::format(fmt, std::forward<Args>(args)...));
}

template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    write("ERROR", std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace pm::log
