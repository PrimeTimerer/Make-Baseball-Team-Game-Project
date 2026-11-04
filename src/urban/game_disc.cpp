#include "urban/game_disc.h"
#include "urban/log.h"
#include "urban/sha256.h"
#include "runtime/ps2_cd_image.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace urban
{
    namespace
    {
        bool readWholeFile(const PS2CdImage &image, std::string_view ps2Path, std::vector<uint8_t> &out)
        {
            PS2CdImage::Entry entry{};
            if (!image.find(ps2Path, entry) || entry.directory)
            {
                return false;
            }
            out.resize(static_cast<size_t>(entry.size));
            return image.read(static_cast<uint64_t>(entry.lba) * PS2CdImage::kSectorSize, out.data(), out.size());
        }

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // Any ISO whose SYSTEM.CNF boots our ELF counts; the boot ELF hash is checked afterwards.
        bool bootsGame(const std::filesystem::path &path)
        {
            // PS2CdImage is a singleton; verifyDiscAndPrepare reopens the chosen image afterwards.
            PS2CdImage &probe = PS2CdImage::instance();
            if (!probe.open(path))
            {
                return false;
            }
            std::vector<uint8_t> cnf;
            if (!readWholeFile(probe, "SYSTEM.CNF", cnf))
            {
                return false;
            }
            return std::string(cnf.begin(), cnf.end()).find(kBootElfPath) != std::string::npos;
        }
    }

    std::filesystem::path findDiscImage(const std::vector<std::filesystem::path> &searchRoots)
    {
        for (const auto &root : searchRoots)
        {
            std::error_code error;
            if (!std::filesystem::is_directory(root, error))
            {
                continue;
            }
            for (const auto &entry : std::filesystem::directory_iterator(root, error))
            {
                if (entry.is_regular_file(error) && lower(entry.path().extension().string()) == ".iso" &&
                    bootsGame(entry.path()))
                {
                    return entry.path();
                }
            }
        }
        return {};
    }

    DiscCheckResult verifyDiscAndPrepare(const std::filesystem::path &imagePath, const std::filesystem::path &cacheDir)
    {
        DiscCheckResult result;
        PS2CdImage &image = PS2CdImage::instance();
        if (!image.open(imagePath))
        {
            result.error = "cannot mount " + imagePath.string() + " as an ISO9660 disc image";
            return result;
        }

        std::vector<uint8_t> systemCnf;
        if (!readWholeFile(image, "SYSTEM.CNF", systemCnf))
        {
            result.error = "SYSTEM.CNF not found; this is not a PS2 game disc";
            return result;
        }
        if (std::string(systemCnf.begin(), systemCnf.end()).find(kBootElfPath) == std::string::npos)
        {
            result.error = std::string("SYSTEM.CNF does not boot ") + kBootElfPath + "; expected " + kSerial;
            return result;
        }

        std::vector<uint8_t> bootElf;
        if (!readWholeFile(image, kBootElfPath, bootElf))
        {
            result.error = std::string("boot executable ") + kBootElfPath + " missing";
            return result;
        }
        const std::string bootHash = Sha256::hex(bootElf.data(), bootElf.size());
        if (bootHash != kBootElfSha256)
        {
            result.error = std::string("boot executable hash mismatch (") + bootHash + "); the recompiled code was built from " +
                           kSerial + " " + kBootElfSha256;
            return result;
        }
        URBAN_INFO(Boot, kBootElfPath << " sha256 " << bootHash << " (matches " << kSerial << ")");

        std::error_code error;
        std::filesystem::create_directories(cacheDir, error);
        result.bootElfPath = cacheDir / kBootElfPath;
        std::ofstream out(result.bootElfPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bootElf.data()), static_cast<std::streamsize>(bootElf.size()));
        if (!out)
        {
            result.error = "cannot write " + result.bootElfPath.string();
            return result;
        }
        result.ok = true;
        return result;
    }
}
