#include "runtime/ps2_cd_image.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
    uint32_t le32(const uint8_t *p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    bool seekFile(std::FILE *file, uint64_t offset)
    {
#if defined(_WIN32)
        return offset <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) &&
               _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) == 0;
#else
        return offset <= static_cast<uint64_t>(std::numeric_limits<off_t>::max()) &&
               fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
    }

    // Disc overlay: files under <dir>/DATA.CVM/<path> replace the files inside DATA.CVM (CRI CVM = 0x1800-byte header +
    // ISO9660). A file that fits its original sectors is patched in place; a bigger one goes after the end of the image and
    // its directory record (and DATA.CVM's size) point there. Nothing changes when no overlay directory is set.
    struct OverlayPatch
    {
        uint64_t offset = 0;
        std::vector<uint8_t> bytes;
    };
    std::filesystem::path g_overlayDir;
    std::vector<OverlayPatch> g_patches;
    std::vector<uint8_t> g_virtual;     // bytes after the image end
    uint64_t g_virtualBase = 0;         // image size rounded up to a sector

    void put32le(uint8_t *p, uint32_t v)
    {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    }

    void put32be(uint8_t *p, uint32_t v)
    {
        p[3] = static_cast<uint8_t>(v);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[0] = static_cast<uint8_t>(v >> 24);
    }

    std::filesystem::path overlayDirectory()
    {
        if (!g_overlayDir.empty())
            return g_overlayDir;
        if (const char *env = std::getenv("PS2X_DISC_OVERLAY"); env && *env)
            return std::filesystem::path(env);
        return {};
    }
}

void PS2CdImage::setOverlayDirectory(const std::filesystem::path &directory)
{
    PS2CdImage &image = instance();
    std::lock_guard<std::mutex> lock(image.m_mutex);
    g_overlayDir = directory;
    if (image.m_file)
        image.applyOverlayUnlocked();
}

