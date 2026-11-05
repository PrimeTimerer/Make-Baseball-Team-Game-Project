#pragma once

#include <cstdint>
#include <filesystem>

// YKTK3 performance log, on while the game runs: logs/perf-<time>.txt gets one line per second with the in-game date,
// the speed reached against the chosen one (100%, 200%), the worst frame and the share of the second VU1 spent
// interpreted / compiled. A slow stretch gets one screenshot (logs/perf-<time>/slow-<n>.bmp), and every VU1 program
// that runs interpreted is saved to logs/perf-<time>/vu1 (tools/vu1_generate.py --capture <that folder> --record
// compiles them). PS2X_PERF_LOG=0 turns it off.
namespace urban::perflog
{
    void install(uint8_t *rdram, const std::filesystem::path &userDir);
}
