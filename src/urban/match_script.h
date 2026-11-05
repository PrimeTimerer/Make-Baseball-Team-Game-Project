#pragma once

#include <cstdint>
#include <filesystem>

class PS2Runtime;

// YKTK3 match scripts: forced results for chosen batters and pitchers in the 3D match.
//
// The match decides each pitch in one call at release: sub_29DB08 -> sub_29C8D8 -> sub_32AD60(realgame, play) ->
// sub_329F78. That call rolls swing, contact, hit or out, and the hit's kind and zone with the realgame object
// [0x614E88] ("rg"). A pitch not in play gets 1 strike, 2 ball, 6 strikeout or 7 walk from sub_3298D8. A ball in play
// gets its zone and detail from sub_32BB30 (rg+0x1618 = 4 hit / 5 out, rg+0x161C detail; HR = 0x15/0x1A/0x1F), which
// calls sub_32CF38 to set up the 3D play. The 3D animates that decision afterwards, and sub_29D5C0 applies it to the
// count and score (sub_2A0F68: box score).
//
// The hooks use the game's own sign override (rg+0x124 sign, +0x128 swing, +0x12C contact, +0x134), so the swing,
// the animation and the records all follow the forced result:
// - HR / HIT: swing and fair contact as a hit, then a center HR (zone 0x17, detail 0x1A) or a center single (zone 0x16,
//   detail 0x16).
// - K: swing and miss on every pitch (strike, then strikeout).
// - BB: take every pitch, called a ball (then a walk).
// Batter: rg+0xD4 points at the match copy (u16 global player ID at +0). Pitcher: the fielding team's lineup in the
// simgame match [0x5E783C], position 0.
// The same decision runs for 結果表示 / 簡易試合 and for the CPU clubs' games when the schedule moves on, so the hooks
// cover those too. A forced HR / hit / walk is capped per half-inning (an all-HR team would never make an out).
// Stacking multiplies the batter's at-bat values (rg+0xAC..0xC4) and the pitcher's pitch object values past 99.
// Settings: match_script.ini in the user folder (enabled=, steal=, limit=, rule=<target>,<bat|pit>,<hr|hit|k|bb>,
// boost=<target>,<stat>,<percent>), read again when it changes. Targets: own (the own club's players), opp (the
// opponents in the own club's games), all (everyone, every game), or a player's global ID.
namespace urban::matchscript
{
    void install(PS2Runtime &runtime, uint8_t *rdram, const std::filesystem::path &userDir);
}
