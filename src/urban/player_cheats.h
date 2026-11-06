#pragma once

#include <cstdint>
#include <filesystem>

// YKTK3 player cheats: the "선수 치트" tab of the in-game menu (F2), laid out like PROSPI 26 Studio's 대상 고르기. At every
// frame the 12 rosters are found (the player's own team by its class, after a new game or a loaded save alike) and the
// chosen items of player_cheat_items.txt (values by own-team record offset: kroon37's team S+, plus 球速) and the chosen
// skills (any player, WS, manager or coach skill; at most 8 a player) are written into the targeted players: the own team,
// chosen teams or every team (CPU records keep the ability block behind a pointer at +0x6C). A picked player keeps their own
// items and skills, found by their player ID wherever they are. Items taken off get the bytes they overwrote back (restore=1,
// kept from this run's first write). Chosen skills are added to the player's own; taken off
// again, the slots go back to what they held in this run. Settings: player_cheats.ini in the user folder (written by the tab).
class PS2Runtime;

namespace urban::players
{
    void install(uint8_t *rdram, const std::filesystem::path &userDir, const std::filesystem::path &exeDir);
    // The camp queue (special practice through the game's own judgment, beyond its 6 places): a hook, so after attach.
    void installCamp(PS2Runtime &runtime);
}
