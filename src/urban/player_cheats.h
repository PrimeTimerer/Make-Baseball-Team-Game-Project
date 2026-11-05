#pragma once

#include <cstdint>
#include <filesystem>

// YKTK3 player cheats: the "선수 치트" tab of the in-game menu (F2). At every frame the player's own team is found (the
// roster whose class is the user team's, after a new game or a loaded save alike) and the chosen items of
// player_cheat_items.txt (values by player record offset: kroon37's team S+) are written into its players. A pinned
// player keeps their own items, found by their player ID wherever they are on the roster. Settings: player_cheats.ini
// in the user folder (written by the tab).
namespace urban::players
{
    void install(uint8_t *rdram, const std::filesystem::path &userDir, const std::filesystem::path &exeDir);
}
