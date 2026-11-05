#include "urban/player_cheats.h"

#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_PLAYER_MENU 1
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace urban::players
{
    namespace
    {
        // YKTK3 (SLPM-65853): the same chain after a new game and after loading a save. This port's heap is laid out
        // 0x40 below PCSX2's, so fixed addresses from PCSX2 cheats miss; the chain does not.
        constexpr uint32_t kTeamTable = 0x5D1240u;    // 12 team pointers: lions 0 ... yokohama 11
        constexpr uint32_t kTeamRoster = 0x120u;      // team -> roster
        constexpr uint32_t kRosterClass = 0x4u;       // roster -> its class; the player's own team has its own one
        constexpr uint32_t kOwnTeamClass = 0x6659F0u;
        constexpr uint32_t kCpuTeamClass = 0x665378u;
        constexpr uint32_t kRosterPlayers = 0x8u;     // roster -> 40 player record pointers
        constexpr uint32_t kRecordSize = 0x1CCu;      // the own team's records
        constexpr uint32_t kCpuRecordSize = 0xF8u;    // CPU teams': the same fields, the ability block replaced by a pointer
        // The ability block (6-byte entries [?][aptitude][value][limit][u16]): inline at +0x6C..+0x143 in the own team's
        // records, elsewhere in CPU teams' (their +0x6C points to it; same layout, offsets - 0x6C). Checked 2026-11-05 in a
        // March save: 440 CPU records, e.g. 新垣 (Hawks 18) 球速 75 of 82 = 155 km/h by the owner's ISO notes.
        constexpr uint32_t kBlock = 0x6Cu, kBlockSize = 0xD8u;
        // Skills: 8 u16 skill IDs at +0x44 in both kinds of record (0 = empty; the game loops over 8 there).
        constexpr uint32_t kSkills = 0x44u;
        constexpr int kSkillSlots = 8;
        // Record fields: player ID (+0, 0 in an empty slot), name (+4, 16 bytes Shift-JIS), uniform number (+0x17),
        // age (+0x18), not 0 at +0x26 for pitchers.
        constexpr uint32_t kName = 0x4u, kNumber = 0x17u, kAge = 0x18u, kPitcher = 0x26u;
        constexpr int kTeams = 12, kSlots = 40;
        constexpr uint32_t kRamSize = 0x2000000u;
        constexpr uint32_t kOwnBit = 1u << kTeams;     // in Settings::teams: whichever team is the player's own
        constexpr uint32_t kAllTeams = (1u << kTeams) - 1u;
        constexpr const char *kTeamNames[kTeams] = {"라이온즈", "호크스",   "파이터즈", "마린즈",   "버팔로즈", "골든이글스",
                                                    "드래곤즈", "스왈로즈", "자이언츠", "타이거즈", "카프",     "요코하마"};

        struct Skill
        {
            uint16_t id;
            int group;
            const char *ko, *jp;
        };
        constexpr Skill kSkillTable[] = {
#include "urban/player_skills.inc"
        };
        constexpr const char *kSkillGroups[] = {"선수 스킬", "WS 결정구", "감독 스킬", "코치 스킬"};

        struct Run
        {
            uint32_t offset = 0u;
            std::vector<uint8_t> bytes;
        };
        struct Item
        {
            std::string key, label, note, section;
            std::vector<Run> runs;
        };
        struct Pin
        {
            bool on = true;
            uint32_t mask = 0u;
            std::vector<uint16_t> skills;
            std::string name;
        };
        enum Scope
        {
            kScopeOwn = 0,   // the own team's players (as before 2026-11-05)
            kScopeTeams = 1, // the players of the chosen teams
            kScopeAll = 2    // every team's players
        };
        struct Settings
        {
            bool enabled = false;
            bool all = true; // the default items for the players of the scope without a pin (off: only the picked players)
            int scope = kScopeOwn;
            uint32_t teams = kOwnBit; // kScopeTeams: bit t = team t, kOwnBit = the own team
            uint32_t mask = 0u;       // the default items (bit i = g_items[i])
            std::vector<uint16_t> skills; // the default skills, in slot order (at most 8)
            std::map<uint16_t, Pin> pins; // the picked players, by player ID, on any team
        };
        struct Row
        {
            uint16_t id = 0u;
            int team = -1;
            int number = 0, age = 0;
            bool pitcher = false;
            std::string name;
        };
        using Slots = std::array<uint16_t, kSkillSlots>;
        // A player whose skills the cheat changed in this run: what the slots held before and what it wrote last.
        struct SkillBackup
        {
            Slots before{}, written{};
        };

        uint8_t *g_rdram = nullptr;
        std::vector<Item> g_items; // read once at start
        std::filesystem::path g_itemsPath, g_settingsPath;
        bool g_safeMode = false;
        std::mutex g_mutex; // the settings and the rosters: the menu (GS thread) and the frame hook (EE thread)
        Settings g_settings;
        int g_team = -1;
        std::array<std::vector<Row>, kTeams> g_rows;
        std::map<uint16_t, SkillBackup> g_skillBackups; // EE thread only, by player ID

        std::string trim(std::string text)
        {
            const char *spaces = " \t\r\n";
            text.erase(0, text.find_first_not_of(spaces));
            const size_t last = text.find_last_not_of(spaces);
            text.erase(last == std::string::npos ? 0u : last + 1u);
            return text;
        }

        std::string utf8(const std::filesystem::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        uint32_t read32(uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, g_rdram + address, sizeof(value));
            return value;
        }

        uint16_t read16(uint32_t address)
        {
            uint16_t value;
            std::memcpy(&value, g_rdram + address, sizeof(value));
            return value;
        }

        // A guest pointer this code follows: word aligned, past the kernel, with `size` bytes inside the 32 MB.
        bool valid(uint32_t address, uint32_t size)
        {
            return (address & 3u) == 0u && address >= 0x100000u && address < kRamSize && size <= kRamSize - address;
        }

        uint32_t allItems()
        {
            return g_items.size() >= 32u ? 0xFFFFFFFFu : (1u << g_items.size()) - 1u;
        }

        const Skill *findSkill(uint16_t id)
        {
            for (const Skill &skill : kSkillTable)
                if (skill.id == id)
                    return &skill;
            return nullptr;
        }

        // player_cheat_items.txt: "== section ==", "[key] label", then "> note" and "offset: byte byte ..." lines (hex,
        // from the start of an own-team player record); "#" comments. At most 32 items.
        std::vector<Item> loadItems(const std::filesystem::path &path)
        {
            std::vector<Item> items;
            std::ifstream file(path, std::ios::binary);
            std::string line, section;
            bool first = true;
            while (std::getline(file, line))
            {
                if (first && line.rfind("\xEF\xBB\xBF", 0) == 0u)
                    line.erase(0, 3);
                first = false;
                line = trim(line);
                if (line.empty() || line[0] == '#')
                    continue;
                if (line.rfind("==", 0) == 0u)
                {
                    const size_t start = line.find_first_not_of("= ");
                    const size_t end = line.find_last_not_of("= ");
                    section = start == std::string::npos ? std::string() : line.substr(start, end - start + 1);
                    continue;
                }
                if (line[0] == '[')
                {
                    const size_t close = line.find(']');
                    if (close == std::string::npos || items.size() >= 32u)
                        continue;
                    Item item;
                    item.section = section;
                    item.key = trim(line.substr(1, close - 1));
                    item.label = trim(line.substr(close + 1));
                    if (item.label.empty())
                        item.label = item.key;
                    items.push_back(std::move(item));
                    continue;
                }
                if (items.empty())
                    continue;
                if (line[0] == '>')
                {
                    items.back().note = trim(line.substr(1));
                    continue;
                }
                const size_t colon = line.find(':');
                if (colon == std::string::npos)
                    continue;
                Run run;
                char *end = nullptr;
                const std::string offset = trim(line.substr(0, colon));
                run.offset = static_cast<uint32_t>(std::strtoul(offset.c_str(), &end, 16));
                if (offset.empty() || *end != '\0')
                    continue;
                std::istringstream bytes(line.substr(colon + 1));
                std::string byte;
                while (bytes >> byte)
                    run.bytes.push_back(static_cast<uint8_t>(std::strtoul(byte.c_str(), nullptr, 16)));
                if (!run.bytes.empty() && run.offset + run.bytes.size() <= kRecordSize)
                    items.back().runs.push_back(std::move(run));
            }
            return items;
        }

        uint32_t maskOf(const std::string &keys)
        {
            uint32_t mask = 0u;
            std::istringstream list(keys);
            std::string key;
            while (std::getline(list, key, '+'))
                for (size_t i = 0; i < g_items.size(); ++i)
                    if (g_items[i].key == trim(key))
                        mask |= 1u << i;
            return mask;
        }

        std::string keysOf(uint32_t mask)
        {
            std::string keys;
            for (size_t i = 0; i < g_items.size(); ++i)
                if (mask & (1u << i))
                    keys += (keys.empty() ? "" : "+") + g_items[i].key;
            return keys;
        }

        // "0B+50+2B": skill IDs in hex, kept in order, known ones only, at most 8.
        std::vector<uint16_t> skillsOf(const std::string &text)
        {
            std::vector<uint16_t> skills;
            std::istringstream list(text);
            std::string id;
            while (std::getline(list, id, '+'))
            {
                id = trim(id);
                char *end = nullptr;
                const unsigned long value = std::strtoul(id.c_str(), &end, 16);
                if (id.empty() || *end != '\0' || value > 0xFFFFu || !findSkill(static_cast<uint16_t>(value)))
                    continue;
                const uint16_t skill = static_cast<uint16_t>(value);
                if (std::find(skills.begin(), skills.end(), skill) == skills.end() && skills.size() < kSkillSlots)
                    skills.push_back(skill);
            }
            return skills;
        }

        std::string skillText(const std::vector<uint16_t> &skills)
        {
            std::string text;
            char hex[8];
            for (uint16_t skill : skills)
            {
                std::snprintf(hex, sizeof(hex), "%02X", skill);
                text += (text.empty() ? "" : "+") + std::string(hex);
            }
            return text;
        }

        std::string teamsText(uint32_t teams)
        {
            std::string text = (teams & kOwnBit) ? "own" : "";
            for (int t = 0; t < kTeams; ++t)
                if (teams & (1u << t))
                    text += (text.empty() ? "" : "+") + std::to_string(t);
            return text;
        }

        // player_cheats.ini: enabled=, all=, scope=own|teams|all, teams=own+<team>..., items=key+key, skills=<hex>+<hex>,
        // pin=<player id>,<on>,<key+key>,<name>, pinskills=<player id>,<hex+hex>. Files without scope= (before
        // 2026-11-05) keep their meaning: the own team.
        Settings loadSettings(const std::filesystem::path &path)
        {
            Settings settings;
            settings.mask = allItems();
            std::map<uint16_t, std::vector<uint16_t>> pinSkills;
            std::ifstream file(path, std::ios::binary);
            std::string line;
            while (std::getline(file, line))
            {
                line = trim(line);
                const size_t equals = line.find('=');
                if (line.empty() || line[0] == '#' || equals == std::string::npos)
                    continue;
                const std::string key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
                if (key == "enabled")
                    settings.enabled = value == "1";
                else if (key == "all")
                    settings.all = value == "1";
                else if (key == "scope")
                    settings.scope = value == "all" ? kScopeAll : value == "teams" ? kScopeTeams : kScopeOwn;
                else if (key == "teams")
                {
                    settings.teams = 0u;
                    std::istringstream list(value);
                    std::string team;
                    while (std::getline(list, team, '+'))
                    {
                        team = trim(team);
                        char *end = nullptr;
                        const unsigned long t = std::strtoul(team.c_str(), &end, 10);
                        if (team == "own")
                            settings.teams |= kOwnBit;
                        else if (!team.empty() && *end == '\0' && t < static_cast<unsigned long>(kTeams))
                            settings.teams |= 1u << t;
                    }
                }
                else if (key == "items")
                    settings.mask = maskOf(value);
                else if (key == "skills")
                    settings.skills = skillsOf(value);
                else if (key == "pin" || key == "pinskills")
                {
                    std::istringstream fields(value);
                    std::string id, on, items, name;
                    if (!std::getline(fields, id, ','))
                        continue;
                    const unsigned long number = std::strtoul(trim(id).c_str(), nullptr, 10);
                    if (number == 0u || number > 0xFFFFu)
                        continue;
                    if (key == "pinskills")
                    {
                        std::getline(fields, items);
                        pinSkills[static_cast<uint16_t>(number)] = skillsOf(items);
                        continue;
                    }
                    if (!std::getline(fields, on, ',') || !std::getline(fields, items, ','))
                        continue;
                    std::getline(fields, name);
                    Pin pin;
                    pin.on = trim(on) == "1";
                    pin.mask = maskOf(items);
                    pin.name = trim(name);
                    settings.pins[static_cast<uint16_t>(number)] = std::move(pin);
                }
            }
            for (auto &[id, skills] : pinSkills)
                if (auto pin = settings.pins.find(id); pin != settings.pins.end())
                    pin->second.skills = std::move(skills);
            return settings;
        }

        void saveSettings(const Settings &settings)
        {
            std::ofstream file(g_settingsPath, std::ios::binary | std::ios::trunc);
            file << "# 선수 치트 설정: 게임 메뉴(F2)의 \"선수 치트\" 탭이 씁니다\n"
                 << "enabled=" << (settings.enabled ? 1 : 0) << "\n"
                 << "all=" << (settings.all ? 1 : 0) << "\n"
                 << "scope=" << (settings.scope == kScopeAll ? "all" : settings.scope == kScopeTeams ? "teams" : "own") << "\n"
                 << "teams=" << teamsText(settings.teams) << "\n"
                 << "items=" << keysOf(settings.mask) << "\n"
                 << "skills=" << skillText(settings.skills) << "\n";
            for (const auto &[id, pin] : settings.pins)
            {
                file << "pin=" << id << "," << (pin.on ? 1 : 0) << "," << keysOf(pin.mask) << "," << pin.name << "\n";
                if (!pin.skills.empty())
                    file << "pinskills=" << id << "," << skillText(pin.skills) << "\n";
            }
        }

        // The record's name (Shift-JIS, padded with spaces) as UTF-8 with single spaces.
        std::string nameOf(uint32_t record)
        {
#if defined(_WIN32)
            char sjis[16];
            std::memcpy(sjis, g_rdram + record + kName, sizeof(sjis));
            int length = 0;
            while (length < 16 && sjis[length] != '\0')
                ++length;
            wchar_t wide[16];
            const int count = length > 0 ? MultiByteToWideChar(932, 0, sjis, length, wide, 16) : 0;
            std::wstring name;
            bool gap = false;
            for (int i = 0; i < count; ++i)
            {
                if (wide[i] == L' ' || wide[i] == 0x3000)
                {
                    gap = !name.empty();
                    continue;
                }
                if (gap)
                    name += L' ';
                gap = false;
                name += wide[i];
            }
            char text[96];
            const int size = name.empty() ? 0
                                          : WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), text,
                                                                static_cast<int>(sizeof(text)), nullptr, nullptr);
            return std::string(text, size > 0 ? static_cast<size_t>(size) : 0u);
#else
            (void)record;
            return std::string();
#endif
        }

        // Whether the default items reach team t's players without a pin.
        bool inScope(const Settings &settings, int t, int own)
        {
            if (!settings.all)
                return false;
            if (settings.scope == kScopeAll)
                return true;
            if (settings.scope == kScopeTeams)
                return ((settings.teams >> t) & 1u) != 0u || ((settings.teams & kOwnBit) != 0u && t == own);
            return t == own;
        }

        // The chosen items into a record. `block` is its ability block (own team: record + 0x6C).
        void writeItems(uint32_t record, uint32_t block, bool own, uint32_t mask)
        {
            for (size_t i = 0; i < g_items.size(); ++i)
            {
                if (!(mask & (1u << i)))
                    continue;
                for (const Run &run : g_items[i].runs)
                {
                    const uint32_t end = run.offset + static_cast<uint32_t>(run.bytes.size());
                    uint32_t at;
                    if (end <= kBlock)
                        at = record + run.offset;
                    else if (run.offset >= kBlock && end <= kBlock + kBlockSize)
                        at = block + (run.offset - kBlock);
                    else if (own)
                        at = record + run.offset;
                    else
                        continue; // past the block: CPU records have nothing there
                    std::memcpy(g_rdram + at, run.bytes.data(), run.bytes.size());
                }
            }
        }

        Slots readSkills(uint32_t record)
        {
            Slots slots;
            std::memcpy(slots.data(), g_rdram + record + kSkills, sizeof(slots));
            return slots;
        }

        void writeSkills(uint32_t record, const Slots &slots)
        {
            std::memcpy(g_rdram + record + kSkills, slots.data(), sizeof(slots));
        }

        // The chosen skills, added to the player's own: one they lack goes into an empty slot or, all 8 full, over the last
        // skill not chosen. Not chosen any more (or the cheat off), the slots go back to what they held before, unless the
        // game changed them meanwhile. The backup lives for this run only.
        void applySkills(uint32_t record, uint16_t id, const std::vector<uint16_t> *chosen)
        {
            auto backup = g_skillBackups.find(id);
            if (!chosen || chosen->empty())
            {
                if (backup != g_skillBackups.end())
                {
                    if (readSkills(record) == backup->second.written)
                        writeSkills(record, backup->second.before);
                    g_skillBackups.erase(backup);
                }
                return;
            }
            const Slots now = readSkills(record);
            if (backup == g_skillBackups.end())
                backup = g_skillBackups.emplace(id, SkillBackup{now, now}).first;
            else if (now != backup->second.written)
                backup->second.before = now; // the game (or a loaded save) wrote them: those are the player's own now
            Slots want = backup->second.before;
            for (uint16_t skill : *chosen)
            {
                if (std::find(want.begin(), want.end(), skill) != want.end())
                    continue;
                auto slot = std::find(want.begin(), want.end(), uint16_t{0});
                for (int i = kSkillSlots - 1; slot == want.end() && i >= 0; --i)
                    if (std::find(chosen->begin(), chosen->end(), want[static_cast<size_t>(i)]) == chosen->end())
                        slot = want.begin() + i;
                if (slot != want.end())
                    *slot = skill;
            }
            if (want != now)
                writeSkills(record, want);
            backup->second.written = want;
        }

        // EE thread, at every vblank after the .pnach patches: find the 12 rosters (the own team by its class) and write
        // the chosen items and skills into the targeted players.
        void frame()
        {
            static unsigned tick = 0u;
            const bool refresh = (++tick % 30u) == 0u;
            std::lock_guard<std::mutex> lock(g_mutex);
            uint32_t rosters[kTeams] = {};
            int team = -1;
            for (int t = 0; t < kTeams; ++t)
            {
                const uint32_t object = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                if (!valid(object, kTeamRoster + 4u))
                    continue;
                const uint32_t roster = read32(object + kTeamRoster);
                if (!valid(roster, kRosterPlayers + 4u * kSlots))
                    continue;
                const uint32_t kind = read32(roster + kRosterClass);
                if (kind == kOwnTeamClass && team < 0)
                    team = t;
                else if (kind != kCpuTeamClass)
                    continue;
                rosters[t] = roster;
            }
            if (team != g_team)
                std::fprintf(stderr, "[players] own team: %d\n", team);
            const bool rebuild = team != g_team || refresh;
            g_team = team;
            if (rebuild)
                for (std::vector<Row> &rows : g_rows)
                    rows.clear();
            const bool apply = team >= 0 && g_settings.enabled;
            if (!rebuild && !apply && g_skillBackups.empty())
                return;
            for (int t = 0; t < kTeams; ++t)
            {
                if (rosters[t] == 0u)
                    continue;
                const bool own = t == team;
                const bool scoped = inScope(g_settings, t, team);
                for (int k = 0; k < kSlots; ++k)
                {
                    const uint32_t record = read32(rosters[t] + kRosterPlayers + 4u * static_cast<uint32_t>(k));
                    if (!valid(record, own ? kRecordSize : kCpuRecordSize))
                        continue;
                    const uint16_t id = read16(record);
                    if (id == 0u)
                        continue; // an empty slot
                    const uint32_t block = own ? record + kBlock : read32(record + kBlock);
                    if (!own && !valid(block, kBlockSize))
                        continue;
                    if (rebuild)
                    {
                        Row row;
                        row.id = id;
                        row.team = t;
                        row.number = g_rdram[record + kNumber];
                        row.age = g_rdram[record + kAge];
                        row.pitcher = g_rdram[record + kPitcher] != 0u;
                        row.name = nameOf(record);
                        g_rows[static_cast<size_t>(t)].push_back(std::move(row));
                    }
                    uint32_t mask = 0u;
                    const std::vector<uint16_t> *skills = nullptr;
                    if (apply)
                    {
                        const auto pin = g_settings.pins.find(id);
                        if (pin != g_settings.pins.end())
                        {
                            if (pin->second.on)
                            {
                                mask = pin->second.mask;
                                skills = &pin->second.skills;
                            }
                        }
                        else if (scoped)
                        {
                            mask = g_settings.mask;
                            skills = &g_settings.skills;
                        }
                    }
                    if (mask != 0u)
                        writeItems(record, block, own, mask);
                    if (skills || !g_skillBackups.empty())
                        applySkills(record, id, skills);
                }
            }
        }

