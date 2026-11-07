#pragma once

#include <cstdint>
#include <filesystem>

// YKTK3: the 99 limit of abilities in matches, the "99 제한" tab of the in-game menu (F2). With its switch on:
//  - the match copies take an ability's value byte when it is above 99 (player cheats' 198 / 255, the S+ codes), where the
//    game takes the value its experience reaches in the table at 0x5D1320 (99 at most): sub_00180238 when the match-copy
//    builders call it, and sub_0018C1E0 (the own club's values with condition) without its cut at 99 (255; the player's
//    limit byte still caps it, as in the game);
//  - the pitch object (sub_002B0748) keeps 球威, 制球, +0x6C and the pitch-type levels past 99 (255): the built-in code
//    variant recomp/code_variants/pitch_object_no_cap.pnach, whose words the tab writes into the game code while on.
// 球速 keeps its 99 (sub_002B1660 maps it to km/h through a table that ends there), and so does 体力 (the copies' stamina is
// the value times 100 on the game's 0-9900 scale; 255 made the own club change pitchers after every batter). Off: the game.
// Settings: ability_limits.ini in the user folder (written by the tab).
class PS2Runtime;

namespace urban::limits
{
    // Hooks, so after attach.
    void install(PS2Runtime &runtime, uint8_t *rdram, const std::filesystem::path &userDir);
}