void PS2CdImage::applyOverlayUnlocked()
{
    g_patches.clear();
    g_virtual.clear();
    g_virtualBase = (m_size + kSectorSize - 1) / kSectorSize * kSectorSize;
    const std::filesystem::path dir = overlayDirectory();
    std::error_code error;
    if (dir.empty() || !std::filesystem::is_directory(dir, error))
        return;

    size_t inPlace = 0, appended = 0, unknown = 0;
    for (const auto &top : std::filesystem::directory_iterator(dir, error))
    {
        if (!top.is_directory())
            continue;
        const std::string containerKey = normalizeKey("/" + top.path().filename().string());
        const auto container = m_entries.find(containerKey);
        if (container == m_entries.end() || container->second.directory)
            continue;
        const uint64_t cvmBase = static_cast<uint64_t>(container->second.lba) * kSectorSize;
        uint8_t magic[4] = {};
        if (!readUnlocked(cvmBase, magic, 4) || std::memcmp(magic, "CVMH", 4) != 0)
            continue;
        const uint64_t inner = cvmBase + 0x1800u;
        std::vector<uint8_t> pvd(kSectorSize);
        if (!readUnlocked(inner + 16u * kSectorSize, pvd.data(), kSectorSize) || std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
            continue;

        // path -> (absolute offset of the directory record, lba, size)
        struct InnerFile
        {
            uint64_t record = 0;
            uint32_t lba = 0;
            uint32_t size = 0;
        };
        std::unordered_map<std::string, InnerFile> files;
        std::vector<std::pair<std::pair<uint32_t, uint32_t>, std::string>> pending{{{le32(pvd.data() + 156 + 2), le32(pvd.data() + 156 + 10)}, std::string()}};
        while (!pending.empty())
        {
            const auto [extent, prefix] = pending.back();
            pending.pop_back();
            std::vector<uint8_t> data(extent.second);
            if (extent.second > (16u << 20) || !readUnlocked(inner + static_cast<uint64_t>(extent.first) * kSectorSize, data.data(), data.size()))
                break;
            for (size_t offset = 0; offset < data.size();)
            {
                const uint8_t length = data[offset];
                if (length == 0)
                {
                    offset = (offset / kSectorSize + 1) * kSectorSize;
                    continue;
                }
                if (length < 34 || offset + length > data.size())
                    break;
                const uint8_t *record = data.data() + offset;
                const uint8_t nameLength = record[32];
                if (!(nameLength == 1 && (record[33] == 0 || record[33] == 1)))
                {
                    const std::string key = normalizeKey(prefix + "/" + std::string(reinterpret_cast<const char *>(record + 33), nameLength));
                    if (record[25] & 2u)
                        pending.push_back({{le32(record + 2), le32(record + 10)}, key});
                    else
                        files[key] = InnerFile{inner + static_cast<uint64_t>(extent.first) * kSectorSize + offset, le32(record + 2), le32(record + 10)};
                }
                offset += length;
            }
        }

        uint32_t maxLba = le32(pvd.data() + 80);
        for (const auto &item : std::filesystem::recursive_directory_iterator(top.path(), error))
        {
            if (!item.is_regular_file())
                continue;
            const std::string key = normalizeKey("/" + std::filesystem::relative(item.path(), top.path()).generic_string());
            const auto it = files.find(key);
            if (it == files.end())
            {
                ++unknown;
                std::cerr << "[CDIMAGE] overlay: " << top.path().filename().string() << key << " is not on the disc" << std::endl;
                continue;
            }
            std::FILE *source = nullptr;
#if defined(_WIN32)
            source = ::_wfopen(item.path().c_str(), L"rb");
#else
            source = std::fopen(item.path().string().c_str(), "rb");
#endif
            if (!source)
                continue;
            std::vector<uint8_t> bytes(static_cast<size_t>(std::filesystem::file_size(item.path(), error)));
            const bool readOk = bytes.empty() || std::fread(bytes.data(), 1, bytes.size(), source) == bytes.size();
            std::fclose(source);
            if (!readOk)
                continue;
            InnerFile &file = it->second;
            const uint64_t allocated = (static_cast<uint64_t>(file.size) + kSectorSize - 1) / kSectorSize * kSectorSize;
            uint32_t lba = file.lba;
            if (bytes.size() <= allocated)
            {
                g_patches.push_back({inner + static_cast<uint64_t>(file.lba) * kSectorSize, bytes});
                ++inPlace;
            }
            else
            {
                uint64_t at = g_virtualBase + g_virtual.size();
                at = (at + kSectorSize - 1) / kSectorSize * kSectorSize;
                g_virtual.resize(static_cast<size_t>(at - g_virtualBase), 0);
                g_virtual.insert(g_virtual.end(), bytes.begin(), bytes.end());
                lba = static_cast<uint32_t>((at - inner) / kSectorSize);
                maxLba = std::max<uint32_t>(maxLba, lba + static_cast<uint32_t>((bytes.size() + kSectorSize - 1) / kSectorSize));
                ++appended;
            }
            OverlayPatch record{file.record + 2, std::vector<uint8_t>(16)};
            put32le(record.bytes.data(), lba);
            put32be(record.bytes.data() + 4, lba);
            put32le(record.bytes.data() + 8, static_cast<uint32_t>(bytes.size()));
            put32be(record.bytes.data() + 12, static_cast<uint32_t>(bytes.size()));
            g_patches.push_back(std::move(record));
        }
        if (maxLba != le32(pvd.data() + 80))
        {
            OverlayPatch volume{inner + 16u * kSectorSize + 80, std::vector<uint8_t>(8)};
            put32le(volume.bytes.data(), maxLba);
            put32be(volume.bytes.data() + 4, maxLba);
            g_patches.push_back(std::move(volume));
        }
        if (!g_virtual.empty())
        {
            const uint64_t end = g_virtualBase + g_virtual.size();
            if (end - cvmBase > container->second.size)
                container->second.size = end - cvmBase;
        }
    }
    if (inPlace + appended + unknown)
        std::cout << "[CDIMAGE] overlay " << dir.string() << ": " << inPlace << " in place, " << appended << " appended"
                  << (unknown ? ", " + std::to_string(unknown) + " not on the disc" : std::string()) << std::endl;
}

PS2CdImage &PS2CdImage::instance()
{
    static PS2CdImage image;
    return image;
}

std::string PS2CdImage::normalizeKey(std::string_view ps2Path)
{
    std::string_view path = ps2Path;
    const size_t colon = path.find(':');
    if (colon != std::string_view::npos && colon < 8)
    {
        path.remove_prefix(colon + 1);
    }
    const size_t version = path.rfind(';');
    if (version != std::string_view::npos)
    {
        path = path.substr(0, version);
    }
    std::string key;
    key.reserve(path.size() + 1);
    for (char c : path)
    {
        if (c == '\\')
        {
            c = '/';
        }
        if (c == '/' && (key.empty() || key.back() == '/'))
        {
            continue;
        }
        key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    while (!key.empty() && (key.back() == '/' || key.back() == '.'))
    {
        key.pop_back();
    }
    return key;
}

bool PS2CdImage::open(const std::filesystem::path &imagePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_openAttempted = true;
    if (m_file)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }
    m_entries.clear();
#if defined(_WIN32)
    m_file = ::_wfopen(imagePath.c_str(), L"rb");
#else
    m_file = std::fopen(imagePath.string().c_str(), "rb");
#endif
    if (!m_file)
    {
        std::cerr << "[CDIMAGE] cannot open " << imagePath.string() << std::endl;
        return false;
    }
    std::error_code error;
    m_size = std::filesystem::file_size(imagePath, error);
    m_path = imagePath;

    std::vector<uint8_t> sector(kSectorSize);
    uint32_t lba = 16;
    for (;; ++lba)
    {
        if (!readUnlocked(static_cast<uint64_t>(lba) * kSectorSize, sector.data(), kSectorSize) ||
            std::memcmp(sector.data() + 1, "CD001", 5) != 0 || sector[0] == 255)
        {
            std::cerr << "[CDIMAGE] " << imagePath.string() << " is not an ISO9660 image" << std::endl;
            std::fclose(m_file);
            m_file = nullptr;
            return false;
        }
        if (sector[0] == 1)
        {
            break;
        }
    }
    const uint8_t *root = sector.data() + 156;
    if (!parseDirectory(le32(root + 2), le32(root + 10), std::string(), 0))
    {
        std::fclose(m_file);
        m_file = nullptr;
        m_entries.clear();
        return false;
    }
    std::cout << "[CDIMAGE] mounted " << imagePath.string() << " (" << m_entries.size() << " entries)" << std::endl;
    applyOverlayUnlocked();
    return true;
}

bool PS2CdImage::parseDirectory(uint32_t lba, uint32_t size, const std::string &prefix, int depth)
{
    if (depth > 16 || size > (64u << 20))
    {
        return false;
    }
    std::vector<uint8_t> data(size);
    if (!readUnlocked(static_cast<uint64_t>(lba) * kSectorSize, data.data(), size))
    {
        return false;
    }
    size_t offset = 0;
    while (offset < data.size())
    {
        const uint8_t length = data[offset];
        if (length == 0)
        {
            offset = (offset / kSectorSize + 1) * kSectorSize;
            continue;
        }
        if (offset + length > data.size() || length < 34)
        {
            return false;
        }
        const uint8_t *record = data.data() + offset;
        const uint32_t entryLba = le32(record + 2);
        const uint32_t entrySize = le32(record + 10);
        const bool directory = (record[25] & 2u) != 0u;
        const uint8_t nameLength = record[32];
        offset += length;
        if (nameLength == 1 && (record[33] == 0 || record[33] == 1))
        {
            continue;
        }
        const std::string name(reinterpret_cast<const char *>(record + 33), nameLength);
        const std::string key = normalizeKey(prefix + "/" + name);
        m_entries[key] = Entry{entryLba, entrySize, directory};
        if (directory && !parseDirectory(entryLba, entrySize, key, depth + 1))
        {
            return false;
        }
    }
    return true;
}

void PS2CdImage::close()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file)
    {
        std::fclose(m_file);
        m_file = nullptr;
    }
    m_entries.clear();
    m_openAttempted = false;
}

