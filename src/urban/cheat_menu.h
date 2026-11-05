#pragma once

#include <cstdint>
#include <filesystem>

// YKTK3 cheats one by one: the "치트" tab of the in-game menu (F2). tools/cheat_groups.py splits the .pnach files of
// the mods folder into one file per cheat function (mods/기능별, listed in mods/기능별/목록.tsv with their category and
// Korean name); the tab shows them by category with a check box each. A check box adds or removes the file in
// config.ini [Mods] Enabled (written in the same code page as the launcher writes it), and the .pnach watcher applies
// the new list within a second: an enabled cheat runs, a disabled one gets the game's own code back. Whole files that
// are enabled (the launcher's list) are shown too, so one can be turned off when its functions are used instead.
// A cheat with a number (mods/기능별/값.tsv: ticket price, idea memo, money...) gets buttons to change it, and a data
// write gets 고정 (held at every frame) or once; they go to config.ini [CheatValues] (see urban::mods::pnachFiles).
namespace urban::cheatmenu
{
    void install(const std::filesystem::path &configPath, uint8_t *rdram);
}
