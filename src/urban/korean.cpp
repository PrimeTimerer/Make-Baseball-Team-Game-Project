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
        // Fixed: "<address> <korean> [<japanese>]" (strings and tables of the executable, the bytes they replace).
        // Through a pointer: "@<pointer> <magic> <offset> <korean> <japanese>": the table the game loaded at [pointer]
        // (checked by the magic bytes it starts with); only a field that still holds the Japanese bytes is replaced.
        struct Patch
        {
            bool viaPointer = false;
            uint32_t address = 0; // fixed address, or the pointer
            std::vector<uint8_t> magic;
            uint32_t offset = 0;
            std::vector<uint8_t> korean;
            std::vector<uint8_t> japanese;
            std::vector<uint8_t> text; // japanese + NUL, for japanese()
        };

        struct State
        {
            std::vector<Patch> patches;
            std::unordered_map<uint32_t, size_t> fixedAt; // fixed patch with Japanese bytes by address
        };

        uint8_t *g_rdram = nullptr;
        std::shared_ptr<const State> g_patches;

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
                    p.japanese = hexBytes(f[4]);
                    if (p.korean.size() != p.japanese.size() || p.korean.empty())
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

        void apply(uint8_t *rdram, const std::vector<Patch> &patches);

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
            for (const Patch &p : patches)
            {
                if (!p.viaPointer)
                {
                    uint8_t *at = rdram + p.address;
                    if (std::memcmp(at, p.korean.data(), p.korean.size()) != 0)
                        std::memcpy(at, p.korean.data(), p.korean.size());
                    continue;
                }
                const uint32_t to = target(p);
                if (to != 0u && std::memcmp(rdram + to, p.japanese.data(), p.japanese.size()) == 0)
                    std::memcpy(rdram + to, p.korean.data(), p.korean.size());
            }
        }
    }

    std::string original(uint32_t address)
    {
        const auto state = std::atomic_load(&g_patches);
        if (!state || !g_rdram)
            return {};
        for (const Patch &p : state->patches)
            if (p.viaPointer && target(p) == (address & 0x01FFFFFFu))
            {
                const auto end = std::find(p.japanese.begin(), p.japanese.end(), uint8_t{0});
                return std::string(p.japanese.begin(), end);
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
        for (const Patch &p : state->patches)
            if (p.viaPointer && target(p) == address)
                return reinterpret_cast<const char *>(p.text.data());
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
        for (size_t i = 0; i < state->patches.size(); ++i)
            if (!state->patches[i].viaPointer && !state->patches[i].japanese.empty())
                state->fixedAt.emplace(state->patches[i].address, i);
        g_rdram = rdram;
        std::shared_ptr<const State> shared = state;
        std::atomic_store(&g_patches, shared);
        std::cout << "[korean] " << shared->patches.size() << " names patched in memory (" << file.string() << ")"
                  << std::endl;
        // Now, before the game starts (code that copies a pointer table at boot sees the Korean one), and then every
        // frame: the executable's data comes back with every save state; the plan table is loaded later, from a package.
        apply(rdram, shared->patches);
        ps2x::pnach::addFrameHook([rdram, shared]() { apply(rdram, shared->patches); });
    }
}