bool PS2CdImage::isOpen() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_file != nullptr;
}

bool PS2CdImage::ensureOpenFromIoPaths()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file)
        {
            return true;
        }
        if (m_openAttempted)
        {
            return false;
        }
    }
    const std::filesystem::path image = PS2Runtime::getIoPaths().cdImage;
    if (image.empty())
    {
        return false;
    }
    return open(image);
}

bool PS2CdImage::find(std::string_view ps2Path, Entry &out) const
{
    const std::string key = normalizeKey(ps2Path);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_file)
    {
        return false;
    }
    const auto it = m_entries.find(key);
    if (it == m_entries.end())
    {
        return false;
    }
    out = it->second;
    return true;
}

bool PS2CdImage::readUnlocked(uint64_t offset, void *destination, size_t size) const
{
    if (!m_file)
    {
        return false;
    }
    if (g_patches.empty() && g_virtual.empty())
    {
        if (!seekFile(m_file, offset))
        {
            return false;
        }
        return size == 0 || std::fread(destination, 1u, size, m_file) == size;
    }
    auto *out = static_cast<uint8_t *>(destination);
    const uint64_t end = offset + size;
    if (offset < m_size)
    {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(end, m_size) - offset);
        if (!seekFile(m_file, offset) || std::fread(out, 1u, count, m_file) != count)
        {
            return false;
        }
    }
    if (end > m_size)
    {
        const uint64_t virtualEnd = g_virtualBase + g_virtual.size();
        if (end > virtualEnd)
        {
            return false;
        }
        for (uint64_t at = std::max(offset, m_size); at < end; ++at)
        {
            out[at - offset] = at >= g_virtualBase ? g_virtual[static_cast<size_t>(at - g_virtualBase)] : 0u;
        }
    }
    for (const OverlayPatch &patch : g_patches)
    {
        const uint64_t from = std::max(offset, patch.offset);
        const uint64_t to = std::min(end, patch.offset + patch.bytes.size());
        if (from < to)
        {
            std::memcpy(out + (from - offset), patch.bytes.data() + (from - patch.offset), static_cast<size_t>(to - from));
        }
    }
    return true;
}

bool PS2CdImage::read(uint64_t offset, void *destination, size_t size) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return readUnlocked(offset, destination, size);
}

bool PS2CdImage::readSectors(uint32_t lba, uint32_t count, void *destination) const
{
    return read(static_cast<uint64_t>(lba) * kSectorSize, destination, static_cast<size_t>(count) * kSectorSize);
}

uint64_t PS2CdImage::sizeBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_size;
}

uint64_t PS2CdImage::readableSize() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return g_virtual.empty() ? m_size : g_virtualBase + g_virtual.size();
}

std::filesystem::path PS2CdImage::path() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_path;
}
