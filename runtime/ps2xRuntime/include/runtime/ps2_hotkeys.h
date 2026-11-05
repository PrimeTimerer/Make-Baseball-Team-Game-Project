#pragma once

#include <string>

// The game window's keys: F1 help, F2 menu, F3 FPS, F4 cheats, F5 / F9 quick save / load, F7 200% / F8 turbo, F11 full
// screen; developer keys need Ctrl (Ctrl+F6 profile capture, Ctrl+F7 its marks, Ctrl+F8 debug dump, Ctrl+F1 debug
// panel). F6, F10 and F12 stay free.
namespace ps2x::hotkeys
{
    // The key list F1 shows at the top left (UTF-8, drawn by PCSX2's GS); call before the game loop starts.
    void setHelpText(std::string text);
}
