#pragma once

#include <filesystem>

namespace bd {

// Initialize native file I/O hooks (CreateFileA, ReadFile, etc.).
// Must be called before game module launch so the hooks are active.
void InitFileHooks(const std::filesystem::path& game_dir);

// Returns the user content root directory for saves and profile data.
// Windows: Documents/reblue   Linux: ~/.local/share/reblue
std::filesystem::path GetUserContentRoot();

// Cache guest membase for game patch hooks
void InitGamePatches();

}  // namespace bd
