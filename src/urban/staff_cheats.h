#pragma once

#include <cstdint>
#include <filesystem>

// YKTK3 staff cheats: the "스태프 치트" tab of the in-game menu (F2). For the own team (whichever it is) or any team: the
// manager, the pitching, fielding and training coaches and the two scouts, each with the coaching abilities the game shows
// (D..S), the manager's personality and the skills (manager and coaches: 8 slots at +0x1C; scouts: 6 slots at +0x1A in the
// scout skill IDs). An entry is written once ("한 번 적용") or held at every frame ("고정"); "원래대로" puts back what the
// record held before the first write of this run. Settings: staff_cheats.ini in the user folder (written by the tab).
// The tab also holds the off-season retirement screen's switches (引退勧告 always accepted, 引退引止め always works, any
// retiring player can become a coach): hooks on that screen's three judgements (installHooks).
class PS2Runtime;

namespace urban::staff
{
    void install(uint8_t *rdram, const std::filesystem::path &userDir);
    // The retirement screen's judgements: hooks, so after attach (and after install).
    void installHooks(PS2Runtime &runtime);
}
