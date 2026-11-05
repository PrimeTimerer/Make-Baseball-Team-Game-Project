#pragma once

#include <cstdint>
#include <filesystem>

// YKTK3 イチネンモード (one-year mode) cheats: the "이치넨" tab of the in-game menu (F2). The mode's skill-table manager
// ([0x62B580]) keeps the shop's lineup (+0x40, catalog numbers, count +0xC4) over a catalog of 332 items (+0x14, 16 bytes
// each: type, id, level, price) and the marks of what was bought (slot bits +0xBC/+0xC0; skills +0x102[id], conditions
// +0x16E[id]; ability items add their level to +0xCC[id], their effect). The tab shows the slots with the items' names
// (Korean from mods/기능별/한국어 이름.tsv, the game's own names next to them), changes a slot to any catalog item once or
// held (고정), makes slots buyable again (one, all, or at every frame), and sets or holds the points ([0x62B45C] + 8).
// Settings: ichinen_cheats.ini in the user folder (written by the tab).
namespace urban::ichinen
{
    void install(uint8_t *rdram, const std::filesystem::path &userDir);
}