#if defined(URBAN_PLAYER_MENU)
        uint16_t g_selected = 0u; // what the items show: 0 = the default items, else a picked player (GS thread)
        int g_browse = -1;        // the team the player list shows (-1: the own team)
        int g_range = 0;          // the player list: 0 everyone, 1 pitchers, 2 fielders

        // The chosen skills (at most 8, in slot order), then every skill by group, two to a row.
        void drawSkills(const Pcsx2GsUi *ui, std::vector<uint16_t> &skills, bool &changed)
        {
            ui->text_disabled(("고른 스킬 " + std::to_string(skills.size()) + " / 8 (선수마다 8칸) · 아래 버튼을 누르면 뺍니다").c_str());
            if (!skills.empty())
            {
                ui->same_line();
                if (ui->small_button("고른 스킬 모두 빼기"))
                {
                    skills.clear();
                    changed = true;
                }
            }
            for (size_t i = 0; i < skills.size(); ++i)
            {
                const Skill *skill = findSkill(skills[i]);
                ui->push_id(400000 + skills[i]);
                if (i % 4u != 0u)
                    ui->same_line();
                const bool drop = ui->small_button((std::string(skill ? skill->ko : "?") + "  x").c_str()) != 0;
                ui->pop_id();
                if (drop)
                {
                    skills.erase(skills.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    break;
                }
            }
            ui->text_disabled("원래 스킬에 더합니다: 빈 칸에 넣고, 8칸이 차면 고르지 않은 스킬을 뒤에서부터 바꿉니다.");
            ui->text_disabled("빼거나 끄면 이번 실행에서 바꾼 칸을 원래 스킬로 돌립니다.");
            if (!ui->begin_tab_bar("skillgroups"))
                return;
            for (int group = 0; group < 4; ++group)
            {
                if (!ui->begin_tab(kSkillGroups[group]))
                    continue;
                ui->begin_child("skilllist", 0.0f, 170.0f);
                int column = 0;
                for (const Skill &skill : kSkillTable)
                {
                    if (skill.group != group)
                        continue;
                    const auto at = std::find(skills.begin(), skills.end(), skill.id);
                    int on = at != skills.end() ? 1 : 0;
                    ui->push_id(300000 + skill.id);
                    if (column++ % 2 != 0)
                        ui->same_line();
                    ui->begin_disabled(!on && skills.size() >= static_cast<size_t>(kSkillSlots) ? 1 : 0);
                    if (ui->checkbox((std::string(skill.ko) + "  " + skill.jp).c_str(), &on))
                    {
                        if (on)
                            skills.push_back(skill.id);
                        else
                            skills.erase(at);
                        changed = true;
                    }
                    ui->end_disabled();
                    ui->pop_id();
                }
                ui->end_child();
                ui->end_tab();
            }
            ui->end_tab_bar();
        }

        // The items of `mask`, a tab per section of player_cheat_items.txt, three to a row; then the skills tab.
        void drawItems(const Pcsx2GsUi *ui, uint32_t &mask, std::vector<uint16_t> &skills, bool &changed)
        {
            if (ui->small_button("능력치 모두 켜기"))
            {
                mask = allItems();
                changed = true;
            }
            ui->same_line();
            if (ui->small_button("능력치 모두 끄기"))
            {
                mask = 0u;
                changed = true;
            }
            ui->same_line();
            ui->text_disabled(("능력치 " + std::to_string(std::popcount(mask)) + " / " + std::to_string(g_items.size()) + " 항목 · 스킬 " +
                               std::to_string(skills.size()) + " / 8")
                                  .c_str());
            if (!ui->begin_tab_bar("sections"))
                return;
            for (size_t first = 0; first < g_items.size();)
            {
                size_t end = first;
                while (end < g_items.size() && g_items[end].section == g_items[first].section)
                    ++end;
                const std::string &section = g_items[first].section;
                if (ui->begin_tab(section.empty() ? "항목" : section.c_str()))
                {
                    for (size_t i = first; i < end; ++i)
                    {
                        ui->push_id(static_cast<int>(i));
                        if ((i - first) % 3u != 0u)
                            ui->same_line();
                        int on = (mask >> i) & 1u ? 1 : 0;
                        if (ui->checkbox(g_items[i].label.c_str(), &on))
                        {
                            mask = on ? mask | (1u << i) : mask & ~(1u << i);
                            changed = true;
                        }
                        ui->pop_id();
                    }
                    for (size_t i = first; i < end; ++i)
                        if (!g_items[i].note.empty())
                            ui->text_disabled((g_items[i].label + ": " + g_items[i].note).c_str());
                    ui->end_tab();
                }
                first = end;
            }
            if (ui->begin_tab("스킬 (하나씩)"))
            {
                drawSkills(ui, skills, changed);
                ui->end_tab();
            }
            ui->end_tab_bar();
        }

        std::string scopeText(const Settings &settings, int team)
        {
            if (!settings.all)
                return "고른 선수만";
            if (settings.scope == kScopeAll)
                return "모든 구단 모든 선수";
            if (settings.scope == kScopeOwn)
                return team >= 0 ? std::string("내 구단(") + kTeamNames[team] + ") 전원" : std::string("내 구단 전원");
            std::string text;
            int count = 0;
            if (settings.teams & kOwnBit)
            {
                text = "내 구단";
                ++count;
            }
            for (int t = 0; t < kTeams; ++t)
                if (((settings.teams >> t) & 1u) && !(t == team && (settings.teams & kOwnBit)))
                {
                    text += (text.empty() ? "" : "·") + std::string(kTeamNames[t]);
                    ++count;
                }
            if (count > 4)
                return "고른 구단 " + std::to_string(count) + "곳 전원";
            return text.empty() ? std::string("고른 구단 없음") : text + " 전원";
        }

        // GS thread, while the menu is open. Works on a copy; a change is stored and saved at the end. The layout follows
        // PROSPI 26 Studio's 대상 고르기: the target (own team / chosen teams / every team / picked players only), the items,
        // then the player list where a picked player gets items of their own.
        void draw(const Pcsx2GsUi *ui)
        {
            if (g_safeMode)
            {
                ui->text_wrapped("안전 모드로 시작해서 선수 치트가 꺼져 있습니다.");
                return;
            }
            if (g_items.empty())
            {
                ui->text_wrapped(("항목 파일이 없습니다: " + utf8(g_itemsPath)).c_str());
                return;
            }
            Settings settings;
            int team;
            std::array<std::vector<Row>, kTeams> rows;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                settings = g_settings;
                team = g_team;
                rows = g_rows;
            }
            bool changed = false;
            size_t players = 0;
            for (const std::vector<Row> &list : rows)
                players += list.size();
            const auto rowOf = [&](uint16_t id) -> const Row * {
                for (const std::vector<Row> &list : rows)
                    for (const Row &row : list)
                        if (row.id == id)
                            return &row;
                return nullptr;
            };

            int enabled = settings.enabled ? 1 : 0;
            if (ui->checkbox("선수 치트 켜기", &enabled))
            {
                settings.enabled = enabled != 0;
                changed = true;
            }
            ui->same_line();
            if (team < 0)
                ui->text_disabled("내 구단을 찾는 중: 세이브를 불러오거나 새 게임을 시작하면 자동으로 찾습니다");
            else
                ui->text_disabled((std::string("내 구단 ") + kTeamNames[team] + " (자동 감지) · 12개 구단 선수 " + std::to_string(players) + "명")
                                      .c_str());

            // 1. Who gets the default items.
            ui->separator_text("대상 고르기: 기본 항목을 쓸 선수");
            const int mode = settings.all ? settings.scope : 3;
            const char *modes[4] = {"내 구단 전원", "고른 구단 전원", "모든 구단 모든 선수", "고른 선수만"};
            for (int m = 0; m < 4; ++m)
            {
                if (m != 0)
                    ui->same_line();
                if (ui->radio(modes[m], mode == m ? 1 : 0) && mode != m)
                {
                    settings.all = m != 3;
                    if (m != 3)
                        settings.scope = m;
                    changed = true;
                }
            }
            if (settings.all && settings.scope == kScopeTeams)
            {
                int own = (settings.teams & kOwnBit) ? 1 : 0;
                if (ui->checkbox("내 구단 (어느 구단이든 자동)", &own))
                {
                    settings.teams = own ? settings.teams | kOwnBit : settings.teams & ~kOwnBit;
                    changed = true;
                }
                ui->same_line();
                if (ui->small_button("12개 구단 모두"))
                {
                    settings.teams |= kAllTeams;
                    changed = true;
                }
                ui->same_line();
                if (ui->small_button("구단 모두 해제"))
                {
                    settings.teams = 0u;
                    changed = true;
                }
                for (int t = 0; t < kTeams; ++t)
                {
                    ui->push_id(200000 + t);
                    if (t % 4 != 0)
                        ui->same_line();
                    int on = ((settings.teams >> t) & 1u) ? 1 : 0;
                    const std::string label = std::string(kTeamNames[t]) + (t == team ? " (내)" : "");
                    if (ui->checkbox(label.c_str(), &on))
                    {
                        settings.teams = on ? settings.teams | (1u << t) : settings.teams & ~(1u << t);
                        changed = true;
                    }
                    ui->pop_id();
                }
            }
            ui->text_colored(0xFF66CCFFu, ("적용 대상: " + scopeText(settings, team) + (settings.pins.empty() ? std::string()
                                                                                                   : " + 고른 선수 " + std::to_string(settings.pins.size()) + "명 (각자 항목)"))
                                              .c_str());
            ui->text_disabled("'선수 치트 켜기'가 켜져 있을 때만 씁니다 (고르기만으로는 안 바뀜).");

            // 2. The items: the default ones or a picked player's.
            ui->separator_text("능력치 · 스킬");
            if (g_selected != 0u && settings.pins.find(g_selected) == settings.pins.end())
                g_selected = 0u;
            std::vector<uint16_t> targets{0u};
            std::vector<std::string> names{"기본 항목: 위 대상 전원"};
            for (const auto &[id, pin] : settings.pins)
            {
                const Row *row = rowOf(id);
                targets.push_back(id);
                names.push_back("고른 선수: " + (pin.name.empty() ? "ID " + std::to_string(id) : pin.name) +
                                (row ? std::string(" · ") + kTeamNames[row->team] : std::string(" · 로스터에 없음")));
            }
            std::vector<const char *> pointers;
            for (const std::string &name : names)
                pointers.push_back(name.c_str());
            int current = 0;
            for (size_t i = 0; i < targets.size(); ++i)
                if (targets[i] == g_selected)
                    current = static_cast<int>(i);
            ui->set_next_width(340.0f);
            if (ui->combo("편집 대상", &current, pointers.data(), static_cast<int>(pointers.size())))
                g_selected = targets[static_cast<size_t>(current)];
            const auto selected = settings.pins.find(g_selected);
            bool apply = true;
            if (selected != settings.pins.end())
            {
                ui->same_line();
                int on = selected->second.on ? 1 : 0;
                if (ui->checkbox("이 선수에게 적용", &on))
                {
                    selected->second.on = on != 0;
                    changed = true;
                }
                apply = selected->second.on;
            }
            uint32_t &mask = selected == settings.pins.end() ? settings.mask : selected->second.mask;
            std::vector<uint16_t> &skills = selected == settings.pins.end() ? settings.skills : selected->second.skills;
            ui->begin_disabled(apply ? 0 : 1);
            drawItems(ui, mask, skills, changed);
            ui->end_disabled();

            // 3. The players: any team's, picked ones get items of their own.
            ui->separator_text("선수 고르기: 고른 선수는 자기 항목을 씁니다 (구단이 바뀌어도 따라감)");
            if (g_browse < 0 || g_browse >= kTeams)
                g_browse = team >= 0 ? team : 0;
            std::vector<std::string> teamLabels;
            for (int t = 0; t < kTeams; ++t)
                teamLabels.push_back(std::string(kTeamNames[t]) + " · " + std::to_string(rows[static_cast<size_t>(t)].size()) + "명" +
                                     (t == team ? " (내 구단)" : ""));
            std::vector<const char *> teamPointers;
            for (const std::string &label : teamLabels)
                teamPointers.push_back(label.c_str());
            ui->set_next_width(260.0f);
            ui->combo("구단##browse", &g_browse, teamPointers.data(), kTeams);
            ui->same_line();
            const char *ranges[3] = {"전체", "투수", "야수"};
            ui->set_next_width(90.0f);
            ui->combo("범위##range", &g_range, ranges, 3);
            std::vector<const Row *> visible;
            for (const Row &row : rows[static_cast<size_t>(g_browse)])
                if (g_range == 0 || (g_range == 1) == row.pitcher)
                    visible.push_back(&row);
            if (ui->small_button("보이는 선수 전체 고르기"))
            {
                for (const Row *row : visible)
                    if (settings.pins.find(row->id) == settings.pins.end())
                    {
                        Pin added;
                        added.mask = settings.mask;
                        added.skills = settings.skills;
                        added.name = row->name;
                        settings.pins[row->id] = std::move(added);
                    }
                changed = true;
            }
            ui->same_line();
            if (ui->small_button("보이는 선수 고르기 해제"))
            {
                for (const Row *row : visible)
                    settings.pins.erase(row->id);
                changed = true;
            }
            if (!settings.pins.empty())
            {
                ui->same_line();
                if (ui->small_button(("고른 선수 모두 해제 (" + std::to_string(settings.pins.size()) + ")").c_str()))
                {
                    settings.pins.clear();
                    changed = true;
                }
            }
            const bool scoped = inScope(settings, g_browse, team);
            ui->begin_child("players", 0.0f, 190.0f);
            if (visible.empty())
                ui->text_disabled(team < 0 ? "게임을 시작하면 명단이 나옵니다." : "이 구단·범위에 선수가 없습니다.");
            for (const Row *row : visible)
            {
                ui->push_id(row->id);
                auto pin = settings.pins.find(row->id);
                int pinned = pin != settings.pins.end() ? 1 : 0;
                if (ui->checkbox("고르기", &pinned))
                {
                    if (pinned)
                    {
                        Pin added;
                        added.mask = settings.mask;
                        added.skills = settings.skills;
                        added.name = row->name;
                        settings.pins[row->id] = std::move(added);
                        g_selected = row->id;
                    }
                    else
                        settings.pins.erase(row->id);
                    changed = true;
                    pin = settings.pins.find(row->id);
                }
                ui->same_line();
                char line[160];
                std::snprintf(line, sizeof(line), "%2d번  %s  (%s, %d세)", row->number, row->name.empty() ? "?" : row->name.c_str(),
                              row->pitcher ? "투수" : "야수", row->age);
                ui->text(line);
                ui->same_line();
                if (pin == settings.pins.end())
                    ui->text_disabled(scoped ? "기본 항목" : "대상 아님");
                else
                {
                    const std::string state = pin->second.on ? "개별 " + std::to_string(std::popcount(pin->second.mask)) + "/" +
                                                                   std::to_string(g_items.size()) + "항목 · 스킬 " +
                                                                   std::to_string(pin->second.skills.size())
                                                             : std::string("개별: 끔");
                    ui->text_colored(0xFF66CCFFu, state.c_str());
                    ui->same_line();
                    if (ui->small_button(g_selected == row->id ? "설정 중" : "설정"))
                        g_selected = row->id;
                }
                ui->pop_id();
            }
            ui->end_child();

            // Picked players on no roster now (released, retired, another save): kept for when they are back.
            int away = 0;
            for (auto it = settings.pins.begin(); it != settings.pins.end();)
            {
                if (rowOf(it->first) || team < 0)
                {
                    ++it;
                    continue;
                }
                if (away++ == 0)
                    ui->separator_text("로스터에 없는 고른 선수 (돌아오면 다시 적용)");
                ui->push_id(100000 + it->first);
                const bool remove = ui->small_button("고르기 해제") != 0;
                ui->same_line();
                ui->text_disabled((it->second.name.empty() ? "ID " + std::to_string(it->first) : it->second.name).c_str());
                ui->pop_id();
                if (remove)
                {
                    it = settings.pins.erase(it);
                    changed = true;
                }
                else
                    ++it;
            }

            if (changed)
            {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_settings = settings;
                }
                saveSettings(settings);
            }
        }
