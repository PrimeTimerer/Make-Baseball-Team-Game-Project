#include "urban/staff_cheats.h"
#include "urban/save_identity.h"

#include "urban/skill_table.h"

#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_STAFF_MENU 1
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace urban::staff
{
    namespace
    {
        constexpr uint32_t kTeamTable = 0x5D1240u; // 12 team objects: lions 0 ... yokohama 11
        constexpr uint32_t kTeamRoster = 0x120u, kRosterClass = 0x4u, kOwnTeamClass = 0x6659F0u;
        // Team object -> staff (2026-11-05, two teams of a March save): +0x124 manager, +0x128 pitching coach, +0x12C
        // fielding coach, +0x130 training coach, +0x134 the scout department, whose +0x488 and +0x48C are the two scouts.
        constexpr uint32_t kStaff = 0x124u, kScoutDepartment = 0x134u, kScoutList = 0x488u;
        // Staff record: ID (+0), name (+2, 16 bytes Shift-JIS), the manager's personality (+0x1A temperament, +0x1B style,
        // 0-10), skills (+0x1C, 8 u16; scouts: +0x1A, 6 scout skill IDs), coaching abilities from +0x3A (coaches, 0x80
        // bytes a record; the manager's record is 0x40).
        constexpr uint32_t kName = 0x2u, kTemper = 0x1Au, kStyle = 0x1Bu, kSkills = 0x1Cu, kScoutSkills = 0x1Au;
        constexpr uint32_t kAbilityBase = 0x3Au, kBackupStart = 0x1Au;
        constexpr int kTeams = 12, kRoles = 6, kSkillSlots = 8, kScoutSkillSlots = 6;
        constexpr uint32_t kRamSize = 0x2000000u;
        constexpr const char *kTeamNames[kTeams] = {"라이온즈", "호크스",   "파이터즈", "마린즈",   "버팔로즈", "골든이글스",
                                                    "드래곤즈", "스왈로즈", "자이언츠", "타이거즈", "카프",     "요코하마"};
        constexpr const char *kRoleNames[kRoles] = {"감독", "투수 코치", "야수 코치", "트레이닝 코치", "스카우트 1", "스카우트 2"};
        constexpr const char *kRankNames[6] = {"그대로", "D", "C", "B", "A", "S"};
        // The manager's personality: the game shows 0-3, 4-6 and 7-10 as these (sub_3EED70); written as 2, 5 and 8.
        constexpr const char *kTempers[4] = {"그대로", "냉정 (冷静)", "온화 (温和)", "열혈 (熱血)"};
        constexpr const char *kStyles[4] = {"그대로", "대담한 용병 (大胆采配)", "임기응변 (臨機応変)", "견실파 (堅実派)"};
        constexpr uint8_t kPersonalityValue[3] = {2u, 5u, 8u};

        // The coaching abilities as the game reads them (sub_3EEE10, offsets from +0x3A; 0-4 = D..S). Kind 0: one byte;
        // 1: the mean of two (巧打: left and right); 2: the best level of the 10 pitches ([type][level] pairs from +4).
        struct Ability
        {
            int role; // 1 pitching coach, 2 fielding coach, 3 training coach
            const char *ko;
            int kind;
            uint8_t offset, second;
        };
        constexpr Ability kAbilityTable[] = {
            {1, "체력 지도", 0, 0x00, 0},      {1, "구속 지도", 0, 0x01, 0},       {1, "구위 지도", 0, 0x02, 0},
            {1, "제구 지도", 0, 0x03, 0},      {1, "변화구 지도", 2, 0x04, 0},     {1, "정신 지도", 0, 0x21, 0},
            {2, "체력 지도", 0, 0x00, 0},      {2, "교타 지도", 1, 0x18, 0x19},    {2, "장타 지도", 0, 0x1A, 0},
            {2, "번트 지도", 0, 0x1D, 0},      {2, "주루 지도", 0, 0x1E, 0},       {2, "포수 리드 지도", 0, 0x1F, 0},
            {2, "정신 지도", 0, 0x21, 0},      {2, "포구 지도", 0, 0x22, 0},       {2, "송구 지도", 0, 0x23, 0},
            {2, "수비 반응 지도", 0, 0x24, 0}, {3, "축적 피로 회복", 0, 0x25, 0}, {3, "부상 회복", 0, 0x28, 0},
            {3, "의욕 회복", 0, 0x27, 0},      {3, "컨디션 회복", 0, 0x26, 0},
        };
        constexpr int kAbilityCount = static_cast<int>(std::size(kAbilityTable));
        constexpr uint32_t kAbilityEnd = kAbilityBase + 0x29u; // A[0x28] is the last one read

        struct Entry
        {
            bool lock = false;           // 고정: written at every frame while the tab's switch is on
            int temper = -1, style = -1; // the manager: 0..2, -1 = as it is
            std::array<int8_t, kAbilityCount> abilities{};
            std::vector<uint16_t> skills; // chosen, put first in slot order; empty = as they are
            Entry()
            {
                abilities.fill(-1);
            }
            bool idle() const
            {
                return !lock && temper < 0 && style < 0 && skills.empty() &&
                       std::all_of(abilities.begin(), abilities.end(), [](int8_t v) { return v < 0; });
            }
        };
        struct Settings
        {
            bool enabled = false;
            std::map<int, std::array<Entry, kRoles>> teams; // -1: the own team (whichever it is), else the team index
        };
        struct View
        {
            uint32_t record = 0u;
            uint16_t id = 0u;
            std::string name;
            int temper = 0, style = 0;
            std::array<int, kAbilityCount> abilities{};
            std::vector<uint16_t> skills;
        };
        struct Backup
        {
            uint16_t id = 0u;
            std::vector<uint8_t> bytes; // from +0x1A
        };
        struct Action
        {
            int team; // as in Settings::teams
            int role;
            bool restore;
        };

        uint8_t *g_rdram = nullptr;
        std::filesystem::path g_settingsPath; // this save's (urban::saveid), empty before any game
        uint32_t g_generation = ~0u;          // urban::saveid::generation() the settings belong to
        std::string g_profile;
        bool g_safeMode = false;
        std::mutex g_mutex; // everything below: the menu (GS thread) and the frame hook (EE thread)
        Settings g_settings;
        int g_own = -1;
        std::array<std::array<View, kRoles>, kTeams> g_views;
        std::map<uint32_t, Backup> g_backups; // by record address, for this run
        std::vector<Action> g_actions;

        std::string trim(std::string text)
        {
            const char *spaces = " \t\r\n";
            text.erase(0, text.find_first_not_of(spaces));
            const size_t last = text.find_last_not_of(spaces);
            text.erase(last == std::string::npos ? 0u : last + 1u);
            return text;
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

        bool valid(uint32_t address, uint32_t size)
        {
            return (address & 3u) == 0u && address >= 0x100000u && address < kRamSize && size <= kRamSize - address;
        }

        bool isScout(int role)
        {
            return role >= 4;
        }

        uint32_t recordSize(int role)
        {
            return role == 0 ? 0x40u : isScout(role) ? 0x30u : 0x80u;
        }

        uint32_t backupEnd(int role)
        {
            return role == 0 ? kSkills + 2u * kSkillSlots : isScout(role) ? kScoutSkills + 2u * kScoutSkillSlots : kAbilityEnd;
        }

        // Team t's staff record for a role, 0 if there is none (or the game is not set up).
        uint32_t recordOf(int team, int role)
        {
            if (team < 0 || team >= kTeams)
                return 0u;
            const uint32_t object = read32(kTeamTable + 4u * static_cast<uint32_t>(team));
            if (!valid(object, kScoutDepartment + 4u))
                return 0u;
            uint32_t record;
            if (!isScout(role))
                record = read32(object + kStaff + 4u * static_cast<uint32_t>(role));
            else
            {
                const uint32_t department = read32(object + kScoutDepartment);
                if (!valid(department, kScoutList + 8u))
                    return 0u;
                record = read32(department + kScoutList + 4u * static_cast<uint32_t>(role - 4));
            }
            if (!valid(record, recordSize(role)) || read16(record) == 0u || g_rdram[record + kName] == 0u)
                return 0u;
            return record;
        }

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

        int readAbility(uint32_t record, const Ability &ability)
        {
            const uint8_t *a = g_rdram + record + kAbilityBase;
            const auto at = [&](int i) { return static_cast<int>(static_cast<int8_t>(a[i])); };
            if (ability.kind == 1)
                return (at(ability.offset) + at(ability.second) + 1) / 2;
            if (ability.kind == 2)
            {
                int best = -1;
                for (int k = 0; k < 10; ++k)
                    if (a[4 + 2 * k] != 0u)
                        best = std::max(best, at(5 + 2 * k));
                return best;
            }
            return at(ability.offset);
        }

        void writeAbility(uint32_t record, const Ability &ability, int value)
        {
            uint8_t *a = g_rdram + record + kAbilityBase;
            const uint8_t v = static_cast<uint8_t>(value);
            if (ability.kind == 1)
            {
                a[ability.offset] = v;
                a[ability.second] = v;
            }
            else if (ability.kind == 2)
            {
                bool any = false;
                for (int k = 0; k < 10; ++k)
                    if (a[4 + 2 * k] != 0u)
                    {
                        a[5 + 2 * k] = v;
                        any = true;
                    }
                if (!any)
                {
                    a[4] = 3u; // スライダー
                    a[5] = v;
                }
            }
            else
                a[ability.offset] = v;
        }

        std::vector<uint16_t> readSkills(uint32_t record, int role)
        {
            const int slots = isScout(role) ? kScoutSkillSlots : kSkillSlots;
            std::vector<uint16_t> skills(static_cast<size_t>(slots));
            std::memcpy(skills.data(), g_rdram + record + (isScout(role) ? kScoutSkills : kSkills), 2u * skills.size());
            return skills;
        }

        // The chosen skills first, in their order, then the record's other skills, as many as fit.
        void writeSkills(uint32_t record, int role, const std::vector<uint16_t> &chosen)
        {
            const std::vector<uint16_t> now = readSkills(record, role);
            std::vector<uint16_t> want;
            for (uint16_t skill : chosen)
                if (skill != 0u && want.size() < now.size() && std::find(want.begin(), want.end(), skill) == want.end())
                    want.push_back(skill);
            for (uint16_t skill : now)
                if (skill != 0u && want.size() < now.size() && std::find(want.begin(), want.end(), skill) == want.end())
                    want.push_back(skill);
            want.resize(now.size(), 0u);
            if (want != now)
                std::memcpy(g_rdram + record + (isScout(role) ? kScoutSkills : kSkills), want.data(), 2u * want.size());
        }

        void keepBackup(uint32_t record, int role)
        {
            const auto it = g_backups.find(record);
            if (it != g_backups.end() && it->second.id == read16(record))
                return;
            Backup backup;
            backup.id = read16(record);
            backup.bytes.assign(g_rdram + record + kBackupStart, g_rdram + record + backupEnd(role));
            g_backups[record] = std::move(backup);
        }

        void restore(uint32_t record)
        {
            const auto it = g_backups.find(record);
            if (it == g_backups.end())
                return;
            if (it->second.id == read16(record))
                std::memcpy(g_rdram + record + kBackupStart, it->second.bytes.data(), it->second.bytes.size());
            g_backups.erase(it);
        }

        void apply(uint32_t record, int role, const Entry &entry)
        {
            keepBackup(record, role);
            if (role == 0)
            {
                if (entry.temper >= 0 && entry.temper < 3)
                    g_rdram[record + kTemper] = kPersonalityValue[entry.temper];
                if (entry.style >= 0 && entry.style < 3)
                    g_rdram[record + kStyle] = kPersonalityValue[entry.style];
            }
            for (int i = 0; i < kAbilityCount; ++i)
                if (kAbilityTable[i].role == role && entry.abilities[static_cast<size_t>(i)] >= 0)
                    writeAbility(record, kAbilityTable[i], entry.abilities[static_cast<size_t>(i)]);
            if (!entry.skills.empty())
                writeSkills(record, role, entry.skills);
        }

        int ownTeam()
        {
            for (int t = 0; t < kTeams; ++t)
            {
                const uint32_t object = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                if (!valid(object, kTeamRoster + 4u))
                    continue;
                const uint32_t roster = read32(object + kTeamRoster);
                if (valid(roster, kRosterClass + 4u) && read32(roster + kRosterClass) == kOwnTeamClass)
                    return t;
            }
            return -1;
        }

        // EE thread, at every vblank after the .pnach patches: the menu's one-off writes and restores, then the locked
        // entries; every 15 frames the staff the menu shows.
        Settings loadSettings(const std::filesystem::path &path); // below

        void frame()
        {
            static unsigned tick = 0u;
            std::lock_guard<std::mutex> lock(g_mutex);
            // Another save, or a new game (owner, 11-06 and 11-08: settings per save, a new game starts with none).
            if (const uint32_t generation = urban::saveid::generation(); generation != g_generation)
            {
                g_generation = generation;
                g_profile = urban::saveid::current();
                g_settingsPath = urban::saveid::settingsFile(g_profile, "staff_cheats.ini");
                std::error_code error;
                g_settings = !g_settingsPath.empty() && std::filesystem::exists(g_settingsPath, error) ? loadSettings(g_settingsPath) : Settings{};
                g_backups.clear(); // their records were the old game's
                g_actions.clear();
                std::fprintf(stderr, "[staff] save %s: staff cheats %s, %zu teams\n", g_profile.empty() ? "(none)" : g_profile.c_str(),
                             g_settings.enabled ? "on" : "off", g_settings.teams.size());
            }
            const int own = ownTeam();
            g_own = own;
            const auto teamOf = [own](int key) { return key < 0 ? own : key; };
            for (const Action &action : g_actions)
            {
                const uint32_t record = recordOf(teamOf(action.team), action.role);
                if (record == 0u)
                    continue;
                if (action.restore)
                    restore(record);
                else if (const auto entries = g_settings.teams.find(action.team); entries != g_settings.teams.end())
                    apply(record, action.role, entries->second[static_cast<size_t>(action.role)]);
            }
            const bool acted = !g_actions.empty();
            g_actions.clear();
            if (g_settings.enabled && own >= 0)
                for (const auto &[key, entries] : g_settings.teams)
                    for (int role = 0; role < kRoles; ++role)
                        if (entries[static_cast<size_t>(role)].lock)
                            if (const uint32_t record = recordOf(teamOf(key), role); record != 0u)
                                apply(record, role, entries[static_cast<size_t>(role)]);
            if ((++tick % 15u) != 0u && !acted)
                return;
            for (int t = 0; t < kTeams; ++t)
                for (int role = 0; role < kRoles; ++role)
                {
                    View view;
                    view.record = own >= 0 ? recordOf(t, role) : 0u;
                    if (view.record != 0u)
                    {
                        view.id = read16(view.record);
                        view.name = nameOf(view.record);
                        view.temper = g_rdram[view.record + kTemper];
                        view.style = g_rdram[view.record + kStyle];
                        if (role >= 1 && role <= 3)
                            for (int i = 0; i < kAbilityCount; ++i)
                                if (kAbilityTable[i].role == role)
                                    view.abilities[static_cast<size_t>(i)] = readAbility(view.record, kAbilityTable[i]);
                        for (uint16_t skill : readSkills(view.record, role))
                            if (skill != 0u)
                                view.skills.push_back(skill);
                    }
                    g_views[static_cast<size_t>(t)][static_cast<size_t>(role)] = std::move(view);
                }
        }

        // staff_cheats.ini: enabled=, entry=<own|team>,<role>,<lock>,<temper>,<style>,<20 abilities: - or 0-4>,<hex+hex>
        Settings loadSettings(const std::filesystem::path &path)
        {
            Settings settings;
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
                {
                    settings.enabled = value == "1";
                    continue;
                }
                if (key != "entry")
                    continue;
                std::vector<std::string> fields;
                std::istringstream list(value);
                std::string field;
                while (std::getline(list, field, ','))
                    fields.push_back(trim(field));
                if (fields.size() < 6)
                    continue;
                const int team = fields[0] == "own" ? -1 : std::atoi(fields[0].c_str());
                const int role = std::atoi(fields[1].c_str());
                if (team < -1 || team >= kTeams || role < 0 || role >= kRoles)
                    continue;
                Entry entry;
                entry.lock = fields[2] == "1";
                entry.temper = std::clamp(std::atoi(fields[3].c_str()), -1, 2);
                entry.style = std::clamp(std::atoi(fields[4].c_str()), -1, 2);
                for (int i = 0; i < kAbilityCount && i < static_cast<int>(fields[5].size()); ++i)
                    if (fields[5][static_cast<size_t>(i)] >= '0' && fields[5][static_cast<size_t>(i)] <= '4')
                        entry.abilities[static_cast<size_t>(i)] = static_cast<int8_t>(fields[5][static_cast<size_t>(i)] - '0');
                if (fields.size() > 6)
                {
                    std::istringstream ids(fields[6]);
                    std::string id;
                    const size_t slots = isScout(role) ? kScoutSkillSlots : kSkillSlots;
                    while (std::getline(ids, id, '+'))
                    {
                        char *end = nullptr;
                        const unsigned long skill = std::strtoul(id.c_str(), &end, 16);
                        if (!id.empty() && *end == '\0' && urban::skills::find(static_cast<uint16_t>(skill), isScout(role)) &&
                            entry.skills.size() < slots)
                            entry.skills.push_back(static_cast<uint16_t>(skill));
                    }
                }
                settings.teams[team][static_cast<size_t>(role)] = std::move(entry);
            }
            return settings;
        }

        void saveSettings(const Settings &settings, const std::filesystem::path &path)
        {
            if (path.empty())
                return; // before any game: nothing to keep it for
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << "# 스태프 치트 설정: 게임 메뉴(F2)의 \"스태프 치트\" 탭이 씁니다\n"
                 << "# entry=<own 또는 구단 0-11>,<역할 0 감독 1 투수코치 2 야수코치 3 트레이닝코치 4·5 스카우트>,<고정>,<기질>,<용병>,"
                    "<능력 20칸: - 그대로, 0-4 = D-S>,<스킬 16진+16진>\n"
                 << "enabled=" << (settings.enabled ? 1 : 0) << "\n";
            for (const auto &[team, entries] : settings.teams)
                for (int role = 0; role < kRoles; ++role)
                {
                    const Entry &entry = entries[static_cast<size_t>(role)];
                    if (entry.idle())
                        continue;
                    std::string abilities;
                    for (int8_t value : entry.abilities)
                        abilities += value < 0 ? '-' : static_cast<char>('0' + value);
                    std::string skills;
                    char hex[8];
                    for (uint16_t skill : entry.skills)
                    {
                        std::snprintf(hex, sizeof(hex), "%X", skill);
                        skills += (skills.empty() ? "" : "+") + std::string(hex);
                    }
                    file << "entry=" << (team < 0 ? std::string("own") : std::to_string(team)) << "," << role << ","
                         << (entry.lock ? 1 : 0) << "," << entry.temper << "," << entry.style << "," << abilities << "," << skills
                         << "\n";
                }
        }

#if defined(URBAN_STAFF_MENU)
        int g_teamKey = -1; // the team the tab shows (-1: the own team); GS thread

        std::string skillNames(const std::vector<uint16_t> &skills, bool scout)
        {
            std::string text;
            for (uint16_t id : skills)
            {
                const urban::skills::Skill *skill = urban::skills::find(id, scout);
                text += (text.empty() ? "" : ", ") + std::string(skill ? skill->ko : "?");
            }
            return text.empty() ? std::string("없음") : text;
        }

        void drawSkills(const Pcsx2GsUi *ui, int role, Entry &entry, bool &changed)
        {
            const bool scout = isScout(role);
            const size_t slots = scout ? kScoutSkillSlots : kSkillSlots;
            ui->text_disabled(("고른 스킬 " + std::to_string(entry.skills.size()) + " / " + std::to_string(slots) +
                               " · 앞 칸부터 넣고 남는 칸에 원래 스킬을 둡니다 · 누르면 뺍니다")
                                  .c_str());
            for (size_t i = 0; i < entry.skills.size(); ++i)
            {
                const urban::skills::Skill *skill = urban::skills::find(entry.skills[i], scout);
                ui->push_id(500000 + entry.skills[i]);
                if (i % 4u != 0u)
                    ui->same_line();
                const bool drop = ui->small_button((std::string(skill ? skill->ko : "?") + "  x").c_str()) != 0;
                ui->pop_id();
                if (drop)
                {
                    entry.skills.erase(entry.skills.begin() + static_cast<std::ptrdiff_t>(i));
                    changed = true;
                    break;
                }
            }
            if (!entry.skills.empty())
            {
                if (ui->small_button("고른 스킬 모두 빼기"))
                {
                    entry.skills.clear();
                    changed = true;
                }
            }
            // The groups this role uses first.
            std::vector<int> groups;
            if (scout)
                groups = {urban::skills::kScout};
            else if (role == 0)
                groups = {urban::skills::kManager, urban::skills::kCoach, urban::skills::kPlayer, urban::skills::kWinningShot};
            else
                groups = {urban::skills::kCoach, urban::skills::kManager, urban::skills::kPlayer, urban::skills::kWinningShot};
            if (!ui->begin_tab_bar("staffskillgroups"))
                return;
            for (int group : groups)
            {
                if (!ui->begin_tab(urban::skills::groupName(group)))
                    continue;
                ui->begin_child("staffskilllist", 0.0f, 150.0f);
                int column = 0;
                for (const urban::skills::Skill *skill = urban::skills::begin(); skill != urban::skills::end(); ++skill)
                {
                    if (skill->group != group)
                        continue;
                    const auto at = std::find(entry.skills.begin(), entry.skills.end(), skill->id);
                    int on = at != entry.skills.end() ? 1 : 0;
                    ui->push_id(600000 + group * 1000 + skill->id);
                    if (column++ % 2 != 0)
                        ui->same_line();
                    ui->begin_disabled(!on && entry.skills.size() >= slots ? 1 : 0);
                    if (ui->checkbox(urban::skills::label(g_rdram, *skill).c_str(), &on))
                    {
                        if (on)
                            entry.skills.push_back(skill->id);
                        else
                            entry.skills.erase(at);
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

        void drawRole(const Pcsx2GsUi *ui, int team, int role, const View &view, Entry &entry, bool &changed,
                      std::vector<Action> &actions)
        {
            if (view.record == 0u)
            {
                ui->text_disabled("이 자리의 스태프를 찾지 못했습니다 (게임을 시작하면 나옵니다).");
                return;
            }
            ui->text((view.name + "  (ID " + std::to_string(view.id) + ")").c_str());
            ui->same_line();
            ui->text_disabled(("지금 스킬: " + skillNames(view.skills, isScout(role))).c_str());
            int lock = entry.lock ? 1 : 0;
            if (ui->checkbox("고정 (매 프레임 유지)", &lock))
            {
                entry.lock = lock != 0;
                changed = true;
            }
            ui->same_line();
            if (ui->small_button("한 번 적용"))
                actions.push_back({team, role, false});
            ui->same_line();
            if (ui->small_button("원래대로 (이번 실행)"))
            {
                entry.lock = false;
                changed = true;
                actions.push_back({team, role, true});
            }
            if (role == 0)
            {
                ui->separator_text("성격");
                int temper = entry.temper + 1;
                ui->set_next_width(220.0f);
                if (ui->combo("기질##temper", &temper, kTempers, 4))
                {
                    entry.temper = temper - 1;
                    changed = true;
                }
                ui->same_line();
                ui->text_disabled((std::string("지금 ") + kTempers[1 + (view.temper <= 3 ? 0 : view.temper <= 6 ? 1 : 2)]).c_str());
                int style = entry.style + 1;
                ui->set_next_width(220.0f);
                if (ui->combo("용병##style", &style, kStyles, 4))
                {
                    entry.style = style - 1;
                    changed = true;
                }
                ui->same_line();
                ui->text_disabled((std::string("지금 ") + kStyles[1 + (view.style <= 3 ? 0 : view.style <= 6 ? 1 : 2)]).c_str());
            }
            else if (!isScout(role))
            {
                ui->separator_text("지도 능력 (D~S)");
                if (ui->small_button("모두 S"))
                {
                    for (int i = 0; i < kAbilityCount; ++i)
                        if (kAbilityTable[i].role == role)
                            entry.abilities[static_cast<size_t>(i)] = 4;
                    changed = true;
                }
                ui->same_line();
                if (ui->small_button("모두 그대로"))
                {
                    for (int i = 0; i < kAbilityCount; ++i)
                        if (kAbilityTable[i].role == role)
                            entry.abilities[static_cast<size_t>(i)] = -1;
                    changed = true;
                }
                int column = 0;
                for (int i = 0; i < kAbilityCount; ++i)
                {
                    if (kAbilityTable[i].role != role)
                        continue;
                    ui->push_id(700000 + i);
                    if (column++ % 2 != 0)
                        ui->same_line();
                    const int now = view.abilities[static_cast<size_t>(i)];
                    char line[96];
                    std::snprintf(line, sizeof(line), "%s (지금 %s)", kAbilityTable[i].ko, now >= 0 && now <= 4 ? kRankNames[now + 1] : "-");
                    ui->text(line);
                    ui->same_line();
                    int value = entry.abilities[static_cast<size_t>(i)] + 1;
                    ui->set_next_width(80.0f);
                    if (ui->combo("##rank", &value, kRankNames, 6))
                    {
                        entry.abilities[static_cast<size_t>(i)] = static_cast<int8_t>(value - 1);
                        changed = true;
                    }
                    ui->pop_id();
                }
            }
            ui->separator_text(isScout(role) ? "스카우트 스킬" : role == 0 ? "스킬 (게임 화면에는 앞 3칸)" : "스킬 (게임 화면에는 앞 5칸)");
            drawSkills(ui, role, entry, changed);
        }

        // GS thread, while the menu is open. Works on a copy; a change is stored and saved at the end, one-off writes and
        // restores are done by the next frame.
        void draw(const Pcsx2GsUi *ui)
        {
            if (g_safeMode)
            {
                ui->text_wrapped("안전 모드로 시작해서 스태프 치트가 꺼져 있습니다.");
                return;
            }
            Settings settings;
            int own;
            std::array<std::array<View, kRoles>, kTeams> views;
            std::string profile;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                settings = g_settings;
                own = g_own;
                views = g_views;
                profile = g_profile;
            }
            ui->text_disabled(("세이브: " + urban::saveid::describe(profile) + " — 이 세이브의 스태프 설정").c_str());
            bool changed = false;
            std::vector<Action> actions;
            int locked = 0;
            for (const auto &[team, entries] : settings.teams)
                for (const Entry &entry : entries)
                    locked += entry.lock ? 1 : 0;

            int enabled = settings.enabled ? 1 : 0;
            if (ui->checkbox("스태프 치트 켜기 (고정한 항목을 씀)", &enabled))
            {
                settings.enabled = enabled != 0;
                changed = true;
            }
            ui->same_line();
            const std::string status = (own >= 0 ? std::string("내 구단 ") + kTeamNames[own] + " (자동 감지)" : std::string("내 구단을 찾는 중")) +
                                       " · 고정 " + std::to_string(locked) + "명";
            ui->text_disabled(status.c_str());
            std::vector<std::string> labels{std::string("내 구단 (자동") + (own >= 0 ? std::string(": ") + kTeamNames[own] : std::string()) + ")"};
            for (int t = 0; t < kTeams; ++t)
                labels.push_back(kTeamNames[t]);
            std::vector<const char *> pointers;
            for (const std::string &label : labels)
                pointers.push_back(label.c_str());
            int current = g_teamKey + 1;
            ui->set_next_width(260.0f);
            if (ui->combo("구단##staffteam", &current, pointers.data(), static_cast<int>(pointers.size())))
                g_teamKey = current - 1;
            ui->same_line();
            ui->text_disabled("'한 번 적용'은 스위치와 관계없이 바로 씁니다");
            const int team = g_teamKey < 0 ? own : g_teamKey;
            if (team < 0)
                ui->text_disabled("세이브를 불러오거나 새 게임을 시작하면 스태프가 나옵니다.");
            else
            {
                std::array<Entry, kRoles> &entries = settings.teams[g_teamKey];
                if (ui->begin_tab_bar("staffroles"))
                {
                    for (int role = 0; role < kRoles; ++role)
                    {
                        if (!ui->begin_tab(kRoleNames[role]))
                            continue;
                        drawRole(ui, g_teamKey, role, views[static_cast<size_t>(team)][static_cast<size_t>(role)],
                                 entries[static_cast<size_t>(role)], changed, actions);
                        ui->end_tab();
                    }
                    ui->end_tab_bar();
                }
            }
            if (!changed && actions.empty())
                return;
            // Keep only the teams with something set.
            for (auto it = settings.teams.begin(); it != settings.teams.end();)
                it = std::all_of(it->second.begin(), it->second.end(), [](const Entry &entry) { return entry.idle(); })
                         ? settings.teams.erase(it)
                         : std::next(it);
            std::filesystem::path path;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_settings = settings;
                path = g_settingsPath;
                g_actions.insert(g_actions.end(), actions.begin(), actions.end());
                if (!actions.empty())
                    for (const Action &action : actions)
                        if (!action.restore && g_settings.teams.find(action.team) == g_settings.teams.end())
                            g_settings.teams[action.team]; // an idle entry: the write finds nothing to do
            }
            if (changed)
                saveSettings(settings, path);
        }
#endif
    }

    void install(uint8_t *rdram, const std::filesystem::path &userDir)
    {
        if (!rdram)
            return;
        g_rdram = rdram;
        const char *mods = std::getenv("URBAN_MODS");
        g_safeMode = mods && std::strcmp(mods, ";") == 0; // the launcher's safe start
        (void)userDir; // the settings are per save: cheat_saves/<save>/staff_cheats.ini, read when a game is there (frame)
        size_t entries = 0;
        for (const auto &[team, list] : g_settings.teams)
            for (const Entry &entry : list)
                entries += entry.idle() ? 0u : 1u;
        std::fprintf(stderr, "[staff] staff cheats %s, %zu entries%s\n", g_settings.enabled ? "on" : "off", entries,
                     g_safeMode ? "; safe start: off" : "");
#if defined(URBAN_STAFF_MENU)
        ps2x::menu::addPanel("스태프 치트", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
        if (!g_safeMode)
            ps2x::pnach::addFrameHook(frame);
    }
}
