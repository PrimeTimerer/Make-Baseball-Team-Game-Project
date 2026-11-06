#pragma once

#include <cstdint>

// YKTK3 stadium and facility cheats: the "구장·시설" tab of the in-game menu (F2). The club object [0x5CA114] keeps the
// stadium at +0x13A0 ([level][weeks left][state] for the stadium 0-6, 6 = ドーム which also sets +0x1274, the turf 0-3 and
// the seats 0-4), the stadium facilities (counts by type at +0x13A9, what the 24 slots hold at +0x1420, which slot covers
// each of the 8 x 4 squares at +0x1438) and the practice facilities at +0x13D8 (11 x [level 0-3][weeks left][state]).
// The tab raises the stadium and the practice facilities to the top at once (the game gates them by year: the dome from
// year 10), places stadium facilities without cost and without the game's limits by type (by year) in the squares the
// stadium's level opens, and adds facilities beyond the squares (their effect only: the game sums count x effect by type).
namespace urban::stadium
{
    void install(uint8_t *rdram);
}
