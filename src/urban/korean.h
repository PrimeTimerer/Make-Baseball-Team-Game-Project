#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

// YKTK3 Korean translation, the part that is not on the disc: RAM_PATCH.TXT of the korean folder lists names the game
// keeps in memory (hometown objects in the executable, the plan table it loads), with their Korean bytes. They are
// written over the game's memory and kept there (the executable's data comes back with every save state loaded).
// tools/korean/kobuild.py makes the file; the disc part of the folder is read by PS2CdImage's overlay.
namespace urban::korean
{
    void install(uint8_t *rdram, const std::filesystem::path &folder);

    // The original (Japanese, Shift-JIS) bytes of a name the patch replaced at this game address, or "" when none:
    // for tools that list the game's own names (the cheat menu's plan list for the launcher).
    std::string original(uint32_t address);

    // `text` points into game memory: the original Japanese string (NUL-terminated) when the patch replaced the string
    // that starts there, otherwise `text` itself. The cheat menu reads the game's names through this and keeps showing
    // them as before.
    const char *japanese(const char *text);
}
