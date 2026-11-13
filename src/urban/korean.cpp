#include "urban/korean.h"

#include "runtime/ps2_pnach.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace urban::korean
{
    namespace
    {
        // Fixed: "<address> <korean> [<japanese>]" (strings and tables of the executable). The Japanese bytes are read
        // from memory at install (the executable is loaded by then), so the file needs only the Korean ones.
        // Through a pointer: "@<pointer> <magic> <offset> <korean> #<fnv1a64>" (or the old "<japanese>" bytes): the table
        // the game loaded at [pointer] (checked by the magic bytes it starts with); a field is replaced only while it
        // still holds the Japanese bytes, recognised by their hash, and those bytes are kept for original()/japanese().
        struct Patch
        {
            bool viaPointer = false;
            uint32_t address = 0; // fixed address, or the pointer
            std::vector<uint8_t> magic;
            uint32_t offset = 0;
            std::vector<uint8_t> korean;
            std::vector<uint8_t> japanese;
            std::vector<uint8_t> text; // japanese + NUL, for japanese()
            bool hashed = false;       // pointer patch recognising the Japanese field by hash
            uint64_t hash = 0;
        };

        struct State
        {
            std::vector<Patch> patches;
            std::unordered_map<uint32_t, size_t> fixedAt; // fixed patch by address
        };

        // Japanese fields of hashed pointer patches, kept the first time each is replaced (patch index -> bytes + NUL).
        // Entries are never changed once added, so pointers into them stay valid.
        std::mutex g_capturedMutex;
        std::unordered_map<size_t, std::vector<uint8_t>> g_captured;

        uint8_t *g_rdram = nullptr;
        std::shared_ptr<const State> g_patches;

        uint64_t fnv1a64(const uint8_t *data, size_t size)
        {
            uint64_t h = 0xcbf29ce484222325ull;
            for (size_t i = 0; i < size; ++i)
                h = (h ^ data[i]) * 0x100000001b3ull;
            return h;
        }

        std::vector<uint8_t> hexBytes(const std::string &hex)
        {
            std::vector<uint8_t> out;
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
            return out;
        }

        std::vector<Patch> load(const std::filesystem::path &file)
        {
            std::vector<Patch> out;
            std::ifstream in(file);
            std::string line;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                std::vector<std::string> f;
                for (size_t start = 0; start < line.size();)
                {
                    const size_t space = line.find(' ', start);
                    f.push_back(line.substr(start, space == std::string::npos ? std::string::npos : space - start));
                    start = space == std::string::npos ? line.size() : space + 1;
                }
                Patch p;
                if (line[0] == '@' && f.size() == 5)
                {
                    p.viaPointer = true;
                    p.address = static_cast<uint32_t>(std::stoul(f[0].substr(1), nullptr, 16)) & 0x01FFFFFFu;
                    p.magic = hexBytes(f[1]);
                    p.offset = static_cast<uint32_t>(std::stoul(f[2], nullptr, 16));
                    p.korean = hexBytes(f[3]);
                    if (!f[4].empty() && f[4][0] == '#')
                    {
                        p.hashed = true;
                        p.hash = std::stoull(f[4].substr(1), nullptr, 16);
                    }
                    else
                        p.japanese = hexBytes(f[4]);
                    if (p.korean.empty() || (!p.hashed && p.korean.size() != p.japanese.size()))
                        continue;
                }
                else if (f.size() == 2 || f.size() == 3)
                {
                    p.address = static_cast<uint32_t>(std::stoul(f[0], nullptr, 16)) & 0x01FFFFFFu;
                    p.korean = hexBytes(f[1]);
                    if (p.korean.empty() || p.address + p.korean.size() > 0x02000000u)
                        continue;
                    if (f.size() == 3)
                        p.japanese = hexBytes(f[2]);
                    if (p.japanese.size() != p.korean.size())
                        p.japanese.clear();
                }
                else
                    continue;
                p.text = p.japanese;
                p.text.push_back(0);
                out.push_back(std::move(p));
            }
            return out;
        }

        // The target of a pointer patch now, or 0 when the table is not loaded.
        uint32_t target(const Patch &p)
        {
            uint32_t base = 0;
            std::memcpy(&base, g_rdram + p.address, sizeof(base));
            base &= 0x01FFFFFFu;
            if (base < 0x100000u || base + p.offset + p.korean.size() > 0x02000000u ||
                base + p.magic.size() > 0x02000000u || std::memcmp(g_rdram + base, p.magic.data(), p.magic.size()) != 0)
                return 0;
            return base + p.offset;
        }

        // Only where the memory differs.
        void apply(uint8_t *rdram, const std::vector<Patch> &patches)
        {
            for (size_t i = 0; i < patches.size(); ++i)
            {
                const Patch &p = patches[i];
                if (!p.viaPointer)
                {
                    uint8_t *at = rdram + p.address;
                    if (std::memcmp(at, p.korean.data(), p.korean.size()) != 0)
                        std::memcpy(at, p.korean.data(), p.korean.size());
                    continue;
                }
                const uint32_t to = target(p);
                if (to == 0u)
                    continue;
                uint8_t *at = rdram + to;
                if (p.hashed)
                {
                    if (fnv1a64(at, p.korean.size()) != p.hash)
                        continue;
                    {
                        std::lock_guard<std::mutex> lock(g_capturedMutex);
                        if (g_captured.find(i) == g_captured.end())
                        {
                            std::vector<uint8_t> text(at, at + p.korean.size());
                            text.push_back(0);
                            g_captured.emplace(i, std::move(text));
                        }
                    }
                    std::memcpy(at, p.korean.data(), p.korean.size());
                }
                else if (std::memcmp(at, p.japanese.data(), p.japanese.size()) == 0)
                    std::memcpy(at, p.korean.data(), p.korean.size());
            }
        }

        // The Japanese text of pointer patch i (bytes + NUL), or nullptr while it has not been seen yet.
        const uint8_t *pointerText(const State &state, size_t i)
        {
            const Patch &p = state.patches[i];
            if (!p.hashed)
                return p.text.data();
            std::lock_guard<std::mutex> lock(g_capturedMutex);
            const auto it = g_captured.find(i);
            return it == g_captured.end() ? nullptr : it->second.data();
        }
    }

    std::string original(uint32_t address)
    {
        const auto state = std::atomic_load(&g_patches);
        if (!state || !g_rdram)
            return {};
        for (size_t i = 0; i < state->patches.size(); ++i)
        {
            const Patch &p = state->patches[i];
            if (p.viaPointer && target(p) == (address & 0x01FFFFFFu))
            {
                const uint8_t *text = pointerText(*state, i);
                return text ? std::string(reinterpret_cast<const char *>(text)) : std::string();
            }
        }
        return {};
    }

    const char *japanese(const char *text)
    {
        const auto state = std::atomic_load(&g_patches);
        const auto *at = reinterpret_cast<const uint8_t *>(text);
        if (!state || !g_rdram || !text || at < g_rdram || at >= g_rdram + 0x02000000u)
            return text;
        const uint32_t address = static_cast<uint32_t>(at - g_rdram);
        if (const auto it = state->fixedAt.find(address); it != state->fixedAt.end())
            return reinterpret_cast<const char *>(state->patches[it->second].text.data());
        for (size_t i = 0; i < state->patches.size(); ++i)
            if (state->patches[i].viaPointer && target(state->patches[i]) == address)
            {
                const uint8_t *original = pointerText(*state, i);
                return original ? reinterpret_cast<const char *>(original) : text;
            }
        return text;
    }

    void install(uint8_t *rdram, const std::filesystem::path &folder)
    {
        std::error_code error;
        const std::filesystem::path file = folder / "RAM_PATCH.TXT";
        if (!rdram || !std::filesystem::is_regular_file(file, error))
            return;
        auto state = std::make_shared<State>();
        state->patches = load(file);
        if (state->patches.empty())
            return;
        // The executable is loaded and not patched yet: its own bytes are the Japanese originals of the fixed patches.
        size_t kept = 0;
        for (size_t i = 0; i < state->patches.size(); ++i)
        {
            Patch &p = state->patches[i];
            if (p.viaPointer)
                continue;
            if (p.japanese.empty())
            {
                p.japanese.assign(rdram + p.address, rdram + p.address + p.korean.size());
                p.text = p.japanese;
                p.text.push_back(0);
                ++kept;
            }
            state->fixedAt.emplace(p.address, i);
        }
        g_rdram = rdram;
        std::shared_ptr<const State> shared = state;
        std::atomic_store(&g_patches, shared);
        std::cout << "[korean] " << shared->patches.size() << " names patched in memory, " << kept
                  << " originals kept from the executable (" << file.string() << ")" << std::endl;
        // Now, before the game starts (code that copies a pointer table at boot sees the Korean one), and then every
        // frame: the executable's data comes back with every save state; the plan table is loaded later, from a package.
        apply(rdram, shared->patches);
        ps2x::pnach::addFrameHook([rdram, shared]() { apply(rdram, shared->patches); });
    }
}
