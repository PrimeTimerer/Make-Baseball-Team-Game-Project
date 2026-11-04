#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace urban
{
    // Pro Yakyuu Team wo Tsukurou! 3 (Japan), SLPM-65853.
    inline constexpr const char *kSerial = "SLPM-65853";
    inline constexpr const char *kBootElfPath = "SLPM_658.53";
    inline constexpr const char *kBootElfSha256 = "3697202792fa559312ae1e0f89dd91514d99034cd754f8a608ec50dbee3fd4e0";

    struct DiscCheckResult
    {
        bool ok = false;
        std::string error;
        std::filesystem::path bootElfPath;
    };

    std::filesystem::path findDiscImage(const std::vector<std::filesystem::path> &searchRoots);
    DiscCheckResult verifyDiscAndPrepare(const std::filesystem::path &image, const std::filesystem::path &cacheDir);
}
