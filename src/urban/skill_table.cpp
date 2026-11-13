#include "urban/skill_table.h"
#include "urban/korean.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

namespace urban::skills
{
    namespace
    {
        constexpr Skill kTable[] = {
#include "urban/player_skills.inc"
        };
        constexpr const char *kGroupNames[kGroups] = {"선수 스킬", "WS 결정구", "감독 스킬", "코치 스킬", "스카우트 스킬"};
        // The game's name tables (SLPM-65853 ELF data, the same addresses in guest RAM): pointers to Shift-JIS names by
        // index. Player IDs index the first directly; manager skills are 0xC8 + index, coach skills 0x12C + index.
        constexpr uint32_t kPlayerNames = 0x5D0598u, kManagerNames = 0x5D0758u, kCoachNames = 0x5D07D0u, kScoutNames = 0x5D0848u;
        constexpr uint32_t kRamSize = 0x2000000u;

        std::mutex g_mutex;
        std::map<uint32_t, std::string> g_japanese; // group << 16 | id

        std::string utf8FromSjis(const char *text, size_t size)
        {
#if defined(_WIN32)
            wchar_t wide[64];
            const int count = size > 0 ? MultiByteToWideChar(932, 0, text, static_cast<int>(size), wide, 64) : 0;
            if (count <= 0)
                return std::string();
            // Half-width katakana (WSﾁｪﾝｼﾞｱｯﾌﾟ) as full-width for the menu font; ASCII stays ASCII.
            wchar_t full[128];
            int length = LCMapStringEx(L"ja-JP", LCMAP_FULLWIDTH, wide, count, full, 128, nullptr, nullptr, 0);
            if (length <= 0)
            {
                std::memcpy(full, wide, sizeof(wchar_t) * static_cast<size_t>(count));
                length = count;
            }
            for (int i = 0; i < length; ++i)
                if (full[i] >= 0xFF01 && full[i] <= 0xFF5E)
                    full[i] = static_cast<wchar_t>(full[i] - 0xFEE0);
            char out[256];
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, full, length, out, static_cast<int>(sizeof(out)), nullptr, nullptr);
            return std::string(out, bytes > 0 ? static_cast<size_t>(bytes) : 0u);
#else
            (void)text;
            (void)size;
            return std::string();
#endif
        }

        std::string japanese(const uint8_t *rdram, const Skill &skill)
        {
            if (!rdram)
                return std::string();
            const uint32_t key = (static_cast<uint32_t>(skill.group) << 16) | skill.id;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (const auto it = g_japanese.find(key); it != g_japanese.end())
                    return it->second;
            }
            uint32_t table = kPlayerNames, index = skill.id;
            if (skill.group == kScout)
                table = kScoutNames;
            else if (skill.group == kManager)
                table = kManagerNames, index = skill.id - 0xC8u;
            else if (skill.group == kCoach)
                table = kCoachNames, index = skill.id - 0x12Cu;
            uint32_t pointer;
            std::memcpy(&pointer, rdram + table + 4u * index, sizeof(pointer));
            if (pointer < 0x100000u || pointer >= kRamSize - 64u)
                return std::string(); // not loaded yet
            const char *text = urban::korean::japanese(reinterpret_cast<const char *>(rdram + pointer));
            std::string name = utf8FromSjis(text, strnlen(text, 48));
            if (!name.empty())
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_japanese[key] = name;
            }
            return name;
        }
    }

    const Skill *begin()
    {
        return kTable;
    }

    const Skill *end()
    {
        return kTable + std::size(kTable);
    }

    const Skill *find(uint16_t id, bool scout)
    {
        for (const Skill &skill : kTable)
            if (skill.id == id && (skill.group == kScout) == scout)
                return &skill;
        return nullptr;
    }

    const char *groupName(int group)
    {
        return group >= 0 && group < kGroups ? kGroupNames[group] : "?";
    }

    std::string label(const uint8_t *rdram, const Skill &skill)
    {
        const std::string jp = japanese(rdram, skill);
        return jp.empty() ? std::string(skill.ko) : std::string(skill.ko) + "  " + jp;
    }
}
