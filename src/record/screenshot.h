#pragma once

#include <windows.h>
#include <shlobj.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace pm {

// Top-down BGRA pixels -> PNG (WIC). Caller's thread must have COM initialized.
bool save_png(const std::filesystem::path& path, const std::vector<uint8_t>& bgra, UINT width, UINT height);
bool copy_image_to_clipboard(HWND owner, const std::vector<uint8_t>& bgra, UINT width, UINT height);

// Known folder subdirectory, e.g. Pictures\PhoneMirror; created on demand.
std::filesystem::path media_folder(const KNOWNFOLDERID& folder);
// "PhoneMirror 2026-10-25 21.14.03" style stem for new files.
std::wstring timestamped_stem(const wchar_t* prefix);

}  // namespace pm
