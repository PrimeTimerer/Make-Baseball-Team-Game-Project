#pragma once

#include <filesystem>

// The in-game menu's status line above the tabs (owner, 11-08: "확인에 대한 불안"): which save the cheat settings belong
// to, and at its right a message for a few seconds whenever a tab saved (and so applied) its settings: the per-save files
// in cheat_saves/<save>/ and config.ini (the 치트 tab's files and values), watched by their write times.
namespace urban::status
{
    void install(const std::filesystem::path &userDir);
}
