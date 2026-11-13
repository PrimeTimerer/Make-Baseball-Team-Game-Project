#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

class PS2CdImage
{
public:
    struct Entry
    {
        uint32_t lba = 0;
        uint64_t size = 0;
        bool directory = false;
    };

    static constexpr uint32_t kSectorSize = 2048u;

    static PS2CdImage &instance();

    bool open(const std::filesystem::path &imagePath);
    void close();
    bool isOpen() const;
    bool ensureOpenFromIoPaths();

    bool find(std::string_view ps2Path, Entry &out) const;
    bool read(uint64_t offset, void *destination, size_t size) const;
    bool readSectors(uint32_t lba, uint32_t count, void *destination) const;
    uint64_t sizeBytes() const;
    uint64_t readableSize() const; // sizeBytes() plus the overlay's appended files
    std::filesystem::path path() const;

    static std::string normalizeKey(std::string_view ps2Path);

    // Files under <directory>/DATA.CVM/<path> replace files inside DATA.CVM (also PS2X_DISC_OVERLAY=<directory>).
    static void setOverlayDirectory(const std::filesystem::path &directory);

private:
    PS2CdImage() = default;
    bool parseDirectory(uint32_t lba, uint32_t size, const std::string &prefix, int depth);
    void applyOverlayUnlocked();
    bool readUnlocked(uint64_t offset, void *destination, size_t size) const;

    mutable std::mutex m_mutex;
    std::FILE *m_file = nullptr;
    std::filesystem::path m_path;
    uint64_t m_size = 0;
    bool m_openAttempted = false;
    std::unordered_map<std::string, Entry> m_entries;
};
