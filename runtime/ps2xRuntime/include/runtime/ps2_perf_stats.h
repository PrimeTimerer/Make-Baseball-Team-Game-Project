#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Cheap counters for a port's always-on performance log (YKTK3: src/urban/perf_log.cpp): VU1 time by kind, the VU1
// images that run interpreted (no ahead-of-time compiled code), and a one-shot GS snapshot.
namespace ps2x::perf
{
    struct Vu1Times
    {
        uint64_t interpretedNs = 0u;
        uint64_t compiledNs = 0u;
    };

    // VU1 microprogram time since the previous call, interpreted and compiled.
    Vu1Times takeVu1Times() noexcept;

    // Every VU1 image that runs interpreted is saved once into `directory` as vu1_<hash>.bin (the input of
    // tools/vu1_generate.py --capture <directory> --record). An empty path saves nothing.
    void setMissingVu1Directory(const std::filesystem::path &directory);

    // Hashes of the VU1 images first seen running interpreted since the previous call.
    std::vector<uint64_t> takeNewMissingVu1();

    // The GS image of the next present, written as a BMP (PCSX2's GS only).
    void requestSnapshot(const std::string &path);
}
