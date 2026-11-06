#pragma once

#include <cstdint>
#include <filesystem>

class PS2Runtime;

// YKTK3 scouting: the "스카우트" tab of the in-game menu (F2).
//
// The game's discovery (Module 人事 → スケジュール設定 → 発掘): each day a scout spends at a domestic tournament or an
// overseas league, sub_00193570 (domestic) / sub_00193DE0 (overseas) go through that place's candidates and find each one
// whose roll sub_0015CEA0(100) is under its chance. The own team keeps a state per candidate in its scouting object
// ([team + 0x134]): +0x490 the 234 domestic ones (6 areas x 高校/大学/社会人 x 13), +0x6CF the 180 overseas ones
// (10 leagues x 18); 0 unknown, 2 memo, 3 found this week, 4 found. The candidates themselves: [0x5D1298] domestic and
// [0x5D129C] + 0xC overseas, 0x90-byte entries (id, name, age +0x15, the 12 clubs' interest +0x30 and evaluations +0x3E).
//
// The cheats: a set number of finds a scout and day (the roll is won while the day's count is under it); players on the
// wish list are found at once; protected players (the wish list, the ones the own team found) are cleared from the CPU
// clubs' interest so only the own team pursues them. Settings: scout_cheats.ini in the user folder (written by the tab).
namespace urban::scout
{
    void install(PS2Runtime &runtime, uint8_t *rdram, const std::filesystem::path &userDir);
}
