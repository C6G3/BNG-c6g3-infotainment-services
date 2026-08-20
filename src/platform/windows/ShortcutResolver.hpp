#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ifs {

/// Resolves Windows Shell shortcut (.lnk) files to their target paths.
/// Safe to call across threads. Initializes and uninitializes COM per call.
class ShortcutResolver {
public:
    /// Checks if a file path is a Windows shortcut (.lnk).
    static bool isShortcut(const std::filesystem::path& path);

    /// Resolves the .lnk file to its target path (file or directory).
    /// Returns std::nullopt if the file is not a valid shortcut or resolution fails.
    static std::optional<std::filesystem::path> resolve(const std::filesystem::path& shortcutPath);
};

} // namespace ifs
