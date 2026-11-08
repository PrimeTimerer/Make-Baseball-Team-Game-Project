#pragma once

#include <cstdint>

class PS2Runtime;

// YKTK3 lineup lock: the "라인업" tab of the in-game menu (F2). The owner (11-08): read the lineup the game starts a match
// with, change it in the menu, and lock it so that a player brought up from the farm or a new signing does not shuffle the
// starting lineup again; the same for the rotation.
//
// sub_0029E7D8 sets a match up (both callers, sub_002A3BE0 and sub_00298FC0, build the two lineups with sub_002A4D80 first):
// a2 = the two lineups, 0x14 bytes a side of 10 pairs [match copy index (s8), position], a3 / t0 = the teams of side 0 / 1.
// It copies them to the match ([0x5E783C] +0x1C + side x 0x14, positions again at +0x6C + side x 10). The match teams
// ([0x5D1278] side 0, [0x5D1274] side 1) hold the 25 match copies at +0x2008 + 4 x index (+0 the player ID). Positions:
// 0 투 1 捕 2 一 3 二 4 三 5 遊 6 左 7 中 8 右 9 指, 15 none (a game without DH has the pitcher 9th and pair 9 empty).
// The hook (own team's games only) remembers the game's lineup for the tab and, while locked, writes the saved one (by
// player ID; a saved player not on the match roster leaves his row to one of the game's own batters) and the rotation's
// next starter into the pitcher's pair, before the game copies them.
namespace urban::lineup
{
    void install(PS2Runtime &runtime, uint8_t *rdram);
}
