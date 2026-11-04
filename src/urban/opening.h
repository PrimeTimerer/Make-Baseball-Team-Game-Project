#pragma once

// YKTK3's opening movie (OVL/OPMV.BIN) is code the game loads at run time, outside the recompiled ELF: its scene is
// answered here as if the movie had ended, and the game goes on to the title screen.
namespace urban::opening
{
    void install();
}
