#pragma once
// Which save the game plays, so that the cheats keep their settings per save (owner, 11-06: a new game or another save
// must not get the settings made for this one). The game's saves are memory card directories BISLPM-65853Y293_M<nn>
// (and _V<nn>); the id is "M00".."M99" / "V00".."V99", "new" for a new game not saved yet, "" before any game.

#include <cstdint>
#include <filesystem>
#include <string>

class PS2Runtime;

namespace urban::saveid
{
    void install(PS2Runtime &runtime, uint8_t *rdram, const std::filesystem::path &userDir);
    std::string current();
    uint32_t generation(); // changes whenever current() does
    // <userDir>/cheat_saves/<id>: the per-save settings files.
    std::filesystem::path folder(const std::string &id);
    // For the menus: "메모리카드 세이브 M00 (만든 때 2026-11-06 11:37)", "새 게임 (아직 저장 안 함)".
    std::string describe(const std::string &id);
}