#endif
    }

    void install(uint8_t *rdram, const std::filesystem::path &userDir, const std::filesystem::path &exeDir)
    {
        if (!rdram)
            return;
        g_rdram = rdram;
        const char *mods = std::getenv("URBAN_MODS");
        g_safeMode = mods && std::strcmp(mods, ";") == 0; // the launcher's safe start
        std::error_code error;
        g_itemsPath = userDir / "player_cheat_items.txt";
        if (!std::filesystem::exists(g_itemsPath, error) && userDir != exeDir)
            g_itemsPath = exeDir / "player_cheat_items.txt";
        g_items = loadItems(g_itemsPath);
        g_settingsPath = userDir / "player_cheats.ini";
        g_settings = loadSettings(g_settingsPath);
        std::fprintf(stderr, "[players] %zu items (%s), %zu skills; player cheats %s, %zu picked, scope %d%s\n", g_items.size(),
                     utf8(g_itemsPath).c_str(), std::size(kSkillTable), g_settings.enabled ? "on" : "off", g_settings.pins.size(),
                     g_settings.all ? g_settings.scope : 3, g_safeMode ? "; safe start: off" : "");
#if defined(URBAN_PLAYER_MENU)
        ps2x::menu::addPanel("선수 치트", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
        if (!g_safeMode && !g_items.empty())
            ps2x::pnach::addFrameHook(frame);
    }
}
