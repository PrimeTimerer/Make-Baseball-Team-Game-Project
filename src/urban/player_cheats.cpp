#include "urban/player_cheats.h"
#include "urban/change_log.h"
#include "urban/function_hooks.h"
#include "urban/save_identity.h"
#include "urban/skill_table.h"
#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_save_state.h"

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
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
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

        // The abilities the editor sets by value: 6-byte entries [kind][aptitude 得意度 0-4][value][limit 上限][u16 exp] in
        // the ability block (own-record offsets; a CPU record's block is elsewhere, at block + offset - 0x6C). The game's
        // values are 0-99 (S+ cheats write up to 255; the match copy clamps to 99). 球速 0-99 = 111-162 km/h.
        enum StatGroup
        {
            kPitching = 0,
            kBatting = 1,
            kFielding = 2,
            kPitches = 3
        };
        struct Stat
        {
            const char *key, *label, *jp;
            uint32_t offset;
            int group;
        };
        constexpr Stat kStats[] = {
            {"stamina", "체력", "体力", 0x6Eu, kPitching},       {"speedP", "구속", "球速", 0xDEu, kPitching},
            {"stuff", "구위", "球威", 0xE4u, kPitching},         {"control", "제구", "制球力", 0xEAu, kPitching},
            {"mentalP", "정신력(투)", "精神力", 0xF0u, kPitching}, {"contactL", "교타 좌", "左巧打力", 0xF8u, kBatting},
            {"contactR", "교타 우", "右巧打力", 0xFEu, kBatting},  {"power", "장타", "長打力", 0x104u, kBatting},
            {"speed", "주력", "走力", 0x10Au, kBatting},          {"arm", "어깨", "肩力", 0x110u, kBatting},
            {"bunt", "번트", "バント", 0x116u, kBatting},         {"running", "주루", "走塁", 0x11Cu, kBatting},
            {"lead", "리드", "リード", 0x122u, kBatting},         {"eye", "선구안", "選球眼", 0x128u, kBatting},
            {"mentalB", "정신력(타)", "精神力", 0x12Eu, kBatting}, {"catching", "포구", "捕球", 0x134u, kBatting},
            {"throwing", "송구", "送球", 0x13Au, kBatting},       {"posP", "투수", "投手", 0x74u, kFielding},
            {"posC", "포수", "捕手", 0x7Au, kFielding},           {"pos1B", "1루수", "一塁手", 0x80u, kFielding},
            {"pos2B", "2루수", "二塁手", 0x86u, kFielding},       {"pos3B", "3루수", "三塁手", 0x8Cu, kFielding},
            {"posSS", "유격수", "遊撃手", 0x92u, kFielding},      {"posOF", "외야수", "外野手", 0x98u, kFielding},
            {"pitch1", "변화구 1", "変化球", 0xA2u, kPitches},    {"pitch2", "변화구 2", "変化球", 0xA8u, kPitches},
            {"pitch3", "변화구 3", "変化球", 0xAEu, kPitches},    {"pitch4", "변화구 4", "変化球", 0xB4u, kPitches},
            {"pitch5", "변화구 5", "変化球", 0xBAu, kPitches},    {"pitch6", "변화구 6", "変化球", 0xC0u, kPitches},
            {"pitch7", "변화구 7", "変化球", 0xC6u, kPitches},    {"pitch8", "변화구 8", "変化球", 0xCCu, kPitches},
            {"pitch9", "변화구 9", "変化球", 0xD2u, kPitches},    {"pitch10", "변화구 10", "変化球", 0xD8u, kPitches},
        };
        constexpr int kStatCount = static_cast<int>(sizeof(kStats) / sizeof(kStats[0]));
        enum StatIndex
        {
            sStamina = 0, sSpeedP, sStuff, sControl, sMentalP, sContactL, sContactR, sPower, sSpeed, sArm, sBunt, sRunning,
            sLead, sEye, sMentalB, sCatching, sThrowing, sPosP, sPosC, sPos1B, sPos2B, sPos3B, sPosSS, sPosOF, sPitch1
        };
        // Ranks by the owner's notes: 90 S+, 80 S, 70 A+, 60 A, 55 B+, 50 B, 45 C+, 40 C, 35 D+, below D. A rank picked in
        // the editor gives its highest value. 得意度 (growth aptitude) 4 S, 3 A, 2 B, 1 C, 0 D.
        constexpr int kGradeCount = 10;
        constexpr const char *kGrades[kGradeCount] = {"S+", "S", "A+", "A", "B+", "B", "C+", "C", "D+", "D"};
        constexpr int kGradeLow[kGradeCount] = {90, 80, 70, 60, 55, 50, 45, 40, 35, 0};
        constexpr int kGradeTop[kGradeCount] = {99, 89, 79, 69, 59, 54, 49, 44, 39, 34};
        constexpr const char *kAptitudes[5] = {"D", "C", "B", "A", "S"};
        // The highest value the editor writes. The game's own scale ends at 99 and its screens grade a value above that D
        // (the owner's screenshots of 11-07: 255 shown D, 99 S+, 89 S, whatever the experience), but values above 99 do
        // count: the owner had a 75-home-run season from 198 / 255 (11-07), so the editor writes up to 255, as before.
        constexpr int kValueMax = 255;
        int gradeOf(int value)
        {
            for (int g = 0; g < kGradeCount - 1; ++g)
                if (value >= kGradeLow[g])
                    return g;
            return kGradeCount - 1;
        }
        // An ability as an edit sets it: -1 leaves a field as it is; limit -2 raises the limit to the value when lower.
        struct StatEdit
        {
            int value = -1, limit = -1, apt = -1;
            bool operator==(const StatEdit &) const = default;
        };
        // A player's own edits: the locked abilities (written at every frame, over everything else).
        struct PlayerEdit
        {
            bool on = true;
            std::string name;
            std::map<int, StatEdit> stats; // by kStats index
        };

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
            bool on = true; // locked: the applied items, skills and slots are written at every frame
            // What the menu edits (a draft: nothing is written before 적용 및 고정, owner 11-06) ...
            uint32_t mask = 0u;
            std::vector<uint16_t> skills;
            // Slot by slot (the 8 skill slots): -1 as it is, 0 empty, else that skill; written after the added skills.
            std::array<int, kSkillSlots> slots{-1, -1, -1, -1, -1, -1, -1, -1};
            // ... and what 적용 및 고정 took from it, which the frame hook writes.
            uint32_t appliedMask = 0u;
            std::vector<uint16_t> appliedSkills;
            std::array<int, kSkillSlots> appliedSlots{-1, -1, -1, -1, -1, -1, -1, -1};
            std::string name;
            bool pending() const { return mask != appliedMask || skills != appliedSkills || slots != appliedSlots; }
        };
        enum Scope
        {
            kScopeOwn = 0,   // the own team's players (as before 2026-11-05)
            kScopeTeams = 1, // the players of the chosen teams
            kScopeAll = 2    // every team's players
        };
        // The default target's settings as 단체 적용 및 고정 took them (the menu edits a draft of them in Settings).
        struct GroupApplied
        {
            bool all = true;
            int scope = kScopeOwn;
            uint32_t teams = kOwnBit;
            uint32_t mask = 0u;
            std::vector<uint16_t> skills;
            std::map<int, StatEdit> stats;
            bool operator==(const GroupApplied &) const = default;
        };
        // A player queued for the camp's special practice through the game's own judgment (the camp hook below): type 0 a
        // new pitch (param: the pitch kind), type 1 switch hitter. Kept in this save's settings until the camp's last step.
        struct CampWish
        {
            uint16_t id = 0u;
            int type = 0, param = 0;
            std::string name;
        };
        struct Settings
        {
            int team = -1; // the own team when these settings were first used (another team: another game's, not used)
            GroupApplied applied;
            bool enabled = false;
            bool restore = true; // an item taken off gets the player's own bytes back (kept from this run's first write)
            // A player's own locks and items (고정, 따로 정한 선수) only while he is on the own team: one traded or
            // released to a CPU club is left alone (owner, 11-06). Files before it lack the key: on.
            bool ownOnly = true;
            bool all = true; // the default items for the players of the scope without a pin (off: only the picked players)
            // The default target's items, skills and ability locks are written only after the owner turns this on in
            // 단체 적용 (owner, 11-06: nothing may change in bulk before it is asked for; files before it lack the key: off).
            bool groupOn = false;
            int scope = kScopeOwn;
            uint32_t teams = kOwnBit; // kScopeTeams: bit t = team t, kOwnBit = the own team
            uint32_t mask = 0u;       // the default items (bit i = g_items[i])
            std::vector<uint16_t> skills; // the default skills, in slot order (at most 8)
            std::map<uint16_t, Pin> pins; // the picked players, by player ID, on any team
            std::map<uint16_t, PlayerEdit> edits; // players' locked abilities, by player ID (on top of items and pins)
            std::map<int, StatEdit> groupStats;   // locked abilities for the default target's players (kStats index)
            std::vector<CampWish> camp;           // the camp queue (one wish a player), changed under g_mutex only
        };
        struct Row
        {
            uint16_t id = 0u;
            int team = -1;
            int number = 0, age = 0;
            bool pitcher = false;
            std::string name;
            std::array<uint16_t, kSkillSlots> skills{}; // the slots as the record has them now
            std::array<uint8_t, kBlockSize> block{};    // the ability block as it is now
            int position = 0;                           // the best position: 0 P, 1 C, 2 1B, 3 2B, 4 3B, 5 SS, 6 OF
            int bats = 0;                               // record +0x39 bits 0-1: 0 right, 1 left, 2 both
            int value(int stat, int field = 2) const { return block[kStats[stat].offset - kBlock + static_cast<uint32_t>(field)]; }
        };
        using Slots = std::array<uint16_t, kSkillSlots>;
        // A player whose skills the cheat changed in this run: what the slots held before and what it wrote last.
        struct SkillBackup
        {
            Slots before{}, written{};
        };

        uint8_t *g_rdram = nullptr;
        std::vector<Item> g_items; // read once at start
        std::filesystem::path g_itemsPath, g_settingsPath; // g_settingsPath: this save's file (empty before a game)
        std::filesystem::path g_legacySettingsPath;        // <userDir>/player_cheats.ini from before settings were per save
        std::string g_profile;                             // the save (urban::saveid)
        uint32_t g_profileGen = ~0u;
        bool g_teamMismatch = false; // the settings were made for another team: not used
        bool g_safeMode = false;
        std::mutex g_mutex; // the settings and the rosters: the menu (GS thread) and the frame hook (EE thread)
        Settings g_settings;
        int g_team = -1;
        std::array<std::vector<Row>, kTeams> g_rows;
        std::map<uint16_t, SkillBackup> g_skillBackups; // EE thread only, by player ID
        // A player whose items the cheat wrote in this run: where, and per item the bytes there before the first write.
        struct StatBackup
        {
            uint32_t record = 0u, block = 0u;
            std::map<size_t, std::vector<std::pair<uint32_t, uint8_t>>> items;
        };
        std::map<uint16_t, StatBackup> g_statBackups; // EE thread only, by player ID
        // A player whose abilities a lock wrote in this run: per ability the aptitude, value, limit and experience (u16)
        // before the first write.
        struct LockBackup
        {
            uint32_t record = 0u, block = 0u;
            std::map<int, std::array<uint8_t, 5>> before;
        };
        std::map<uint16_t, LockBackup> g_lockBackups; // EE thread only, by player ID
        // One-time edits from the menu (under g_mutex), written by the frame hook where it finds the player.
        struct OneShot
        {
            uint16_t id = 0u;
            int stat = 0;
            StatEdit edit;
            int age = 0;
        };
        std::vector<OneShot> g_oneShots;
        // Camp practice at once, for any number of players (owner 11-06: the camp's special practice takes 6): a new
        // pitch into the first empty pitch slot, or the batting side. The batting side is record +0x39, bits 0-1 (0 right,
        // 1 left, 2 both; own and CPU records alike, matched for all 480 players of the 11-06 camp save against their
        // database records' u16 @0x2A >> 1). Pitch kinds as the game's list at 0x6157D0: 2 チェンジアップ ... 19 高速スライダー.
        struct Practice
        {
            uint16_t id = 0u;
            int pitch = 0; // > 0: the pitch kind to add, with value / limit / aptitude
            int value = 50, limit = 99, apt = 2;
            int bats = -1; // >= 0: the batting side to set
            int age = 0;
        };
        std::vector<Practice> g_practices;
        constexpr uint32_t kBats = 0x39u;
        constexpr uint32_t kExpTable = 0x5D1320u; // u16 experience per ability value 0-100 (the game's data)
        constexpr int kPitchKinds = 20;
        constexpr const char *kPitchNames[kPitchKinds] = {"없음",    "직구",   "체인지업", "슬라이더", "슈트",      "커브",      "슬로 커브",
                                                          "싱커",    "스크루", "포크",     "SFF",      "종슬라이더", "팜볼",      "너클",
                                                          "컷볼",    "고속 슈트", "투심",   "서클 체인지업", "드롭 커브", "고속 슬라이더"};
        constexpr const char *kBatsNames[3] = {"우타", "좌타", "양타 (스위치)"};
        const char *pitchName(int kind) { return kind >= 0 && kind < kPitchKinds ? kPitchNames[kind] : "?"; }
        std::filesystem::file_time_type g_settingsTime{}; // player_cheats.ini as last read or written (under g_mutex)
        // The change log (under g_mutex): each read of the 12 rosters against the one before.
        urban::ChangeLog g_log;
        std::map<uint16_t, std::pair<int, std::string>> g_seen; // the last read: player id -> team, name (empty: baseline next)
        int g_seenTeam = -1;
        uint32_t g_seenLoads = 0u; // save states loaded at that read
        std::atomic<bool> g_refreshNow{false};

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
                if (id.empty() || *end != '\0' || value > 0xFFFFu || !urban::skills::find(static_cast<uint16_t>(value), false))
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

        int statIndex(const std::string &key)
        {
            for (int i = 0; i < kStatCount; ++i)
                if (key == kStats[i].key)
                    return i;
            return -1;
        }

        // "<value>,<limit>,<aptitude>" (-1 = as it is, limit -2 = raised to the value when lower).
        StatEdit editOf(std::istringstream &fields)
        {
            StatEdit edit;
            std::string field;
            if (std::getline(fields, field, ','))
                edit.value = std::clamp(std::atoi(trim(field).c_str()), -1, kValueMax);
            if (std::getline(fields, field, ','))
                edit.limit = std::clamp(std::atoi(trim(field).c_str()), -2, kValueMax);
            if (std::getline(fields, field, ','))
                edit.apt = std::clamp(std::atoi(trim(field).c_str()), -1, 4);
            return edit;
        }

        // player_cheats.ini: enabled=, all=, scope=own|teams|all, teams=own+<team>..., items=key+key, skills=<hex>+<hex>,
        // pin=<player id>,<on>,<key+key>,<name>, pinskills=<player id>,<hex+hex>; since 2026-11-06 the value edits:
        // glock=<ability>,<value>,<limit>,<aptitude> (the default target), edit=<player id>,<on>,<name> and
        // lock=<player id>,<ability>,<value>,<limit>,<aptitude>. Files without scope= (before 2026-11-05) keep their
        // meaning: the own team.
        Settings loadSettings(const std::filesystem::path &path)
        {
            Settings settings;
            settings.mask = allItems();
            std::map<uint16_t, std::vector<uint16_t>> pinSkills;
            std::map<uint16_t, std::array<int, kSkillSlots>> pinSlots;
            struct AppliedPin
            {
                uint32_t mask = 0u;
                std::vector<uint16_t> skills;
                std::array<int, kSkillSlots> slots{-1, -1, -1, -1, -1, -1, -1, -1};
            };
            std::map<uint16_t, AppliedPin> appliedPins;
            std::ifstream file(path, std::ios::binary);
            std::string line;
            while (std::getline(file, line))
            {
                line = trim(line);
                const size_t equals = line.find('=');
                if (line.empty() || line[0] == '#' || equals == std::string::npos)
                    continue;
                const std::string key = trim(line.substr(0, equals)), value = trim(line.substr(equals + 1));
                const auto teamsOf = [&](const std::string &text) {
                    uint32_t teams = 0u;
                    std::istringstream list(text);
                    std::string team;
                    while (std::getline(list, team, '+'))
                    {
                        team = trim(team);
                        char *end = nullptr;
                        const unsigned long t = std::strtoul(team.c_str(), &end, 10);
                        if (team == "own")
                            teams |= kOwnBit;
                        else if (!team.empty() && *end == '\0' && t < static_cast<unsigned long>(kTeams))
                            teams |= 1u << t;
                    }
                    return teams;
                };
                const auto scopeOf = [](const std::string &text) { return text == "all" ? kScopeAll : text == "teams" ? kScopeTeams : kScopeOwn; };
                if (key == "enabled")
                    settings.enabled = value == "1";
                else if (key == "team")
                    settings.team = std::atoi(value.c_str());
                else if (key == "all")
                    settings.all = value == "1";
                else if (key == "groupon")
                    settings.groupOn = value == "1";
                else if (key == "ownonly")
                    settings.ownOnly = value != "0";
                else if (key == "camp") // id,type,param,name
                {
                    std::istringstream fields(value);
                    std::string field;
                    int f[3] = {0, -1, 0};
                    for (int k = 0; k < 3 && std::getline(fields, field, ','); ++k)
                        f[k] = std::atoi(trim(field).c_str());
                    CampWish wish;
                    wish.id = static_cast<uint16_t>(std::clamp(f[0], 0, 0xFFFF));
                    wish.type = f[1];
                    wish.param = f[2];
                    std::getline(fields, wish.name);
                    if (wish.id != 0u && (wish.type == 1 || (wish.type == 0 && wish.param > 1 && wish.param < kPitchKinds)) &&
                        std::none_of(settings.camp.begin(), settings.camp.end(), [&](const CampWish &w) { return w.id == wish.id; }))
                        settings.camp.push_back(wish);
                }
                else if (key == "restore")
                    settings.restore = value != "0";
                else if (key == "scope")
                    settings.scope = scopeOf(value);
                else if (key == "teams")
                    settings.teams = teamsOf(value);
                // What 단체 적용 및 고정 took (a_*): the frame hook writes these, not the draft above.
                else if (key == "a_all")
                    settings.applied.all = value == "1";
                else if (key == "a_scope")
                    settings.applied.scope = scopeOf(value);
                else if (key == "a_teams")
                    settings.applied.teams = teamsOf(value);
                else if (key == "a_items")
                    settings.applied.mask = maskOf(value);
                else if (key == "a_skills")
                    settings.applied.skills = skillsOf(value);
                else if (key == "a_glock")
                {
                    std::istringstream fields(value);
                    std::string stat;
                    std::getline(fields, stat, ',');
                    const int index = statIndex(trim(stat));
                    if (index >= 0)
                        settings.applied.stats[index] = editOf(fields);
                }
                else if (key == "apin" || key == "apinskills" || key == "apinslots")
                {
                    std::istringstream fields(value);
                    std::string id, rest;
                    if (!std::getline(fields, id, ','))
                        continue;
                    const unsigned long number = std::strtoul(trim(id).c_str(), nullptr, 10);
                    if (number == 0u || number > 0xFFFFu)
                        continue;
                    AppliedPin &applied = appliedPins[static_cast<uint16_t>(number)];
                    if (key == "apin")
                    {
                        std::getline(fields, rest);
                        applied.mask = maskOf(rest);
                    }
                    else if (key == "apinskills")
                    {
                        std::getline(fields, rest);
                        applied.skills = skillsOf(rest);
                    }
                    else
                    {
                        std::string slot;
                        for (int k = 0; k < kSkillSlots && std::getline(fields, slot, ','); ++k)
                        {
                            const int skill = std::atoi(trim(slot).c_str());
                            applied.slots[static_cast<size_t>(k)] =
                                skill <= 0 || urban::skills::find(static_cast<uint16_t>(skill), false) ? std::max(skill, -1) : -1;
                        }
                    }
                }
                else if (key == "items")
                    settings.mask = maskOf(value);
                else if (key == "skills")
                    settings.skills = skillsOf(value);
                else if (key == "glock")
                {
                    std::istringstream fields(value);
                    std::string stat;
                    std::getline(fields, stat, ',');
                    const int index = statIndex(trim(stat));
                    if (index >= 0)
                        settings.groupStats[index] = editOf(fields);
                }
                else if (key == "edit" || key == "lock")
                {
                    std::istringstream fields(value);
                    std::string id;
                    if (!std::getline(fields, id, ','))
                        continue;
                    const unsigned long number = std::strtoul(trim(id).c_str(), nullptr, 10);
                    if (number == 0u || number > 0xFFFFu)
                        continue;
                    PlayerEdit &edit = settings.edits[static_cast<uint16_t>(number)];
                    if (key == "edit")
                    {
                        std::string on, name;
                        std::getline(fields, on, ',');
                        std::getline(fields, name);
                        edit.on = trim(on) != "0";
                        edit.name = trim(name);
                        continue;
                    }
                    std::string stat;
                    std::getline(fields, stat, ',');
                    const int index = statIndex(trim(stat));
                    if (index >= 0)
                        edit.stats[index] = editOf(fields);
                }
                else if (key == "pin" || key == "pinskills" || key == "pinslots")
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
                    if (key == "pinslots")
                    {
                        std::array<int, kSkillSlots> slots{-1, -1, -1, -1, -1, -1, -1, -1};
                        std::string slot;
                        for (int k = 0; k < kSkillSlots && std::getline(fields, slot, ','); ++k)
                        {
                            const int skill = std::atoi(trim(slot).c_str());
                            slots[static_cast<size_t>(k)] = skill <= 0 || urban::skills::find(static_cast<uint16_t>(skill), false) ? std::max(skill, -1) : -1;
                        }
                        pinSlots[static_cast<uint16_t>(number)] = slots;
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
            for (const auto &[id, slots] : pinSlots)
                if (auto pin = settings.pins.find(id); pin != settings.pins.end())
                    pin->second.slots = slots;
            for (const auto &[id, applied] : appliedPins)
                if (auto pin = settings.pins.find(id); pin != settings.pins.end())
                {
                    pin->second.appliedMask = applied.mask;
                    pin->second.appliedSkills = applied.skills;
                    pin->second.appliedSlots = applied.slots;
                }
            return settings;
        }

        void saveSettings(const Settings &settings)
        {
            if (g_settingsPath.empty())
                return; // no game yet: nothing to keep
            std::error_code made;
            std::filesystem::create_directories(g_settingsPath.parent_path(), made);
            std::ofstream file(g_settingsPath, std::ios::binary | std::ios::trunc);
            const auto scopeText = [](int scope) { return scope == kScopeAll ? "all" : scope == kScopeTeams ? "teams" : "own"; };
            file << "# 선수 치트 설정: 게임 메뉴(F2)의 \"선수 치트\" 탭이 씁니다 (이 세이브의 설정)\n"
                 << "team=" << settings.team << "\n"
                 << "a_all=" << (settings.applied.all ? 1 : 0) << "\n"
                 << "a_scope=" << scopeText(settings.applied.scope) << "\n"
                 << "a_teams=" << teamsText(settings.applied.teams) << "\n"
                 << "a_items=" << keysOf(settings.applied.mask) << "\n"
                 << "a_skills=" << skillText(settings.applied.skills) << "\n";
            for (const auto &[stat, edit] : settings.applied.stats)
                file << "a_glock=" << kStats[stat].key << "," << edit.value << "," << edit.limit << "," << edit.apt << "\n";
            for (const auto &[id, pin] : settings.pins)
            {
                file << "apin=" << id << "," << keysOf(pin.appliedMask) << "\n";
                if (!pin.appliedSkills.empty())
                    file << "apinskills=" << id << "," << skillText(pin.appliedSkills) << "\n";
                if (std::any_of(pin.appliedSlots.begin(), pin.appliedSlots.end(), [](int s) { return s >= 0; }))
                {
                    file << "apinslots=" << id;
                    for (int s : pin.appliedSlots)
                        file << "," << s;
                    file << "\n";
                }
            }
            file << "enabled=" << (settings.enabled ? 1 : 0) << "\n"
                 << "all=" << (settings.all ? 1 : 0) << "\n"
                 << "groupon=" << (settings.groupOn ? 1 : 0) << "\n"
                 << "scope=" << (settings.scope == kScopeAll ? "all" : settings.scope == kScopeTeams ? "teams" : "own") << "\n"
                 << "teams=" << teamsText(settings.teams) << "\n"
                 << "items=" << keysOf(settings.mask) << "\n"
                 << "skills=" << skillText(settings.skills) << "\n"
                 << "restore=" << (settings.restore ? 1 : 0) << "\n"
                 << "ownonly=" << (settings.ownOnly ? 1 : 0) << "\n";
            for (const CampWish &wish : settings.camp)
                file << "camp=" << wish.id << "," << wish.type << "," << wish.param << "," << wish.name << "\n";
            for (const auto &[id, pin] : settings.pins)
            {
                file << "pin=" << id << "," << (pin.on ? 1 : 0) << "," << keysOf(pin.mask) << "," << pin.name << "\n";
                if (!pin.skills.empty())
                    file << "pinskills=" << id << "," << skillText(pin.skills) << "\n";
                if (std::any_of(pin.slots.begin(), pin.slots.end(), [](int s) { return s >= 0; }))
                {
                    file << "pinslots=" << id;
                    for (int s : pin.slots)
                        file << "," << s;
                    file << "\n";
                }
            }
            for (const auto &[stat, edit] : settings.groupStats)
                file << "glock=" << kStats[stat].key << "," << edit.value << "," << edit.limit << "," << edit.apt << "\n";
            for (const auto &[id, edit] : settings.edits)
            {
                file << "edit=" << id << "," << (edit.on ? 1 : 0) << "," << edit.name << "\n";
                for (const auto &[stat, value] : edit.stats)
                    file << "lock=" << id << "," << kStats[stat].key << "," << value.value << "," << value.limit << "," << value.apt << "\n";
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

        // Whether a target (all, scope, teams) reaches team t's players without a pin.
        bool inScopeOf(bool all, int scope, uint32_t teams, int t, int own)
        {
            if (!all)
                return false;
            if (scope == kScopeAll)
                return true;
            if (scope == kScopeTeams)
                return ((teams >> t) & 1u) != 0u || ((teams & kOwnBit) != 0u && t == own);
            return t == own;
        }
        // The draft target (what the 단체 적용 tab shows).
        bool inScope(const Settings &settings, int t, int own) { return inScopeOf(settings.all, settings.scope, settings.teams, t, own); }
        // Whether the frame hook writes the group settings to team t's players now: only what 단체 적용 및 고정 took,
        // while it is on.
        bool groupApplies(const Settings &settings, int t, int own)
        {
            return settings.groupOn && inScopeOf(settings.applied.all, settings.applied.scope, settings.applied.teams, t, own);
        }
        // The draft of the group settings (what 단체 적용 및 고정 would take).
        GroupApplied groupDraft(const Settings &settings)
        {
            GroupApplied draft;
            draft.all = settings.all;
            draft.scope = settings.scope;
            draft.teams = settings.teams;
            draft.mask = settings.mask;
            draft.skills = settings.skills;
            draft.stats = settings.groupStats;
            return draft;
        }

        // Where an item's run goes in a record whose ability block is `block` (own team: record + 0x6C); 0 when a CPU
        // record has nothing there (past the block).
        uint32_t runAddress(uint32_t record, uint32_t block, bool own, const Run &run)
        {
            const uint32_t end = run.offset + static_cast<uint32_t>(run.bytes.size());
            if (end <= kBlock)
                return record + run.offset;
            if (run.offset >= kBlock && end <= kBlock + kBlockSize)
                return block + (run.offset - kBlock);
            return own ? record + run.offset : 0u;
        }

        void restoreBytes(const std::vector<std::pair<uint32_t, uint8_t>> &bytes)
        {
            for (const auto &[address, value] : bytes)
                g_rdram[address] = value;
        }

        // The chosen items into a player's record. The bytes each item overwrites are kept the first time (this run), and
        // an item no longer chosen (unticked, the player out of the target, the cheat off) gets them back when
        // Settings::restore is on (else they stay, as before 2026-11-05). A player found at another record (a trade,
        // another save) starts over: the old bytes belong to that other place.
        void applyItems(uint32_t record, uint32_t block, bool own, uint16_t id, uint32_t mask)
        {
            auto backup = g_statBackups.find(id);
            if (backup != g_statBackups.end() && (backup->second.record != record || backup->second.block != block))
            {
                g_statBackups.erase(backup);
                backup = g_statBackups.end();
            }
            if (backup != g_statBackups.end())
                for (auto it = backup->second.items.begin(); it != backup->second.items.end();)
                {
                    if (mask & (1u << it->first))
                    {
                        ++it;
                        continue;
                    }
                    if (g_settings.restore)
                        restoreBytes(it->second);
                    it = backup->second.items.erase(it);
                }
            if (mask == 0u)
            {
                if (backup != g_statBackups.end())
                    g_statBackups.erase(backup);
                return;
            }
            if (backup == g_statBackups.end())
            {
                StatBackup fresh;
                fresh.record = record;
                fresh.block = block;
                backup = g_statBackups.emplace(id, std::move(fresh)).first;
            }
            for (size_t i = 0; i < g_items.size(); ++i)
            {
                if (!(mask & (1u << i)))
                    continue;
                const bool first = backup->second.items.find(i) == backup->second.items.end();
                std::vector<std::pair<uint32_t, uint8_t>> &kept = backup->second.items[i];
                for (const Run &run : g_items[i].runs)
                {
                    const uint32_t at = runAddress(record, block, own, run);
                    if (at == 0u)
                        continue;
                    if (first)
                        for (uint32_t b = 0; b < run.bytes.size(); ++b)
                            kept.emplace_back(at + b, g_rdram[at + b]);
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
        void applySkills(uint32_t record, uint16_t id, const std::vector<uint16_t> *chosen,
                         const std::array<int, kSkillSlots> *slots = nullptr)
        {
            auto backup = g_skillBackups.find(id);
            const bool bySlot = slots && std::any_of(slots->begin(), slots->end(), [](int s) { return s >= 0; });
            if ((!chosen || chosen->empty()) && !bySlot)
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
            for (uint16_t skill : chosen ? *chosen : std::vector<uint16_t>{})
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
            if (bySlot)
                for (size_t k = 0; k < want.size(); ++k)
                    if ((*slots)[k] >= 0)
                        want[k] = static_cast<uint16_t>((*slots)[k]); // the slot as picked (0: empty)
            if (want != now)
                writeSkills(record, want);
            backup->second.written = want;
        }

        // The byte at `address` before any item of this run wrote it (else as it is now).
        uint8_t originalByte(uint16_t id, uint32_t address)
        {
            const auto backup = g_statBackups.find(id);
            if (backup != g_statBackups.end())
                for (const auto &[item, kept] : backup->second.items)
                    for (const auto &[at, value] : kept)
                        if (at == address)
                            return value;
            return g_rdram[address];
        }

        // A lock taken off under an item still on: the item gives back what was there before the lock when it goes off.
        void handBack(uint16_t id, uint32_t address, uint8_t value)
        {
            const auto backup = g_statBackups.find(id);
            if (backup != g_statBackups.end())
                for (auto &[item, kept] : backup->second.items)
                    for (auto &[at, old] : kept)
                        if (at == address)
                            old = value;
        }

        // The match copy takes every ability from its experience, not from the value byte: sub_00188178 (the match copy
        // builder) stores sub_00180238(entry), the value the u16 experience at entry + 4 reaches in the table at 0x5D1320,
        // capped at 99 (REA / rizin, 2026-11-07). The value byte is what the game's screens show. So a value is written
        // with the experience for it, as the S+ codes do (their bytes end in the experience). table[99] at most: the table
        // ends with table[100] = 65535, and sub_00180238 returns 3 for an experience no entry is above (a value of 100 or
        // more once gave a 3 in matches). Above 99 the value byte itself counts only with the 99 제한 tab's switch.
        void writeStat(uint32_t entry, const StatEdit &edit)
        {
            if (edit.apt >= 0)
                g_rdram[entry + 1u] = static_cast<uint8_t>(std::min(edit.apt, 4));
            if (edit.value >= 0)
            {
                g_rdram[entry + 2u] = static_cast<uint8_t>(std::min(edit.value, kValueMax));
                const uint16_t exp = read16(kExpTable + 2u * static_cast<uint32_t>(std::min(edit.value, 99)));
                g_rdram[entry + 4u] = static_cast<uint8_t>(exp);
                g_rdram[entry + 5u] = static_cast<uint8_t>(exp >> 8);
            }
            if (edit.limit >= 0)
                g_rdram[entry + 3u] = static_cast<uint8_t>(std::min(edit.limit, kValueMax));
            else if (edit.limit == -2 && edit.value >= 0 && g_rdram[entry + 3u] < std::min(edit.value, kValueMax))
                g_rdram[entry + 3u] = static_cast<uint8_t>(std::min(edit.value, kValueMax));
        }

        // The locked abilities: the player's own (`own`), else the default target's (`group`); after the items, so they
        // win. One no longer locked gets its bytes from before the first write back when Settings::restore is on.
        void applyLocks(uint32_t record, uint32_t block, uint16_t id, const std::map<int, StatEdit> *group,
                        const std::map<int, StatEdit> *own)
        {
            auto backup = g_lockBackups.find(id);
            if (backup != g_lockBackups.end() && (backup->second.record != record || backup->second.block != block))
            {
                g_lockBackups.erase(backup); // another place now (a trade, another save): the old bytes belong there
                backup = g_lockBackups.end();
            }
            const auto held = [&](int stat) { return (own && own->count(stat) != 0u) || (group && group->count(stat) != 0u); };
            if (backup != g_lockBackups.end())
            {
                for (auto it = backup->second.before.begin(); it != backup->second.before.end();)
                {
                    if (held(it->first))
                    {
                        ++it;
                        continue;
                    }
                    if (g_settings.restore)
                    {
                        const uint32_t entry = block + kStats[it->first].offset - kBlock;
                        for (uint32_t b = 0; b < 5u; ++b)
                        {
                            g_rdram[entry + 1u + b] = it->second[b];
                            handBack(id, entry + 1u + b, it->second[b]);
                        }
                    }
                    it = backup->second.before.erase(it);
                }
                if (backup->second.before.empty() && !(own && !own->empty()) && !(group && !group->empty()))
                {
                    g_lockBackups.erase(backup);
                    return;
                }
            }
            if ((!own || own->empty()) && (!group || group->empty()))
                return;
            if (backup == g_lockBackups.end())
                backup = g_lockBackups.emplace(id, LockBackup{record, block, {}}).first;
            const auto write = [&](int stat, const StatEdit &edit)
            {
                const uint32_t entry = block + kStats[stat].offset - kBlock;
                if (backup->second.before.find(stat) == backup->second.before.end())
                    backup->second.before[stat] = {originalByte(id, entry + 1u), originalByte(id, entry + 2u), originalByte(id, entry + 3u),
                                                   originalByte(id, entry + 4u), originalByte(id, entry + 5u)};
                writeStat(entry, edit);
            };
            if (group)
                for (const auto &[stat, edit] : *group)
                    if (!own || own->count(stat) == 0u)
                        write(stat, edit);
            if (own)
                for (const auto &[stat, edit] : *own)
                    write(stat, edit);
        }

        std::string nameOfId(uint16_t id)
        {
            for (const std::vector<Row> &rows : g_rows)
                for (const Row &row : rows)
                    if (row.id == id)
                        return row.name;
            const auto seen = g_seen.find(id);
            return seen == g_seen.end() ? "선수 " + std::to_string(id) : seen->second.second;
        }

        // The change log: this read of the 12 rosters against the one before (g_mutex held). After a save state is loaded
        // or another save is opened, the read is the new baseline instead.
        void logRosterChanges(int team, bool manual)
        {
            if (team < 0)
            {
                g_seen.clear();
                return;
            }
            g_log.read(g_rdram);
            std::map<uint16_t, std::pair<int, std::string>> now;
            size_t all = 0u, own = 0u;
            for (int t = 0; t < kTeams; ++t)
                for (const Row &row : g_rows[static_cast<size_t>(t)])
                {
                    now[row.id] = {t, row.name.empty() ? std::string("?") : row.name};
                    ++all;
                    own += t == team ? 1u : 0u;
                }
            const uint32_t loads = ps2x::savestate::loadCount();
            int added = 0;
            const auto add = [&](const std::string &text) {
                g_log.add(text, "players");
                ++added;
            };
            const std::string counts = "12개 구단 " + std::to_string(all) + "명 · 내 구단 " + std::to_string(own) + "명";
            if (g_seen.empty() || team != g_seenTeam || loads != g_seenLoads)
                add(std::string(loads != g_seenLoads && !g_seen.empty() && team == g_seenTeam ? "상태를 불러와서 다시 읽음: " : "선수 목록 읽음: ") +
                    counts);
            else
            {
                std::vector<std::string> cpuIn, cpuOut;
                for (const auto &[id, at] : now)
                {
                    const auto old = g_seen.find(id);
                    const int was = old == g_seen.end() ? -1 : old->second.first;
                    if (was == at.first)
                        continue;
                    if (at.first == team)
                        add("우리 구단 입단: " + at.second + (was >= 0 ? std::string(" (") + kTeamNames[was] + "에서)" : std::string()));
                    else if (was == team)
                        add("우리 구단에서 " + std::string(kTeamNames[at.first]) + "(으)로: " + at.second);
                    else
                        cpuIn.push_back(at.second + " → " + kTeamNames[at.first]);
                }
                for (const auto &[id, was] : g_seen)
                    if (!now.count(id))
                    {
                        if (was.first == team)
                            add("우리 구단에서 빠짐: " + was.second);
                        else
                            cpuOut.push_back(was.second + " (" + kTeamNames[was.first] + ")");
                    }
                if (!cpuIn.empty())
                    add("CPU 구단 입단 · 이적 " + std::to_string(cpuIn.size()) + "명: " + urban::joinNames(cpuIn));
                if (!cpuOut.empty())
                    add("CPU 구단에서 빠짐 " + std::to_string(cpuOut.size()) + "명: " + urban::joinNames(cpuOut));
                if (manual && added == 0)
                    add("새로고침: 바뀐 것 없음 (" + counts + ")");
            }
            g_seen = std::move(now);
            g_seenTeam = team;
            g_seenLoads = loads;
        }

        // The camp's special practice through the game's own judgment, for any number of players (owner 11-06: "더 찾아봐").
        // Each camp step (sub_001E0948, from the calendar) packs the 6 places of the special-practice screen (camp+0xECC)
        // into the camp record [camp+0xF18] (+0xC4 count, +0xC6 entries [roster slot][type][param][result]) and runs
        // sub_00152E58; at the last step (+0xC2 == 5) that calls sub_001555F0, which rolls each place (slot > 0):
        //   type 0 新球種: sub_00155D18(rec, slot, kind) is 0 when the roll wins (1 not eligible; 2, 3, 4 lost at a chance
        //     of <= 10 %, <= 39 %, more), then sub_00155E58 writes the pitch (its value and aptitude rolled by the game);
        //   type 1 スイッチ: sub_001565A8(rec, slot) is 0 when it wins (1 not a right-handed batter, 2 +0x10C < 55 or
        //     +0x11D * 10 + +0x11E < 70, 3 the 80 % roll lost), then sub_00156690 sets +0x39 = 2 and lifts 3 batting values;
        //   a win also calls sub_001410C8([rec+0x3104], 0x1C new pitch / 0x1D switch, slot, 0); result +0xC9 = 0.
        // The chance (sub_00156820) comes from the player, the pitch and the coach, not from the number of players. The hook
        // lets the game roll its own places first, then rolls the queued players with the same calls, one at a time (also
        // roster slot 0, which sub_001555F0 skips).
        constexpr uint32_t kCampProcess = 0x1555F0u, kCampPitchRoll = 0x155D18u, kCampPitchApply = 0x155E58u;
        constexpr uint32_t kCampSwitchRoll = 0x1565A8u, kCampSwitchApply = 0x156690u, kCampBonus = 0x1410C8u;
        constexpr uint32_t kCampCount = 0xC4u, kCampEntries = 0xC6u, kCampStep = 0xC2u, kCampBonusTarget = 0x3104u;
        constexpr uint32_t kCampManager = 0x5DBF58u, kCampRecord = 0xF18u;
        constexpr uint32_t kPitchSlots = 0xA2u; // own records: 10 pitch entries [kind][aptitude][value][limit][u16 exp]
        GuestFunction g_campProcess = nullptr, g_campPitchRoll = nullptr, g_campPitchApply = nullptr;
        GuestFunction g_campSwitchRoll = nullptr, g_campSwitchApply = nullptr, g_campBonus = nullptr;

        std::string campWhat(int type, int param)
        {
            if (type == 0)
                return std::string("새 구종 ") + pitchName(param);
            return type == 1 ? "스위치 타자 전향" : type == 2 ? "컨버트" : "?";
        }

        // The EE scheduler may leave a game function at any call inside it (an EE safe point) and finish it later from its
        // dispatcher, so the hook does not call the game and go on in C++ afterwards: it queues the calls as guest
        // invocations of the current thread, as the runtime's HLE calls do, each one's completion queuing the next one
        // (EeScheduler::invokeCurrent), and the hooked call returns to the game once they are all done.
        PS2Runtime *g_campRuntime = nullptr;
        struct CampJob
        {
            uint16_t id = 0u;
            int slot = -1, type = 0, param = 0;
            uint32_t record = 0u;
            std::string name;
        };
        struct CampRun
        {
            uint32_t rec = 0u;
            R5900Context base{};
            std::vector<CampJob> jobs;
            size_t next = 0u;
            std::vector<std::string> lines;
            std::set<uint16_t> done;
        };
        CampRun g_campRun;          // EE thread only
        bool g_campRunning = false; // EE thread only: while set, the hook lets the game's processor through as it is

        // The own team's roster (its class): the game's camp functions find a roster slot's player there.
        uint32_t ownRoster()
        {
            for (int t = 0; t < kTeams; ++t)
            {
                const uint32_t object = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                if (!valid(object, kTeamRoster + 4u))
                    continue;
                const uint32_t roster = read32(object + kTeamRoster);
                if (valid(roster, kRosterPlayers + 4u * kSlots) && read32(roster + kRosterClass) == kOwnTeamClass)
                    return roster;
            }
            return 0u;
        }

        uint32_t campRecordAt(uint32_t roster, int slot)
        {
            if (roster == 0u || slot < 0 || slot >= kSlots)
                return 0u;
            const uint32_t record = read32(roster + kRosterPlayers + 4u * static_cast<uint32_t>(slot));
            return valid(record, kRecordSize) && read16(record) != 0u ? record : 0u;
        }

        std::string campBats(uint32_t record)
        {
            return kBatsNames[std::min(g_rdram[record + kBats] & 3u, 2u)];
        }

        // A 32-bit value into a register as the EE keeps it (sign-extended to 64 bits).
        void campReg(R5900Context &context, int reg, uint32_t value)
        {
            context.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
        }

        // A game function as an invocation from the hooked call's context: a0-a2 in; `then` gets its context at the end.
        GuestInvocation campCall(uint32_t function, uint32_t a0, uint32_t a1, uint32_t a2, void (*then)(const R5900Context &))
        {
            GuestInvocation call{};
            call.kind = GuestInvocationKind::HleCall;
            call.context = g_campRun.base;
            call.context.pc = function;
            campReg(call.context, 4, a0);
            campReg(call.context, 5, a1);
            campReg(call.context, 6, a2);
            campReg(call.context, 7, 0u);
            campReg(call.context, 29, 0u); // the scheduler's invocation stack
            campReg(call.context, 31, 0u); // back to the dispatcher
            call.onComplete = [then](const R5900Context &done, R5900Context &) { then(done); };
            return call;
        }

        // All done: the change log, and the tried players out of the queue.
        void campFinish()
        {
            g_campRunning = false;
            const std::vector<std::string> lines = std::move(g_campRun.lines);
            const std::set<uint16_t> done = std::move(g_campRun.done);
            g_campRun = CampRun{};
            for (const std::string &line : lines)
                std::fprintf(stderr, "[players] %s\n", line.c_str());
            Settings copy;
            bool save = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const std::string &line : lines)
                    g_log.add(line, "players");
                if (!done.empty())
                {
                    std::vector<CampWish> &camp = g_settings.camp;
                    camp.erase(std::remove_if(camp.begin(), camp.end(), [&](const CampWish &w) { return done.count(w.id) != 0u; }), camp.end());
                    copy = g_settings;
                    save = true;
                }
            }
            if (save)
                saveSettings(copy);
        }

        void campNext();

        void campBonusDone(const R5900Context &)
        {
            const CampJob &job = g_campRun.jobs[g_campRun.next];
            std::string got = " · " + campWhat(job.type, job.param) + " (메모리에서 못 찾음)";
            if (job.type == 0)
            {
                for (int s = 0; s < 10; ++s)
                {
                    const uint32_t entry = job.record + kPitchSlots + 6u * static_cast<uint32_t>(s);
                    if (g_rdram[entry] == job.param)
                        got = " · 변화구 " + std::to_string(s + 1) + " = " + pitchName(g_rdram[entry]) + " " + std::to_string(g_rdram[entry + 2u]) + " (상한 " +
                              std::to_string(g_rdram[entry + 3u]) + ", 득의도 " + std::to_string(g_rdram[entry + 1u]) + ") — 메모리에서 다시 읽은 값";
                }
            }
            else
                got = " · 타석 → " + campBats(job.record) + " — 메모리에서 다시 읽은 값";
            g_campRun.lines.push_back("캠프 판정 성공: " + job.name + got);
            g_campRun.done.insert(job.id);
            ++g_campRun.next;
            campNext();
        }

        void campApplied(const R5900Context &)
        {
            const CampJob &job = g_campRun.jobs[g_campRun.next];
            g_campRuntime->eeScheduler().invokeCurrent(campCall(kCampBonus, read32(g_campRun.rec + kCampBonusTarget), job.type == 0 ? 0x1Cu : 0x1Du,
                                                                static_cast<uint32_t>(job.slot), &campBonusDone));
        }

        void campRolled(const R5900Context &done)
        {
            const uint32_t code = getRegU32(&done, 2);
            const CampJob &job = g_campRun.jobs[g_campRun.next];
            if (code == 0u) // won: the game writes it
                g_campRuntime->eeScheduler().invokeCurrent(campCall(job.type == 0 ? kCampPitchApply : kCampSwitchApply, g_campRun.rec,
                                                                    static_cast<uint32_t>(job.slot), static_cast<uint32_t>(job.param), &campApplied));
            std::string why;
            if (job.type == 0)
                why = code == 1u ? " (게임 조건상 대상이 아님)" : code == 2u ? " (성공 확률 10% 이하)" : code == 3u ? " (성공 확률 10~39%)" : " (성공 확률 40% 이상이었지만 실패)";
            else
                why = code == 1u ? " (우타자만 전향 가능 — 지금 " + campBats(job.record) + ")" : code == 2u ? std::string(" (게임 기준 능력 미달)") : std::string(" (80% 판정에서 실패)");
            g_campRun.lines.push_back("캠프 판정 실패: " + job.name + " · " + campWhat(job.type, job.param) + why);
            g_campRun.done.insert(job.id);
            ++g_campRun.next;
            campNext();
        }

        // The next queued player's roll, or the end.
        void campNext()
        {
            if (g_campRun.next >= g_campRun.jobs.size())
            {
                campFinish();
                return;
            }
            const CampJob &job = g_campRun.jobs[g_campRun.next];
            g_campRuntime->eeScheduler().invokeCurrent(campCall(job.type == 0 ? kCampPitchRoll : kCampSwitchRoll, g_campRun.rec, static_cast<uint32_t>(job.slot),
                                                                static_cast<uint32_t>(job.param), &campRolled));
        }

        // The game's own places are done: their results first, then the queue.
        void campGameDone(const R5900Context &)
        {
            const uint32_t rec = g_campRun.rec, roster = ownRoster();
            std::vector<std::string> game;
            const int count = std::clamp<int>(static_cast<int16_t>(read16(rec + kCampCount)), 0, 6);
            for (int i = 0; i < count; ++i)
            {
                const uint32_t entry = rec + kCampEntries + 4u * static_cast<uint32_t>(i);
                const int slot = static_cast<int8_t>(g_rdram[entry]);
                if (const uint32_t record = campRecordAt(roster, slot); record != 0u)
                    game.push_back("캠프 특수 연습 (게임 6칸): " + nameOf(record) + " · " + campWhat(g_rdram[entry + 1u], g_rdram[entry + 2u]) +
                                   (slot == 0 ? " — 게임이 로스터 0번 선수는 판정하지 않음" : g_rdram[entry + 3u] == 0u ? " 성공" : " 실패"));
            }
            g_campRun.lines.insert(g_campRun.lines.begin(), game.begin(), game.end());
            campNext();
        }

        void campHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_campRunning) // the camp run's own call of the game's processor
            {
                g_campProcess(rdram, ctx, runtime);
                return;
            }
            const uint32_t rec = getRegU32(ctx, 4);
            std::vector<CampWish> wishes;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_team >= 0 && !g_teamMismatch && !g_settingsPath.empty())
                    wishes = g_settings.camp;
            }
            const uint32_t roster = ownRoster();
            if (wishes.empty() || roster == 0u || !valid(rec, kCampBonusTarget + 4u))
            {
                g_campProcess(rdram, ctx, runtime); // as always: the game's own places only
                return;
            }
            CampRun run;
            run.rec = rec;
            run.base = *ctx;
            std::set<uint16_t> inGame; // the game's own places
            const int count = std::clamp<int>(static_cast<int16_t>(read16(rec + kCampCount)), 0, 6);
            for (int i = 0; i < count; ++i)
                if (const uint32_t record = campRecordAt(roster, static_cast<int8_t>(g_rdram[rec + kCampEntries + 4u * static_cast<uint32_t>(i)])); record != 0u)
                    inGame.insert(read16(record));
            for (const CampWish &wish : wishes)
            {
                CampJob job;
                job.id = wish.id;
                job.type = wish.type;
                job.param = wish.param;
                for (int k = 0; k < kSlots && job.slot < 0; ++k)
                    if (const uint32_t at = campRecordAt(roster, k); at != 0u && read16(at) == wish.id)
                    {
                        job.slot = k;
                        job.record = at;
                    }
                job.name = job.record != 0u ? nameOf(job.record) : wish.name.empty() ? "선수 " + std::to_string(wish.id) : wish.name;
                const std::string what = campWhat(wish.type, wish.param);
                std::string skip;
                if (job.record == 0u)
                    skip = "캠프 판정 못함: " + job.name + " · " + what + " (내 구단 로스터에 없음)";
                else if (inGame.count(wish.id) != 0u)
                    skip = "캠프 판정 건너뜀: " + job.name + " · " + what + " (게임 6칸에서 이미 판정함)";
                else if (wish.type == 0)
                {
                    int empty = -1;
                    bool has = false;
                    for (int k = 0; k < 10; ++k)
                    {
                        const int kind = g_rdram[job.record + kPitchSlots + 6u * static_cast<uint32_t>(k)];
                        has = has || kind == wish.param;
                        if (empty < 0 && kind == 0)
                            empty = k;
                    }
                    if (has || empty < 0)
                        skip = "캠프 판정 안 함: " + job.name + " · " + what + (has ? " (이미 던지는 구종)" : " (변화구 칸 10개가 다 참)");
                }
                if (!skip.empty())
                {
                    run.lines.push_back(skip);
                    run.done.insert(wish.id);
                }
                else
                    run.jobs.push_back(job);
            }
            g_campRun = std::move(run);
            g_campRunning = true;
            ctx->pc = getRegU32(ctx, 31); // the hooked call returns to the game once the invocations are done
            runtime->eeScheduler().invokeCurrent(campCall(kCampProcess, rec, 0u, 0u, &campGameDone)); // the game's own places first
        }

        // EE thread, at every vblank after the .pnach patches: find the 12 rosters (the own team by its class) and write
        // the chosen items and skills into the targeted players.
        // Another save (urban::saveid): its own settings file, cheat_saves/<id>/player_cheats.ini. g_mutex held.
        void switchProfile(const std::string &id)
        {
            g_profile = id;
            // Another game is in RAM: this run's backups were taken from the old one, their addresses mean nothing now.
            g_skillBackups.clear();
            g_statBackups.clear();
            g_lockBackups.clear();
            g_oneShots.clear();
            g_seen.clear();
            g_teamMismatch = false;
            std::error_code error;
            if (id.empty())
                g_settingsPath.clear();
            else
            {
                g_settingsPath = urban::saveid::folder(id) / "player_cheats.ini";
                // Once: the settings from before they were per save were made on the owner's main save, M00.
                if (id == "M00" && !std::filesystem::exists(g_settingsPath, error) && std::filesystem::exists(g_legacySettingsPath, error))
                {
                    std::filesystem::create_directories(g_settingsPath.parent_path(), error);
                    std::filesystem::copy_file(g_legacySettingsPath, g_settingsPath, error);
                    std::fprintf(stderr, "[players] the old player_cheats.ini now belongs to save M00\n");
                }
            }
            g_settings = g_settingsPath.empty() ? Settings{} : loadSettings(g_settingsPath); // no file: the defaults (off)
            g_settingsTime = g_settingsPath.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(g_settingsPath, error);
            g_log.add("세이브: " + urban::saveid::describe(id) + " — 이 세이브의 선수 치트 설정 (고정 선수 " + std::to_string(g_settings.edits.size()) +
                          "명 · 따로 정한 선수 " + std::to_string(g_settings.pins.size()) + "명 · 단체 고정 " + (g_settings.groupOn ? "켜짐" : "꺼짐") + ")",
                      "players");
        }

        void frame()
        {
            static unsigned tick = 0u;
            const bool manual = g_refreshNow.exchange(false); // the menu's 새로고침: read the rosters now
            const bool refresh = (++tick % 30u) == 0u || manual;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (const uint32_t generation = urban::saveid::generation(); generation != g_profileGen)
            {
                g_profileGen = generation;
                switchProfile(urban::saveid::current());
            }
            // player_cheats.ini changed outside the tab (an editor, a test, a launcher): read it again (about once a second).
            if (tick % 60u == 0u && !g_settingsPath.empty())
            {
                std::error_code error;
                const auto time = std::filesystem::last_write_time(g_settingsPath, error);
                if (!error && time != g_settingsTime)
                {
                    g_settingsTime = time;
                    g_settings = loadSettings(g_settingsPath);
                    std::fprintf(stderr, "[players] settings reloaded: player cheats %s\n", g_settings.enabled ? "on" : "off");
                }
            }
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
            // The settings belong to one game: the own team they were first used with (another team: another game's).
            if (team >= 0 && !g_settingsPath.empty())
            {
                if (g_settings.team < 0)
                {
                    g_settings.team = team;
                    saveSettings(g_settings);
                }
                g_teamMismatch = g_settings.team != team;
            }
            const bool apply = team >= 0 && g_settings.enabled && !g_teamMismatch && !g_settingsPath.empty();
            if (!rebuild && !apply && g_skillBackups.empty() && g_statBackups.empty() && g_lockBackups.empty() && g_oneShots.empty() &&
                g_practices.empty())
                return;
            std::map<uint16_t, uint32_t> blocks; // where the one-time edits go (only while there are some)
            std::map<uint16_t, std::pair<uint32_t, uint32_t>> places; // record and ability block, for the camp practice
            for (int t = 0; t < kTeams; ++t)
            {
                if (rosters[t] == 0u)
                    continue;
                const bool own = t == team;
                const bool scoped = groupApplies(g_settings, t, team);
                const bool personal = own || !g_settings.ownOnly; // a player's own locks and items reach this team
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
                        row.skills = readSkills(record);
                        std::memcpy(row.block.data(), g_rdram + block, kBlockSize);
                        if (!row.pitcher)
                        {
                            int best = sPosC;
                            for (int stat = sPosC; stat <= sPosOF; ++stat)
                                if (row.value(stat) > row.value(best))
                                    best = stat;
                            row.position = best - sPosP;
                        }
                        row.bats = g_rdram[record + kBats] & 3u;
                        g_rows[static_cast<size_t>(t)].push_back(std::move(row));
                    }
                    if (!g_oneShots.empty())
                        blocks[id] = block;
                    if (!g_practices.empty())
                        places[id] = {record, block};
                    uint32_t mask = 0u;
                    const std::vector<uint16_t> *skills = nullptr;
                    const std::array<int, kSkillSlots> *slots = nullptr;
                    if (apply)
                    {
                        const auto pin = g_settings.pins.find(id);
                        if (pin != g_settings.pins.end())
                        {
                            if (pin->second.on && personal)
                            {
                                mask = pin->second.appliedMask; // what 이 선수 적용 및 고정 took, not the draft
                                skills = &pin->second.appliedSkills;
                                slots = &pin->second.appliedSlots;
                            }
                        }
                        else if (scoped)
                        {
                            mask = g_settings.applied.mask; // what 단체 적용 및 고정 took
                            skills = &g_settings.applied.skills;
                        }
                    }
                    if (mask != 0u || !g_statBackups.empty())
                        applyItems(record, block, own, id, mask);
                    if (skills || slots || !g_skillBackups.empty())
                        applySkills(record, id, skills, slots);
                    // Value edits over the items: the player's own locks, else the default target's (not for a picked
                    // player switched off).
                    const std::map<int, StatEdit> *groupLocks = nullptr, *ownLocks = nullptr;
                    if (apply)
                    {
                        const auto pin = g_settings.pins.find(id);
                        if (scoped && !g_settings.applied.stats.empty() && (pin == g_settings.pins.end() || pin->second.on))
                            groupLocks = &g_settings.applied.stats; // what 단체 적용 및 고정 took
                        const auto edit = g_settings.edits.find(id);
                        if (edit != g_settings.edits.end() && edit->second.on && !edit->second.stats.empty() && personal)
                            ownLocks = &edit->second.stats;
                    }
                    if (groupLocks || ownLocks || !g_lockBackups.empty())
                        applyLocks(record, block, id, groupLocks, ownLocks);
                }
            }
            // One-time edits (the editor's 적용): written once where the player is now; dropped after ~10 s unfound.
            for (auto it = g_oneShots.begin(); it != g_oneShots.end();)
            {
                const auto at = blocks.find(it->id);
                if (at == blocks.end())
                {
                    if (++it->age > 600)
                    {
                        std::fprintf(stderr, "[players] edit for %u dropped: not on a roster\n", it->id);
                        g_log.add("적용 못함: " + nameOfId(it->id) + " · " + kStats[it->stat].label + " (10초 동안 로스터에서 못 찾음)", "players");
                        it = g_oneShots.erase(it);
                    }
                    else
                        ++it;
                    continue;
                }
                const uint32_t entry = at->second + kStats[it->stat].offset - kBlock;
                writeStat(entry, it->edit);
                std::fprintf(stderr, "[players] edit %u %s: %d / %d / %d\n", it->id, kStats[it->stat].key, g_rdram[entry + 2u],
                             g_rdram[entry + 3u], g_rdram[entry + 1u]);
                g_log.add("적용됨: " + nameOfId(it->id) + " · " + kStats[it->stat].label + " " + std::to_string(g_rdram[entry + 2u]) + " (상한 " +
                              std::to_string(g_rdram[entry + 3u]) + ", 득의도 " + std::to_string(g_rdram[entry + 1u]) + ") — 메모리에서 다시 읽은 값",
                          "players");
                it = g_oneShots.erase(it);
            }
            // Tests: URBAN_TEST_PRACTICE="<id>,<pitch>,<value>,<limit>,<apt>,<bats>;..." queued once a game runs.
            static bool testQueued = false;
            if (!testQueued && team >= 0)
            {
                testQueued = true;
                if (const char *test = std::getenv("URBAN_TEST_PRACTICE"); test && *test)
                {
                    std::istringstream items(test);
                    std::string item;
                    while (std::getline(items, item, ';'))
                    {
                        int f[6] = {0, 0, 50, 99, 2, -1};
                        std::istringstream fields(item);
                        std::string field;
                        for (int k = 0; k < 6 && std::getline(fields, field, ','); ++k)
                            f[k] = std::atoi(field.c_str());
                        if (f[0] > 0)
                            g_practices.push_back(Practice{static_cast<uint16_t>(f[0]), f[1], f[2], f[3], f[4], f[5], 0});
                    }
                }
            }
            // Tests: URBAN_TEST_CAMP_STEP=<n>: once the camp has run its first step (its start sets the step to 0), the camp
            // record's step jumps to n, so that the next step is the one that rolls the special practice.
            static bool testStepDone = false;
            if (const char *step = testStepDone ? nullptr : std::getenv("URBAN_TEST_CAMP_STEP"); step && *step)
            {
                const uint32_t camp = read32(kCampManager);
                const uint32_t rec = valid(camp, kCampRecord + 4u) ? read32(camp + kCampRecord) : 0u;
                const int now = valid(rec, kCampBonusTarget + 4u) ? static_cast<int16_t>(read16(rec + kCampStep)) : -1;
                if (now >= 1 && now < std::atoi(step))
                {
                    testStepDone = true;
                    const uint16_t value = static_cast<uint16_t>(std::atoi(step));
                    std::memcpy(g_rdram + rec + kCampStep, &value, sizeof(value));
                    std::fprintf(stderr, "[players] test: camp step %d -> %u\n", now, value);
                }
            }
            // Tests: URBAN_TEST_CAMP="<id>,<type>,<param>;..." put in the camp queue once this save's settings are open.
            static bool testCampQueued = false;
            if (!testCampQueued && team >= 0 && !g_settingsPath.empty())
            {
                testCampQueued = true;
                // URBAN_TEST_EDIT="<player id>,<ability key>,<value>;...": one-time edits, as the menu's 적용 (tests).
                if (const char *edits = std::getenv("URBAN_TEST_EDIT"); edits && *edits)
                {
                    std::istringstream items(edits);
                    std::string item;
                    while (std::getline(items, item, ';'))
                    {
                        std::istringstream fields(item);
                        std::string id, key, value;
                        if (!std::getline(fields, id, ',') || !std::getline(fields, key, ',') || !std::getline(fields, value, ','))
                            continue;
                        const int stat = statIndex(trim(key));
                        if (stat < 0)
                            continue;
                        StatEdit edit;
                        edit.value = std::clamp(std::atoi(value.c_str()), 0, kValueMax);
                        edit.limit = -2;
                        g_oneShots.push_back(OneShot{static_cast<uint16_t>(std::atoi(id.c_str())), stat, edit, 0});
                    }
                }
                if (const char *test = std::getenv("URBAN_TEST_CAMP"); test && *test)
                {
                    std::istringstream items(test);
                    std::string item;
                    while (std::getline(items, item, ';'))
                    {
                        int f[3] = {0, 0, 0};
                        std::istringstream fields(item);
                        std::string field;
                        for (int k = 0; k < 3 && std::getline(fields, field, ','); ++k)
                            f[k] = std::atoi(field.c_str());
                        const uint16_t id = static_cast<uint16_t>(std::clamp(f[0], 0, 0xFFFF));
                        if (id == 0u)
                            continue;
                        std::vector<CampWish> &camp = g_settings.camp;
                        camp.erase(std::remove_if(camp.begin(), camp.end(), [id](const CampWish &w) { return w.id == id; }), camp.end());
                        camp.push_back(CampWish{id, f[1], f[2], nameOfId(id)});
                    }
                    saveSettings(g_settings);
                    std::fprintf(stderr, "[players] test: camp queue %zu\n", g_settings.camp.size());
                }
            }
            // Camp practice (특훈): written once where the player is now; dropped after ~10 s unfound.
            for (auto it = g_practices.begin(); it != g_practices.end();)
            {
                const auto at = places.find(it->id);
                if (at == places.end())
                {
                    if (++it->age > 600)
                    {
                        g_log.add("특훈 못함: " + nameOfId(it->id) + " (10초 동안 로스터에서 못 찾음)", "players");
                        it = g_practices.erase(it);
                    }
                    else
                        ++it;
                    continue;
                }
                const uint32_t record = at->second.first, block = at->second.second;
                const std::string name = nameOfId(it->id);
                if (it->pitch > 1 && it->pitch < kPitchKinds)
                {
                    static const int firstPitch = statIndex("pitch1");
                    int slot = -1;
                    bool has = false;
                    for (int s = 0; s < 10 && firstPitch >= 0; ++s)
                    {
                        const uint32_t entry = block + kStats[firstPitch + s].offset - kBlock;
                        has = has || g_rdram[entry] == it->pitch;
                        if (slot < 0 && g_rdram[entry] == 0u)
                            slot = s;
                    }
                    if (has)
                        g_log.add("새 구종 안 넣음: " + name + "은(는) 이미 " + pitchName(it->pitch) + "을(를) 던짐", "players");
                    else if (slot < 0)
                        g_log.add("새 구종 안 넣음: " + name + "의 변화구 칸 10개가 다 참", "players");
                    else
                    {
                        const uint32_t entry = block + kStats[firstPitch + slot].offset - kBlock;
                        g_rdram[entry] = static_cast<uint8_t>(it->pitch);
                        g_rdram[entry + 1u] = static_cast<uint8_t>(std::clamp(it->apt, 0, 4));
                        g_rdram[entry + 2u] = static_cast<uint8_t>(std::clamp(it->value, 1, kValueMax));
                        g_rdram[entry + 3u] = static_cast<uint8_t>(std::clamp(std::max(it->limit, it->value), 1, kValueMax));
                        // The experience that goes with the value: the game's table at 0x5D1320 (u16 per value 0-100;
                        // the own players' abilities all sit in [table[v], table[v + 1])). Without it the game shows D.
                        const uint16_t exp = read16(kExpTable + 2u * static_cast<uint32_t>(std::min<int>(g_rdram[entry + 2u], 99)));
                        g_rdram[entry + 4u] = static_cast<uint8_t>(exp);
                        g_rdram[entry + 5u] = static_cast<uint8_t>(exp >> 8);
                        g_log.add("새 구종: " + name + " · 변화구 " + std::to_string(slot + 1) + " = " + pitchName(g_rdram[entry]) + " " +
                                      std::to_string(g_rdram[entry + 2u]) + " (상한 " + std::to_string(g_rdram[entry + 3u]) + ") — 메모리에서 다시 읽은 값",
                                  "players");
                    }
                }
                if (it->bats >= 0 && it->bats <= 2)
                {
                    g_rdram[record + kBats] = static_cast<uint8_t>((g_rdram[record + kBats] & ~3u) | static_cast<unsigned>(it->bats));
                    g_log.add("타석: " + name + " → " + kBatsNames[g_rdram[record + kBats] & 3u] + " — 메모리에서 다시 읽은 값", "players");
                }
                it = g_practices.erase(it);
            }
            if (rebuild)
                logRosterChanges(team, manual);
        }

#if defined(URBAN_PLAYER_MENU)

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
                const urban::skills::Skill *skill = urban::skills::find(skills[i], false);
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
            for (int group = urban::skills::kPlayer; group <= urban::skills::kCoach; ++group)
            {
                if (!ui->begin_tab(urban::skills::groupName(group)))
                    continue;
                ui->begin_child("skilllist", 0.0f, 170.0f);
                int column = 0;
                for (const urban::skills::Skill *entry = urban::skills::begin(); entry != urban::skills::end(); ++entry)
                {
                    const urban::skills::Skill &skill = *entry;
                    if (skill.group != group)
                        continue;
                    const auto at = std::find(skills.begin(), skills.end(), skill.id);
                    int on = at != skills.end() ? 1 : 0;
                    ui->push_id(300000 + skill.id);
                    if (column++ % 2 != 0)
                        ui->same_line();
                    ui->begin_disabled(!on && skills.size() >= static_cast<size_t>(kSkillSlots) ? 1 : 0);
                    if (ui->checkbox(urban::skills::label(g_rdram, skill).c_str(), &on))
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

        // A picked player's 8 skill slots one by one: as it is, empty, or any skill (written after the added skills; taken
        // off, the slots go back to the player's own). `row`: the player as the roster has them now (null when away).
        void drawSlotEditor(const Pcsx2GsUi *ui, Pin &pin, const Row *row, bool &changed)
        {
            ui->separator_text("스킬 슬롯별 편집 (이 선수의 8칸)");
            // The choices: as it is, empty, then every player / WS / manager / coach skill by group.
            static std::vector<std::string> labels;
            static std::vector<int> ids;
            if (labels.empty() || labels.size() < 3u)
            {
                labels = {"그대로", "비우기"};
                ids = {-1, 0};
                for (int group = urban::skills::kPlayer; group <= urban::skills::kCoach; ++group)
                    for (const urban::skills::Skill *skill = urban::skills::begin(); skill != urban::skills::end(); ++skill)
                        if (skill->group == group)
                        {
                            labels.push_back(std::string("[") + urban::skills::groupName(group) + "] " + urban::skills::label(g_rdram, *skill));
                            ids.push_back(skill->id);
                        }
            }
            std::vector<const char *> pointers;
            for (const std::string &label : labels)
                pointers.push_back(label.c_str());
            if (ui->small_button("8칸 모두 그대로"))
            {
                pin.slots.fill(-1);
                changed = true;
            }
            for (int k = 0; k < kSkillSlots; ++k)
            {
                ui->push_id(800000 + k);
                const uint16_t now = row ? row->skills[static_cast<size_t>(k)] : 0u;
                const urban::skills::Skill *skill = now ? urban::skills::find(now, false) : nullptr;
                char head[160];
                std::snprintf(head, sizeof(head), "칸 %d: 지금 %s", k + 1, now == 0u ? "비어 있음" : skill ? skill->ko : "?");
                ui->text(head);
                ui->same_line();
                const int picked = pin.slots[static_cast<size_t>(k)];
                int current = 0;
                for (size_t i = 0; i < ids.size(); ++i)
                    if (ids[i] == picked)
                        current = static_cast<int>(i);
                ui->set_next_width(380.0f);
                if (ui->combo("##slot", &current, pointers.data(), static_cast<int>(pointers.size())))
                {
                    pin.slots[static_cast<size_t>(k)] = ids[static_cast<size_t>(current)];
                    changed = true;
                }
                ui->pop_id();
            }
            ui->text_disabled("'그대로'인 칸은 원래 스킬(또는 위 '스킬' 탭에서 더한 스킬)이 남습니다. 칸을 정하면 매 프레임 그 스킬로 유지됩니다.");
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

        // ---- The player editor (GS thread) ----
        // Laid out like Football Manager's squad screen and the game's own lists: every club's players in a sortable,
        // filtered table on the left (tick several for bulk edits), the focused player's card on the right with the
        // abilities by value or rank, once (적용) or locked (고정), and the S+ items, skills and slots of their own.
        struct Draft
        {
            int value = 0, limit = 0, apt = 0;
            bool use = false; // bulk and group rows: part of the next action
        };
        uint16_t g_focus = 0u;              // the player on the card
        std::set<uint16_t> g_picked;        // the ticked players
        bool g_bulk = false;                // the card edits the ticked players
        std::map<int, Draft> g_drafts;      // the card's typed values by kStats index
        int g_draftFor = -1;                // whose: a player ID, 0 = the ticked players (reset when the card changes)
        std::map<int, Draft> g_groupDrafts; // the group tab's rows
        char g_search[64] = "";
        int g_teamFilter = 0; // 0 every club, 1 the own club, 2 + t club t
        int g_posFilter = 0;  // 0 all, 1 pitchers, 2 fielders, 3 + p: best position C ... OF
        int g_onlyEdited = 0, g_onlyPicked = 0;
        int g_lastClick = -1; // the row last clicked, in the shown order (shift-click ranges)
        int g_fillGrade = 0;  // the quick fill's rank
        constexpr const char *kPositions[7] = {"투", "포", "1루", "2루", "3루", "유", "외"};
        // IM_COL32 order (R in the low byte): S+ gold, S orange, A+/A red, B+/B yellow, C+/C green, D+ blue, D grey.
        constexpr uint32_t kGradeColors[kGradeCount] = {0xFF2CD7FFu, 0xFF28AAFFu, 0xFF5A5AFFu, 0xFF7882FFu, 0xFF50BEFFu,
                                                        0xFF78DCF0u, 0xFF6EDCAAu, 0xFF78C878u, 0xFFE6AA6Eu, 0xFF968C8Cu};
        constexpr uint32_t kLockedRow = 0x5A285A28u, kPinnedRow = 0x5A143C5Au, kChangedText = 0xFF66CCFFu;

        uint32_t gradeColor(int value) { return kGradeColors[gradeOf(value)]; }
        int kmOf(int value) { return 111 + (std::min(value, 99) * 51 + 49) / 99; }
        std::string valueText(int stat, int value)
        {
            char text[32];
            if (stat == sSpeedP)
                std::snprintf(text, sizeof(text), "%dkm %s", kmOf(value), kGrades[gradeOf(value)]);
            else
                std::snprintf(text, sizeof(text), "%s %d", kGrades[gradeOf(value)], value);
            return text;
        }
        std::string aptText(int apt) { return apt >= 0 && apt <= 4 ? kAptitudes[apt] : std::to_string(apt); }
        std::string editText(const StatEdit &edit)
        {
            std::string text = edit.value >= 0 ? valueText(-1, edit.value) : std::string("값 그대로");
            if (edit.limit >= 0)
                text += " 상한 " + std::to_string(edit.limit);
            if (edit.apt >= 0)
                text += std::string(" 득의 ") + kAptitudes[edit.apt];
            return text;
        }

        const Row *findRow(const std::array<std::vector<Row>, kTeams> &rows, uint16_t id)
        {
            for (const std::vector<Row> &list : rows)
                for (const Row &row : list)
                    if (row.id == id)
                        return &row;
            return nullptr;
        }

        // The S+ items a player gets now (the frame hook's choice).
        uint32_t itemsFor(const Settings &settings, const Row &row, int own)
        {
            if (!settings.enabled)
                return 0u;
            const auto pin = settings.pins.find(row.id);
            if (pin != settings.pins.end())
                return pin->second.on && (row.team == own || !settings.ownOnly) ? pin->second.appliedMask : 0u;
            return groupApplies(settings, row.team, own) ? settings.applied.mask : 0u;
        }

        // The items that write ability `stat`'s aptitude, value or limit (they win over a one-time edit, not over a lock).
        uint32_t coverMask(int stat)
        {
            static std::array<uint32_t, kStatCount> masks{};
            static bool done = false;
            if (!done)
            {
                for (int s = 0; s < kStatCount; ++s)
                    for (size_t i = 0; i < g_items.size() && i < 32u; ++i)
                        for (const Run &run : g_items[i].runs)
                            if (run.offset < kStats[s].offset + 4u && run.offset + run.bytes.size() > kStats[s].offset + 1u)
                                masks[static_cast<size_t>(s)] |= 1u << i;
                done = true;
            }
            return masks[static_cast<size_t>(stat)];
        }

        // What a typed row stands for: the value always; the limit when typed (else raised to the value when lower); the
        // aptitude when changed. Without a player (bulk, group): the aptitude as picked, -1 leaving each player's own.
        StatEdit editFrom(const Draft &draft, const Row *row, int stat)
        {
            StatEdit edit;
            edit.value = std::clamp(draft.value, 0, kValueMax);
            if (row)
            {
                edit.limit = draft.limit != row->value(stat, 3) ? std::clamp(draft.limit, 0, kValueMax) : -2;
                edit.apt = draft.apt != std::min(row->value(stat, 1), 4) ? std::clamp(draft.apt, 0, 4) : -1;
            }
            else
            {
                edit.limit = -2;
                edit.apt = draft.apt >= 0 && draft.apt <= 4 ? draft.apt : -1;
            }
            return edit;
        }

        void queueEdit(uint16_t id, int stat, const StatEdit &edit)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_oneShots.push_back(OneShot{id, stat, edit, 0});
        }

        void setLock(Settings &settings, uint16_t id, const std::string &name, int stat, const StatEdit *edit)
        {
            if (edit)
            {
                PlayerEdit &player = settings.edits[id];
                if (player.name.empty())
                    player.name = name;
                player.stats[stat] = *edit;
                return;
            }
            const auto player = settings.edits.find(id);
            if (player == settings.edits.end())
                return;
            player->second.stats.erase(stat);
            if (player->second.stats.empty())
                settings.edits.erase(player);
        }

        const StatEdit *lockOf(const Settings &settings, uint16_t id, int stat)
        {
            const auto player = settings.edits.find(id);
            if (player == settings.edits.end())
                return nullptr;
            const auto lock = player->second.stats.find(stat);
            return lock == player->second.stats.end() ? nullptr : &lock->second;
        }

        int lockCount(const Settings &settings, uint16_t id)
        {
            const auto player = settings.edits.find(id);
            return player == settings.edits.end() ? 0 : static_cast<int>(player->second.stats.size());
        }

        bool shownStat(const Row *row, int stat)
        {
            return kStats[stat].group != kPitches || !row || row->value(stat, 0) != 0; // an empty pitch slot
        }

        void resetDrafts(int owner)
        {
            if (g_draftFor != owner)
            {
                g_drafts.clear();
                g_draftFor = owner;
            }
        }

        // The card's row for one player: the lock's values when locked, else the player's own.
        Draft &cardDraft(int stat, const Row &row, const StatEdit *lock)
        {
            auto it = g_drafts.find(stat);
            if (it == g_drafts.end())
            {
                Draft draft;
                draft.value = lock && lock->value >= 0 ? lock->value : row.value(stat);
                draft.limit = lock && lock->limit >= 0 ? lock->limit : row.value(stat, 3);
                draft.apt = lock && lock->apt >= 0 ? lock->apt : std::min(row.value(stat, 1), 4);
                it = g_drafts.emplace(stat, draft).first;
            }
            return it->second;
        }

        bool draftChanged(const Draft &draft, const Row &row, int stat)
        {
            return draft.value != row.value(stat) || draft.limit != row.value(stat, 3) || draft.apt != std::min(row.value(stat, 1), 4);
        }

        const char *groupTitle(int group)
        {
            return group == kPitching ? "투수 능력" : group == kBatting ? "타격 · 주루 · 수비" : group == kFielding ? "수비 위치 (적성)" : "변화구";
        }

        // One player's abilities of one group: name, now, the new value (typed or by rank), limit, aptitude, 고정, 적용.
        void drawStatTable(const Pcsx2GsUi *ui, Settings &settings, const Row &row, int group, uint32_t items, bool groupLocked,
                           bool &changed)
        {
            ui->push_id(500 + group);
            if (!ui->begin_table("stats", 8, PCSX2GS_TABLE_ROW_BG | PCSX2GS_TABLE_BORDERS, 0.0f, 0.0f))
            {
                ui->pop_id();
                return;
            }
            ui->table_column("능력", 0, 86.0f);
            ui->table_column("지금 (상한·득의도)", 0, 150.0f);
            ui->table_column("새 값", 0, 60.0f);
            ui->table_column("등급", 0, 62.0f);
            ui->table_column("상한", 0, 56.0f);
            ui->table_column("득의도", 0, 54.0f);
            ui->table_column("고정", 0, 40.0f);
            ui->table_column("", 0, 0.0f);
            ui->table_headers();
            for (int stat = 0; stat < kStatCount; ++stat)
            {
                if (kStats[stat].group != group || !shownStat(&row, stat))
                    continue;
                const StatEdit *lock = lockOf(settings, row.id, stat);
                Draft &draft = cardDraft(stat, row, lock);
                ui->push_id(stat);
                ui->table_next_row();
                if (lock)
                    ui->table_row_color(kLockedRow);
                ui->table_next_column();
                // A pitch row says which pitch it is (the game's list: 2 체인지업 ... 19 고속 슬라이더).
                const std::string label = group == kPitches ? std::string(kStats[stat].label) + " · " + pitchName(row.value(stat, 0)) : kStats[stat].label;
                if (draftChanged(draft, row, stat))
                    ui->text_colored(kChangedText, label.c_str());
                else
                    ui->text(label.c_str());
                if (group == kPitches)
                {
                    char kind[48];
                    std::snprintf(kind, sizeof(kind), "%s · 구종 번호 0x%02X", kStats[stat].jp, row.value(stat, 0));
                    ui->tooltip(kind);
                }
                else
                    ui->tooltip(kStats[stat].jp);
                ui->table_next_column();
                const int now = row.value(stat);
                ui->text_colored(gradeColor(now), valueText(stat, now).c_str());
                ui->same_line();
                ui->text_disabled(("/" + std::to_string(row.value(stat, 3)) + " · " + aptText(row.value(stat, 1))).c_str());
                bool typed = false;
                ui->table_next_column();
                ui->set_next_width(56.0f);
                if (ui->input_int("##value", &draft.value, 0))
                {
                    draft.value = std::clamp(draft.value, 0, kValueMax);
                    typed = true;
                }
                ui->tooltip("0~255. 경기는 능력을 경험치로 계산해서(최대 99), 값과 함께 그 경험치도 씁니다. 99 넘는 값은 F2 '99 제한' 탭의 스위치를 켜야 경기에서 그 값 그대로 쓰이고, 끄면 99입니다. 게임 화면에는 99까지만 등급이 나와 100 이상은 D로 보입니다.");
                ui->table_next_column();
                int rank = gradeOf(draft.value);
                ui->set_next_width(58.0f);
                if (ui->combo("##rank", &rank, kGrades, kGradeCount))
                {
                    draft.value = kGradeTop[rank];
                    typed = true;
                }
                ui->tooltip("등급을 고르면 그 등급의 가장 높은 값 (S+ 99, S 89, A+ 79, A 69, B+ 59, B 54, C+ 49, C 44, D+ 39, D 34)");
                ui->table_next_column();
                ui->set_next_width(52.0f);
                if (ui->input_int("##limit", &draft.limit, 0))
                {
                    draft.limit = std::clamp(draft.limit, 0, kValueMax);
                    typed = true;
                }
                ui->tooltip("상한(上限): 성장으로 오를 수 있는 최대. 새 값이 더 크면 상한도 그만큼 올립니다.");
                ui->table_next_column();
                ui->set_next_width(50.0f);
                if (ui->combo("##apt", &draft.apt, kAptitudes, 5))
                    typed = true;
                ui->tooltip("득의도(得意度): 연습으로 오르는 빠르기 S > A > B > C > D");
                ui->table_next_column();
                (void)typed; // a typed value is a draft: nothing changes before 적용 / 고정 (owner, 11-06)
                int locked = lock ? 1 : 0;
                if (ui->checkbox("##lock", &locked))
                {
                    const StatEdit edit = editFrom(draft, &row, stat);
                    setLock(settings, row.id, row.name, stat, locked ? &edit : nullptr);
                    changed = true;
                }
                ui->tooltip("고정(적용 및 고정): 체크하면 지금 '새 값' 칸 그대로 매 프레임 다시 씁니다 (성장·노화·S+ 항목보다 우선). "
                            "고정 중에 새 값을 바꾸면 '새 값으로 고정'을 눌러야 바뀝니다. 끄면 원래 값으로 (위 설정이 켜져 있을 때).");
                ui->table_next_column();
                if (ui->small_button("적용"))
                    queueEdit(row.id, stat, editFrom(draft, &row, stat));
                ui->tooltip("한 번만 씁니다. 그 뒤로는 게임(성장·노화)이 바꿀 수 있습니다.");
                if (lock)
                {
                    const StatEdit edit = editFrom(draft, &row, stat);
                    if (!(edit == *lock))
                    {
                        ui->same_line();
                        if (ui->small_button("새 값으로 고정"))
                        {
                            setLock(settings, row.id, row.name, stat, &edit);
                            changed = true;
                        }
                        ui->tooltip(("지금 고정 값 " + editText(*lock) + "과 새 값이 다릅니다. 눌러야 새 값으로 고정됩니다.").c_str());
                    }
                }
                if (!lock && (items & coverMask(stat)) != 0u)
                {
                    ui->same_line();
                    ui->text_disabled("S+ 항목이 씀");
                    ui->tooltip("S+ 항목이 매 프레임 이 칸을 써서 '적용'은 바로 덮입니다. 이 값을 쓰려면 '고정'을 켜세요.");
                }
                else if (!lock && groupLocked && settings.groupStats.count(stat) != 0u)
                {
                    ui->same_line();
                    ui->text_disabled("단체 고정");
                    ui->tooltip(("단체 적용 탭의 고정 값 " + editText(settings.groupStats.at(stat)) + "이 쓰입니다. 여기서 고정하면 이 선수만 이 값으로.").c_str());
                }
                ui->pop_id();
            }
            ui->end_table();
            ui->pop_id();
        }

        // The S+ items and skills of one player of their own (a pin) instead of the default ones.
        void drawPinTab(const Pcsx2GsUi *ui, Settings &settings, const Row &row, int own, bool slotsOnly, bool &changed)
        {
            const auto pin = settings.pins.find(row.id);
            if (pin == settings.pins.end())
            {
                const bool scoped = groupApplies(settings, row.team, own);
                ui->text_wrapped(scoped ? ("단체 고정을 받는 중: S+ 항목 " + std::to_string(std::popcount(settings.mask)) + "개 · 스킬 " +
                                           std::to_string(settings.skills.size()) + "개 ('단체 적용' 탭에서 바꿈).")
                                              .c_str()
                                        : "단체 고정을 받지 않습니다: 이 선수에게 S+ 항목·스킬을 쓰지 않습니다 (따로 정하면 이 선수에게만).");
                if (ui->button("이 선수만 따로 정하기"))
                {
                    Pin added;
                    added.on = false; // nothing is written until '이 선수에게 고정' (owner, 11-06)
                    added.mask = settings.mask;
                    added.skills = settings.skills;
                    added.name = row.name;
                    settings.pins[row.id] = std::move(added);
                    changed = true;
                }
                ui->text_disabled("따로 정하면 이 선수만의 항목·스킬·슬롯을 고릅니다 (지금 단체 항목을 복사해서 시작). '이 선수에게 고정'을 켜기 전에는 아무것도 쓰지 않습니다.");
                ui->text_disabled("능력치 수치·등급 고정은 이것과 상관없이 늘 맨 위에 씁니다.");
                return;
            }
            // Edit freely below; nothing is written before 이 선수 적용 및 고정 (owner, 11-06).
            const bool pending = pin->second.pending();
            if (ui->button_colored(pin->second.on && !pending ? "이 선수 적용 및 고정 (지금 그대로)" : "이 선수 적용 및 고정",
                                   pending || !pin->second.on ? 0xFF3A6AD0u : 0u, 0.0f))
            {
                pin->second.appliedMask = pin->second.mask;
                pin->second.appliedSkills = pin->second.skills;
                pin->second.appliedSlots = pin->second.slots;
                pin->second.on = true;
                changed = true;
            }
            ui->tooltip("아래에서 고른 S+ 항목·스킬·슬롯을 그대로 받아 이 선수에게 매 프레임 씁니다. 누르기 전에는 아무것도 바뀌지 않습니다.");
            if (pin->second.on)
            {
                ui->same_line();
                if (ui->button("이 선수 고정 끄기"))
                {
                    pin->second.on = false;
                    changed = true;
                }
                ui->tooltip("이 선수에게 쓰기를 멈춥니다 (단체 능력치 고정도 받지 않음, 능력치 카드의 개별 고정은 그대로).");
            }
            ui->text_colored(pin->second.on ? 0xFF5050FFu : 0xFF968C8Cu,
                             pin->second.on ? ("고정 중: S+ 항목 " + std::to_string(std::popcount(pin->second.appliedMask)) + "개 · 스킬 " +
                                               std::to_string(pin->second.appliedSkills.size()) + "개를 쓰는 중")
                                                  .c_str()
                                            : "고정 꺼짐: 이 선수에게 S+ 항목·스킬을 쓰지 않습니다.");
            if (pending && pin->second.on)
                ui->text_colored(kChangedText, "아래에서 바꾼 내용은 아직 적용 안 됨: '이 선수 적용 및 고정'을 눌러야 씁니다.");
            const int on = 1; // the draft below is always editable
            ui->same_line();
            if (ui->small_button("따로 정하기 해제 (단체 설정으로)"))
            {
                settings.pins.erase(pin);
                changed = true;
                return;
            }
            ui->begin_disabled(on ? 0 : 1);
            if (slotsOnly)
                drawSlotEditor(ui, pin->second, &row, changed);
            else
                drawItems(ui, pin->second.mask, pin->second.skills, changed);
            ui->end_disabled();
        }

        void fillDrafts(const Row *row, int group, int value, const Settings &settings, uint16_t id)
        {
            for (int stat = 0; stat < kStatCount; ++stat)
            {
                if ((group >= 0 && kStats[stat].group != group) || (group < 0 && kStats[stat].group == kPitches) || !shownStat(row, stat))
                    continue;
                if (row)
                    cardDraft(stat, *row, lockOf(settings, id, stat)).value = value;
                else
                {
                    auto it = g_drafts.find(stat);
                    if (it == g_drafts.end())
                    {
                        Draft draft;
                        draft.apt = -1; // as each player has it
                        it = g_drafts.emplace(stat, draft).first;
                    }
                    it->second.value = value;
                    it->second.use = true;
                }
            }
        }

        // 특훈: the camp's special practice done at once for any number of players (a new pitch for pitchers, the batting
        // side for anyone), written once (적용) by the frame hook where the players are now.
        int g_practicePitch = 17; // index in the choices below (고속 슬라이더)
        int g_practiceValue = 50, g_practiceLimit = 99, g_practiceApt = 2, g_practiceBats = 2;
        // The camp queue changes at once under g_mutex (the camp hook takes the tried players out on the EE thread).
        template <typename Edit>
        void editCamp(Edit edit)
        {
            Settings copy;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_settingsPath.empty())
                    return;
                edit(g_settings.camp);
                copy = g_settings;
            }
            saveSettings(copy);
        }

        // Own players only; one wish a player (a new one replaces it). Switch: right-handed batters only (the game's rule).
        void queueCamp(const std::vector<const Row *> &targets, int type, int param)
        {
            editCamp([&](std::vector<CampWish> &camp) {
                int added = 0, other = 0, notRight = 0;
                for (const Row *row : targets)
                {
                    if (type == 0 && !row->pitcher)
                        continue;
                    if (row->team != g_team)
                    {
                        ++other;
                        continue;
                    }
                    if (type == 1 && row->bats != 0)
                    {
                        ++notRight;
                        continue;
                    }
                    camp.erase(std::remove_if(camp.begin(), camp.end(), [row](const CampWish &w) { return w.id == row->id; }), camp.end());
                    camp.push_back(CampWish{row->id, type, param, row->name});
                    ++added;
                }
                std::string line = "캠프 판정 대기에 넣음: " + std::to_string(added) + "명 · " + campWhat(type, param);
                if (other > 0)
                    line += " (내 구단이 아닌 " + std::to_string(other) + "명 뺌)";
                if (notRight > 0)
                    line += " (우타자가 아닌 " + std::to_string(notRight) + "명 뺌: 게임은 우타자만 전향)";
                g_log.add(line, "players");
            });
        }

        void drawPractice(const Pcsx2GsUi *ui, const std::vector<const Row *> &targets, const std::vector<CampWish> &queue)
        {
            static const int firstPitch = statIndex("pitch1");
            int pitchers = 0;
            for (const Row *row : targets)
                pitchers += row->pitcher ? 1 : 0;
            ui->text_disabled("캠프 특수 연습(새 구종 습득·스위치 전향)을 인원 제한 없이 바로 적용합니다. 게임의 성공·실패 판정은 거치지 않습니다.");
            ui->separator_text(("새 구종 추가 (투수 " + std::to_string(pitchers) + "명)").c_str());
            if (targets.size() == 1u && targets[0]->pitcher && firstPitch >= 0)
            {
                std::string have;
                for (int s = 0; s < 10; ++s)
                    if (const int kind = targets[0]->value(firstPitch + s, 0); kind != 0)
                        have += std::string(have.empty() ? "" : ", ") + pitchName(kind);
                ui->text_disabled(("지금 던지는 변화구: " + (have.empty() ? std::string("없음") : have)).c_str());
            }
            const char *choices[kPitchKinds - 2];
            for (int k = 2; k < kPitchKinds; ++k)
                choices[k - 2] = kPitchNames[k];
            ui->set_next_width(150.0f);
            ui->combo("##ppitch", &g_practicePitch, choices, kPitchKinds - 2);
            ui->same_line();
            ui->text("값");
            ui->same_line();
            ui->set_next_width(56.0f);
            if (ui->input_int("##pvalue", &g_practiceValue, 0))
                g_practiceValue = std::clamp(g_practiceValue, 1, kValueMax);
            ui->same_line();
            ui->text("상한");
            ui->same_line();
            ui->set_next_width(56.0f);
            if (ui->input_int("##plimit", &g_practiceLimit, 0))
                g_practiceLimit = std::clamp(g_practiceLimit, 1, kValueMax);
            ui->same_line();
            ui->text("득의도");
            ui->same_line();
            ui->set_next_width(50.0f);
            ui->combo("##papt", &g_practiceApt, kAptitudes, 5);
            ui->same_line();
            ui->begin_disabled(pitchers == 0 ? 1 : 0);
            if (ui->button(targets.size() == 1u ? "새 구종 적용" : "고른 투수 모두에게 적용"))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const Row *row : targets)
                    if (row->pitcher)
                        g_practices.push_back(Practice{row->id, g_practicePitch + 2, g_practiceValue, g_practiceLimit, g_practiceApt, -1, 0});
            }
            ui->end_disabled();
            ui->tooltip("빈 변화구 칸(10개 중)에 넣습니다. 이미 던지는 구종이거나 칸이 다 찼으면 넣지 않고 '최근 변화'에 남깁니다.");
            ui->separator_text("타석 바꾸기 (스위치 타자 전향)");
            if (targets.size() == 1u)
                ui->text_disabled((std::string("지금: ") + kBatsNames[std::clamp(targets[0]->bats, 0, 2)]).c_str());
            ui->set_next_width(150.0f);
            ui->combo("##pbats", &g_practiceBats, kBatsNames, 3);
            ui->same_line();
            if (ui->button(targets.size() == 1u ? "타석 적용" : "고른 선수 모두에게 적용##bats"))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const Row *row : targets)
                    g_practices.push_back(Practice{row->id, 0, 0, 0, 0, g_practiceBats, 0});
            }
            ui->tooltip("투수도 타석이 있습니다. 결과는 위 '최근 변화'에 메모리에서 다시 읽은 값으로 남습니다.");
            ui->separator_text("캠프에서 게임 판정으로 (6명 제한 없이)");
            ui->text_disabled("캠프 특수 연습 결과가 나올 때(캠프 마지막 단계) 게임의 6칸을 먼저 판정한 뒤, 이 대기열 선수도 게임의 성공·실패 판정을 그대로 거칩니다.");
            ui->text_disabled("새 구종의 값·상한·득의도는 게임이 정합니다 (위 칸의 값은 안 씀). 결과는 '최근 변화'에 남고, 판정한 선수는 대기열에서 빠집니다.");
            ui->begin_disabled(pitchers == 0 ? 1 : 0);
            if (ui->button(("새 구종 '" + std::string(kPitchNames[g_practicePitch + 2]) + "' 캠프 판정 대기에 넣기").c_str()))
                queueCamp(targets, 0, g_practicePitch + 2);
            ui->end_disabled();
            ui->tooltip("고른 선수 중 내 구단 투수만 넣습니다. 구종은 위 '새 구종 추가'에서 고른 것. 한 선수는 하나만: 다시 넣으면 바뀝니다.");
            ui->same_line();
            if (ui->button("스위치 전향 캠프 판정 대기에 넣기"))
                queueCamp(targets, 1, 0);
            ui->tooltip("게임 기준: 우타자만, 능력 조건(게임이 정한 값)을 채우면 80% 확률. 조건이 안 되면 실패로 남습니다.");
            if (queue.empty())
                ui->text_disabled("캠프 판정 대기열: 비어 있음");
            else
            {
                ui->text(("캠프 판정 대기열 " + std::to_string(queue.size()) + "명").c_str());
                for (const CampWish &wish : queue)
                {
                    ui->push_id(static_cast<int>(wish.id));
                    if (ui->small_button("빼기"))
                        editCamp([id = wish.id](std::vector<CampWish> &camp) {
                            camp.erase(std::remove_if(camp.begin(), camp.end(), [id](const CampWish &w) { return w.id == id; }), camp.end());
                        });
                    ui->pop_id();
                    ui->same_line();
                    ui->text(((wish.name.empty() ? "선수 " + std::to_string(wish.id) : wish.name) + " · " + campWhat(wish.type, wish.param)).c_str());
                }
                if (ui->button("대기열 모두 빼기"))
                    editCamp([](std::vector<CampWish> &camp) { camp.clear(); });
            }
        }

        void drawPlayerCard(const Pcsx2GsUi *ui, Settings &settings, const Row &row, int own, bool &changed)
        {
            resetDrafts(row.id);
            char head[160];
            std::snprintf(head, sizeof(head), "%d  %s", row.number, row.name.empty() ? "?" : row.name.c_str());
            ui->heading(head);
            const uint32_t items = itemsFor(settings, row, own);
            const auto pin = settings.pins.find(row.id);
            const bool excluded = pin != settings.pins.end() && !pin->second.on;
            const bool groupLocked = settings.enabled && groupApplies(settings, row.team, own) && !excluded;
            std::snprintf(head, sizeof(head), "%s · %s · %d세 · ID %u", kTeamNames[row.team], row.pitcher ? "투수" : kPositions[row.position],
                          row.age, row.id);
            ui->text_disabled(head);
            const int locks = lockCount(settings, row.id);
            std::string state = !settings.enabled ? "선수 치트 꺼짐: 켜야 고정·S+ 항목이 써집니다 ('적용'은 바로 씀)"
                                : pin != settings.pins.end() ? (pin->second.on ? "S+ 항목·스킬: 이 선수만 따로 (" + std::to_string(std::popcount(items)) + "항목)"
                                                                               : std::string("이 선수 적용 끔 (S+ 항목·단체 고정 안 받음)"))
                                : items != 0u ? "S+ 항목·스킬: 단체 고정 (" + std::to_string(std::popcount(items)) + "항목)"
                                : !settings.groupOn && inScope(settings, row.team, own)
                                    ? std::string("단체 고정 꺼짐: 이 선수에게는 '적용'·'고정'한 것만 씁니다")
                                    : std::string("S+ 항목: 대상 아님");
            if (settings.enabled)
                state += " · 개별 고정 " + std::to_string(locks) + "개" + (groupLocked && !settings.groupStats.empty() ? " · 단체 고정 " + std::to_string(settings.groupStats.size()) + "개" : "");
            if (settings.ownOnly && row.team != own && (locks > 0 || pin != settings.pins.end()))
                state += " · 내 구단 선수가 아니라 개별 고정·따로 정한 항목은 멈춤";
            ui->text_colored(kChangedText, state.c_str());
            const auto edit = settings.edits.find(row.id);
            if (edit != settings.edits.end())
            {
                int on = edit->second.on ? 1 : 0;
                if (ui->checkbox("이 선수의 개별 고정 쓰기", &on))
                {
                    edit->second.on = on != 0;
                    changed = true;
                }
                ui->same_line();
                if (ui->small_button("이 선수 고정 모두 해제"))
                {
                    settings.edits.erase(edit);
                    g_drafts.clear();
                    changed = true;
                }
            }
            if (!ui->begin_tab_bar("card"))
                return;
            if (ui->begin_tab("능력치 (수치·등급)"))
            {
                ui->text("빠른 입력:");
                ui->same_line();
                ui->set_next_width(58.0f);
                ui->combo("##fill", &g_fillGrade, kGrades, kGradeCount);
                const int value = kGradeTop[g_fillGrade];
                const struct
                {
                    const char *label;
                    int group;
                } fills[] = {{"투수 능력에", kPitching}, {"타자 능력에", kBatting}, {"수비 위치에", kFielding}, {"변화구에", kPitches}};
                for (const auto &fill : fills)
                {
                    ui->same_line();
                    if (ui->small_button(fill.label))
                        fillDrafts(&row, fill.group, value, settings, row.id);
                }
                ui->same_line();
                ui->text_disabled("(새 값 칸에 넣기만 함)");
                int dirty = 0;
                for (int stat = 0; stat < kStatCount; ++stat)
                    if (const auto it = g_drafts.find(stat); it != g_drafts.end() && shownStat(&row, stat) && draftChanged(it->second, row, stat))
                        ++dirty;
                ui->begin_disabled(dirty == 0 ? 1 : 0);
                if (ui->button(("바꾼 능력 " + std::to_string(dirty) + "개 고정").c_str()))
                {
                    for (const auto &[stat, draft] : g_drafts)
                        if (shownStat(&row, stat) && draftChanged(draft, row, stat))
                        {
                            const StatEdit lock = editFrom(draft, &row, stat);
                            setLock(settings, row.id, row.name, stat, &lock);
                        }
                    changed = true;
                }
                ui->same_line();
                if (ui->button(("바꾼 능력 " + std::to_string(dirty) + "개 1회 적용").c_str()))
                    for (const auto &[stat, draft] : g_drafts)
                        if (shownStat(&row, stat) && draftChanged(draft, row, stat))
                            queueEdit(row.id, stat, editFrom(draft, &row, stat));
                ui->end_disabled();
                ui->same_line();
                if (ui->button("입력 되돌리기"))
                    g_drafts.clear();
                ui->tooltip("새 값·상한·득의도 칸을 지금 값(고정 중이면 고정 값)으로 되돌립니다.");
                const int order[2][4] = {{kPitching, kPitches, kBatting, kFielding}, {kBatting, kFielding, kPitching, kPitches}};
                for (int group : order[row.pitcher ? 0 : 1])
                {
                    ui->separator_text(groupTitle(group));
                    if (group == kPitches && std::none_of(std::begin(kStats), std::end(kStats), [&](const Stat &stat) {
                            return stat.group == kPitches && row.value(static_cast<int>(&stat - kStats), 0) != 0;
                        }))
                    {
                        ui->text_disabled("변화구 없음");
                        continue;
                    }
                    drawStatTable(ui, settings, row, group, items, groupLocked, changed);
                }
                ui->end_tab();
            }
            if (ui->begin_tab("S+ 항목 · 스킬"))
            {
                drawPinTab(ui, settings, row, own, false, changed);
                ui->end_tab();
            }
            if (ui->begin_tab("특훈 (구종·타석)"))
            {
                drawPractice(ui, {&row}, settings.camp);
                ui->end_tab();
            }
            if (ui->begin_tab("스킬 슬롯"))
            {
                drawPinTab(ui, settings, row, own, true, changed);
                ui->end_tab();
            }
            ui->end_tab_bar();
        }

        // The ticked players together: abilities (ticked rows only), locks for all of them, S+ items of their own.
        void drawBulkCard(const Pcsx2GsUi *ui, Settings &settings, const std::vector<const Row *> &targets, bool &changed)
        {
            resetDrafts(0);
            const int count = static_cast<int>(targets.size());
            ui->heading(("고른 선수 " + std::to_string(count) + "명 일괄 편집").c_str());
            std::string names;
            for (int i = 0; i < count && i < 8; ++i)
                names += (i ? ", " : "") + targets[static_cast<size_t>(i)]->name;
            if (count > 8)
                names += " 외 " + std::to_string(count - 8) + "명";
            ui->text_wrapped(names.c_str());
            if (count == 0)
            {
                ui->text_disabled("왼쪽 목록에서 선수를 체크하세요 (Ctrl+클릭, Shift+클릭으로 여러 명).");
                return;
            }
            if (!ui->begin_tab_bar("bulk"))
                return;
            if (ui->begin_tab("능력치 (수치·등급)"))
            {
                ui->text("빠른 입력:");
                ui->same_line();
                ui->set_next_width(58.0f);
                ui->combo("##fill", &g_fillGrade, kGrades, kGradeCount);
                const struct
                {
                    const char *label;
                    int group;
                } fills[] = {{"투수 능력에", kPitching}, {"타자 능력에", kBatting}, {"수비 위치에", kFielding}};
                for (const auto &fill : fills)
                {
                    ui->same_line();
                    if (ui->small_button(fill.label))
                        fillDrafts(nullptr, fill.group, kGradeTop[g_fillGrade], settings, 0u);
                }
                ui->same_line();
                if (ui->small_button("체크 모두 해제"))
                    for (auto &[stat, draft] : g_drafts)
                        draft.use = false;
                int used = 0;
                for (const auto &[stat, draft] : g_drafts)
                    used += draft.use ? 1 : 0;
                ui->begin_disabled(used == 0 ? 1 : 0);
                if (ui->button(("체크한 능력 " + std::to_string(used) + "개 고정 (" + std::to_string(count) + "명)").c_str()))
                {
                    for (const auto &[stat, draft] : g_drafts)
                        if (draft.use)
                            for (const Row *row : targets)
                            {
                                const StatEdit lock = editFrom(draft, nullptr, stat);
                                setLock(settings, row->id, row->name, stat, &lock);
                            }
                    changed = true;
                }
                ui->same_line();
                if (ui->button(("체크한 능력 1회 적용 (" + std::to_string(count) + "명)").c_str()))
                    for (const auto &[stat, draft] : g_drafts)
                        if (draft.use)
                            for (const Row *row : targets)
                                queueEdit(row->id, stat, editFrom(draft, nullptr, stat));
                ui->same_line();
                if (ui->button(("체크한 능력 고정 해제 (" + std::to_string(count) + "명)").c_str()))
                {
                    for (const auto &[stat, draft] : g_drafts)
                        if (draft.use)
                            for (const Row *row : targets)
                                setLock(settings, row->id, row->name, stat, nullptr);
                    changed = true;
                }
                ui->end_disabled();
                ui->text_disabled("체크한 능력만 씁니다. 득의도 '그대로'는 선수마다 원래 값을 둡니다. 상한은 새 값보다 낮을 때만 올립니다.");
                static const char *const aptitudes[6] = {"그대로", "D", "C", "B", "A", "S"};
                for (int group : {kPitching, kBatting, kFielding})
                {
                    ui->separator_text(groupTitle(group));
                    ui->push_id(600 + group);
                    if (ui->begin_table("bulkstats", 7, PCSX2GS_TABLE_ROW_BG | PCSX2GS_TABLE_BORDERS, 0.0f, 0.0f))
                    {
                        ui->table_column("쓰기", 0, 36.0f);
                        ui->table_column("능력", 0, 86.0f);
                        ui->table_column("지금 (최소~최대)", 0, 120.0f);
                        ui->table_column("새 값", 0, 60.0f);
                        ui->table_column("등급", 0, 62.0f);
                        ui->table_column("득의도", 0, 70.0f);
                        ui->table_column("고정 중", 0, 0.0f);
                        ui->table_headers();
                        for (int stat = 0; stat < kStatCount; ++stat)
                        {
                            if (kStats[stat].group != group)
                                continue;
                            int low = 255, high = 0, locked = 0;
                            for (const Row *row : targets)
                            {
                                low = std::min(low, row->value(stat));
                                high = std::max(high, row->value(stat));
                                locked += lockOf(settings, row->id, stat) ? 1 : 0;
                            }
                            auto it = g_drafts.find(stat);
                            if (it == g_drafts.end())
                            {
                                Draft draft;
                                draft.value = high;
                                draft.apt = -1;
                                it = g_drafts.emplace(stat, draft).first;
                            }
                            Draft &draft = it->second;
                            ui->push_id(stat);
                            ui->table_next_row();
                            if (locked == count)
                                ui->table_row_color(kLockedRow);
                            ui->table_next_column();
                            int use = draft.use ? 1 : 0;
                            if (ui->checkbox("##use", &use))
                                draft.use = use != 0;
                            ui->table_next_column();
                            ui->text(kStats[stat].label);
                            ui->tooltip(kStats[stat].jp);
                            ui->table_next_column();
                            ui->text_colored(gradeColor(low), valueText(stat, low).c_str());
                            if (high != low)
                            {
                                ui->same_line();
                                ui->text_colored(gradeColor(high), ("~ " + valueText(stat, high)).c_str());
                            }
                            ui->table_next_column();
                            ui->set_next_width(56.0f);
                            if (ui->input_int("##value", &draft.value, 0))
                            {
                                draft.value = std::clamp(draft.value, 0, kValueMax);
                                draft.use = true;
                            }
                            ui->table_next_column();
                            int rank = gradeOf(draft.value);
                            ui->set_next_width(58.0f);
                            if (ui->combo("##rank", &rank, kGrades, kGradeCount))
                            {
                                draft.value = kGradeTop[rank];
                                draft.use = true;
                            }
                            ui->table_next_column();
                            int apt = draft.apt + 1;
                            ui->set_next_width(66.0f);
                            if (ui->combo("##apt", &apt, aptitudes, 6))
                            {
                                draft.apt = apt - 1;
                                draft.use = true;
                            }
                            ui->table_next_column();
                            if (locked > 0)
                                ui->text_colored(kChangedText, (std::to_string(locked) + " / " + std::to_string(count) + "명").c_str());
                            ui->pop_id();
                        }
                        ui->end_table();
                    }
                    ui->pop_id();
                }
                ui->end_tab();
            }
            if (ui->begin_tab("S+ 항목 (따로 정하기)"))
            {
                int pinned = 0;
                for (const Row *row : targets)
                    pinned += settings.pins.count(row->id) ? 1 : 0;
                ui->text_wrapped(("따로 정한 선수 " + std::to_string(pinned) + " / " + std::to_string(count) + "명. 따로 정한 선수는 단체 S+ 항목·스킬 대신 자기 항목을 씁니다.").c_str());
                if (ui->button("고른 선수 모두 따로 정하기 (지금 단체 항목 복사)"))
                {
                    for (const Row *row : targets)
                        if (!settings.pins.count(row->id))
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
                if (ui->button("고른 선수 따로 정하기 해제"))
                {
                    for (const Row *row : targets)
                        settings.pins.erase(row->id);
                    changed = true;
                }
                if (pinned == count)
                {
                    // Shown: the first one's items; a click sets or clears that item for every ticked player.
                    Pin &first = settings.pins[targets.front()->id];
                    uint32_t mask = first.mask;
                    std::vector<uint16_t> skills = first.skills;
                    bool itemsChanged = false;
                    ui->text_disabled("아래 체크는 첫 선수 기준으로 보이고, 바꾸면 고른 선수 모두에게 같은 항목을 켜고 끕니다. 스킬은 선수 카드에서 하나씩.");
                    drawItems(ui, mask, skills, itemsChanged);
                    if (itemsChanged)
                    {
                        const uint32_t on = mask & ~first.mask, off = first.mask & ~mask;
                        for (const Row *row : targets)
                        {
                            Pin &pin = settings.pins[row->id];
                            pin.mask = (pin.mask | on) & ~off;
                        }
                        changed = true;
                    }
                }
                ui->end_tab();
            }
            if (ui->begin_tab("특훈 (구종·타석)"))
            {
                drawPractice(ui, targets, settings.camp);
                ui->end_tab();
            }
            ui->end_tab_bar();
        }

        enum Column
        {
            cPick = 0, cNumber, cName, cTeam, cPos, cAge, cStamina, cSpeedP, cStuff, cControl, cContact, cPower, cSpeed, cArm,
            cEye, cField, cState, cColumns
        };
        int fieldValue(const Row &row) { return row.value(row.pitcher ? sPosP : sPosP + row.position); }
        int sortKey(const Row &row, int column, const Settings &settings)
        {
            switch (column)
            {
            case cPick: return g_picked.count(row.id) ? 1 : 0;
            case cNumber: return row.number;
            case cTeam: return row.team;
            case cPos: return row.pitcher ? 0 : row.position;
            case cAge: return row.age;
            case cStamina: return row.value(sStamina);
            case cSpeedP: return row.value(sSpeedP);
            case cStuff: return row.value(sStuff);
            case cControl: return row.value(sControl);
            case cContact: return row.value(sContactL) + row.value(sContactR);
            case cPower: return row.value(sPower);
            case cSpeed: return row.value(sSpeed);
            case cArm: return row.value(sArm);
            case cEye: return row.value(sEye);
            case cField: return fieldValue(row);
            case cState: return lockCount(settings, row.id) + (settings.pins.count(row.id) ? 100 : 0);
            default: return 0;
            }
        }

        bool matches(const Row &row, const std::string &query)
        {
            if (query.empty())
                return true;
            if (std::all_of(query.begin(), query.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return std::atoi(query.c_str()) == row.number || std::atoi(query.c_str()) == row.id;
            std::string name = row.name, wanted = query;
            for (char &c : name)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char &c : wanted)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string compact;
            for (char c : name)
                if (c != ' ')
                    compact += c;
            return name.find(wanted) != std::string::npos || compact.find(wanted) != std::string::npos;
        }

        void drawList(const Pcsx2GsUi *ui, Settings &settings, std::vector<const Row *> list, int own)
        {
            const uint32_t flags = PCSX2GS_TABLE_SORTABLE | PCSX2GS_TABLE_SCROLL | PCSX2GS_TABLE_SCROLL_X | PCSX2GS_TABLE_ROW_BG |
                                   PCSX2GS_TABLE_BORDERS | PCSX2GS_TABLE_RESIZABLE;
            if (!ui->begin_table("roster", cColumns, flags, 0.0f, 0.0f))
                return;
            ui->table_freeze(3, 1);
            ui->table_column("고름", 0, 30.0f);
            ui->table_column("#", 0, 30.0f);
            ui->table_column("이름", 0, 116.0f);
            ui->table_column("구단", 0, 70.0f);
            ui->table_column("위치", 0, 32.0f);
            ui->table_column("나이", PCSX2GS_COLUMN_DESCENDING_FIRST, 32.0f);
            const char *abilities[] = {"체력", "구속", "구위", "제구", "교타", "장타", "주력", "어깨", "선구", "수비"};
            for (int i = 0; i < 10; ++i)
                ui->table_column(abilities[i], PCSX2GS_COLUMN_DESCENDING_FIRST, i == 1 ? 66.0f : 48.0f);
            ui->table_column("설정", PCSX2GS_COLUMN_DESCENDING_FIRST, 96.0f);
            ui->table_headers();
            int column = -1, down = 0;
            ui->table_sort(&column, &down);
            if (column == cName)
                std::stable_sort(list.begin(), list.end(), [&](const Row *a, const Row *b) { return down ? a->name > b->name : a->name < b->name; });
            else if (column >= 0)
                std::stable_sort(list.begin(), list.end(), [&](const Row *a, const Row *b) {
                    const int x = sortKey(*a, column, settings), y = sortKey(*b, column, settings);
                    return down ? x > y : x < y;
                });
            ui->clipper_begin(static_cast<int>(list.size()));
            int first = 0, end = 0;
            while (ui->clipper_step(&first, &end))
                for (int i = first; i < end; ++i)
                {
                    const Row &row = *list[static_cast<size_t>(i)];
                    ui->table_next_row();
                    const int locks = lockCount(settings, row.id);
                    const bool pinned = settings.pins.count(row.id) != 0u;
                    if (locks > 0)
                        ui->table_row_color(kLockedRow);
                    else if (pinned)
                        ui->table_row_color(kPinnedRow);
                    ui->push_id(row.id);
                    ui->table_next_column();
                    int ticked = g_picked.count(row.id) ? 1 : 0;
                    if (ui->checkbox("##pick", &ticked))
                    {
                        if (ticked)
                            g_picked.insert(row.id);
                        else
                            g_picked.erase(row.id);
                        g_lastClick = i;
                    }
                    ui->table_next_column();
                    char label[24];
                    std::snprintf(label, sizeof(label), "%d##row", row.number);
                    if (ui->selectable(label, !g_bulk && row.id == g_focus ? 1 : 0, PCSX2GS_SELECT_SPAN_COLUMNS))
                    {
                        const uint32_t mods = ui->key_mods();
                        if ((mods & PCSX2GS_KEY_SHIFT) && g_lastClick >= 0 && g_lastClick < static_cast<int>(list.size()))
                        {
                            for (int k = std::min(g_lastClick, i); k <= std::max(g_lastClick, i); ++k)
                                g_picked.insert(list[static_cast<size_t>(k)]->id);
                        }
                        else if (mods & PCSX2GS_KEY_CTRL)
                        {
                            if (!g_picked.erase(row.id))
                                g_picked.insert(row.id);
                        }
                        else
                        {
                            g_focus = row.id;
                            g_bulk = false;
                        }
                        g_lastClick = i;
                    }
                    ui->table_next_column();
                    ui->text(row.name.empty() ? "?" : row.name.c_str());
                    ui->table_next_column();
                    ui->text(own == row.team ? (std::string(kTeamNames[row.team]) + "*").c_str() : kTeamNames[row.team]);
                    ui->table_next_column();
                    ui->text(row.pitcher ? "투" : kPositions[row.position]);
                    ui->table_next_column();
                    ui->text(std::to_string(row.age).c_str());
                    const int cells[10] = {row.value(sStamina), row.value(sSpeedP), row.value(sStuff), row.value(sControl),
                                           (row.value(sContactL) + row.value(sContactR)) / 2, row.value(sPower), row.value(sSpeed),
                                           row.value(sArm), row.value(sEye), fieldValue(row)};
                    for (int k = 0; k < 10; ++k)
                    {
                        ui->table_next_column();
                        ui->text_colored(gradeColor(cells[k]), valueText(k == 1 ? sSpeedP : -1, cells[k]).c_str());
                    }
                    ui->table_next_column();
                    std::string state;
                    if (locks > 0)
                        state = "고정 " + std::to_string(locks);
                    if (pinned)
                        state += std::string(state.empty() ? "" : " · ") + (settings.pins.at(row.id).on ? "따로" : "끔");
                    else if (settings.enabled && groupApplies(settings, row.team, own))
                        state += std::string(state.empty() ? "" : " · ") + "단체";
                    ui->text_disabled(state.c_str());
                    ui->pop_id();
                }
            ui->end_table();
        }

        void drawFinder(const Pcsx2GsUi *ui, Settings &settings, const std::array<std::vector<Row>, kTeams> &rows, int own, bool &changed)
        {
            ui->set_next_width(210.0f);
            ui->input_text("##search", "이름 · 등번호 · ID", g_search, sizeof(g_search));
            ui->tooltip("이름 일부(일본어 그대로), 등번호, 선수 ID. 비우면 모두.");
            ui->same_line();
            std::vector<std::string> teamLabels{"모든 구단", own >= 0 ? std::string("내 구단 (") + kTeamNames[own] + ")" : std::string("내 구단")};
            for (int t = 0; t < kTeams; ++t)
                teamLabels.push_back(std::string(kTeamNames[t]) + " " + std::to_string(rows[static_cast<size_t>(t)].size()) + "명");
            std::vector<const char *> teamPointers;
            for (const std::string &label : teamLabels)
                teamPointers.push_back(label.c_str());
            ui->set_next_width(150.0f);
            ui->combo("##team", &g_teamFilter, teamPointers.data(), static_cast<int>(teamPointers.size()));
            ui->same_line();
            const char *positions[] = {"모든 위치", "투수", "야수", "포수", "1루수", "2루수", "3루수", "유격수", "외야수"};
            ui->set_next_width(100.0f);
            ui->combo("##pos", &g_posFilter, positions, 9);
            ui->tooltip("야수의 위치는 수비 적성이 가장 높은 곳입니다.");
            ui->same_line();
            ui->checkbox("설정한 선수만", &g_onlyEdited);
            ui->same_line();
            ui->checkbox("고른 선수만", &g_onlyPicked);

            const std::string query = trim(g_search);
            std::vector<const Row *> shown;
            for (int t = 0; t < kTeams; ++t)
            {
                if (g_teamFilter == 1 ? t != own : g_teamFilter >= 2 && t != g_teamFilter - 2)
                    continue;
                for (const Row &row : rows[static_cast<size_t>(t)])
                {
                    if (g_posFilter == 1 && !row.pitcher)
                        continue;
                    if (g_posFilter == 2 && row.pitcher)
                        continue;
                    if (g_posFilter >= 3 && (row.pitcher || row.position != g_posFilter - 2))
                        continue;
                    if (g_onlyEdited && !settings.edits.count(row.id) && !settings.pins.count(row.id))
                        continue;
                    if (g_onlyPicked && !g_picked.count(row.id))
                        continue;
                    if (!matches(row, query))
                        continue;
                    shown.push_back(&row);
                }
            }
            ui->text_disabled(("보이는 선수 " + std::to_string(shown.size()) + "명 · 고른 선수 " + std::to_string(g_picked.size()) + "명").c_str());
            ui->same_line();
            if (ui->small_button("보이는 선수 모두 고르기"))
                for (const Row *row : shown)
                    g_picked.insert(row->id);
            ui->same_line();
            if (ui->small_button("고르기 모두 해제"))
                g_picked.clear();
            if (!g_picked.empty())
            {
                ui->same_line();
                if (ui->button_colored(("고른 " + std::to_string(g_picked.size()) + "명 일괄 편집").c_str(), g_bulk ? 0xFF1A1FC9u : 0u, 0.0f))
                    g_bulk = true;
            }
            ui->same_line();
            ui->text_disabled("행 클릭 = 카드 열기 · Ctrl+클릭 / Shift+클릭 = 여러 명 고르기 · 머리줄 클릭 = 정렬");

            float width = 0.0f, height = 0.0f;
            ui->avail(&width, &height);
            const float listWidth = std::max(width * 0.56f, 360.0f);
            ui->begin_panel("list", listWidth, 0.0f, 0);
            if (shown.empty())
                ui->text_disabled(own < 0 && rows[0].empty() ? "게임을 시작하면(세이브 불러오기·새 게임) 명단이 나옵니다." : "조건에 맞는 선수가 없습니다.");
            else
                drawList(ui, settings, shown, own);
            ui->end_child();
            ui->same_line();
            ui->begin_panel("card", 0.0f, 0.0f, 1);
            if (g_bulk)
            {
                std::vector<const Row *> targets;
                for (uint16_t id : g_picked)
                    if (const Row *row = findRow(rows, id))
                        targets.push_back(row);
                if (ui->small_button("< 한 선수 카드로"))
                    g_bulk = false;
                drawBulkCard(ui, settings, targets, changed);
            }
            else if (const Row *row = findRow(rows, g_focus))
                drawPlayerCard(ui, settings, *row, own, changed);
            else
            {
                ui->heading("선수 카드");
                ui->text_wrapped("왼쪽 목록에서 선수를 클릭하면 여기에 능력치 편집 카드가 열립니다.");
                ui->text_disabled("새 값(숫자) 또는 등급을 고른 뒤: '적용' = 한 번만 씀, '고정' = 매 프레임 유지.");
                ui->text_disabled("여러 명을 체크하고 '일괄 편집'을 누르면 같은 값을 한 번에 씁니다.");
            }
            ui->end_child();
        }

        // Who gets the default items, the items and skills, and the group's ability locks.
        void drawGroupTab(const Pcsx2GsUi *ui, Settings &settings, int team, bool &changed)
        {
            // Edit freely below; nothing is written before 단체 적용 및 고정 (owner, 11-06).
            const GroupApplied draft = groupDraft(settings);
            const bool pending = !(draft == settings.applied);
            if (ui->button_colored(settings.groupOn && !pending ? "단체 적용 및 고정 (지금 그대로)" : "단체 적용 및 고정", pending || !settings.groupOn ? 0xFF3A6AD0u : 0u, 0.0f))
            {
                settings.applied = draft;
                settings.groupOn = true;
                changed = true;
            }
            ui->tooltip("아래에서 고른 대상·S+ 항목·스킬·능력 고정을 그대로 받아 대상 전원에게 매 프레임 씁니다. 누르기 전에는 아무것도 바뀌지 않습니다.");
            if (settings.groupOn)
            {
                ui->same_line();
                if (ui->button("단체 고정 끄기"))
                {
                    settings.groupOn = false;
                    changed = true;
                }
                ui->tooltip("대상 전원에게 쓰기를 멈춥니다. '끄거나 빼면 원래 능력치로'를 켜 두면 이번 실행에서 처음 쓰기 전 값으로 돌아갑니다.");
            }
            if (settings.groupOn)
                ui->text_colored(0xFF5050FFu, ("단체 고정 켜짐: 적용한 대상 전원에게 S+ 항목 " + std::to_string(std::popcount(settings.applied.mask)) +
                                               "개 · 스킬 " + std::to_string(settings.applied.skills.size()) + "개 · 능력 고정 " +
                                               std::to_string(settings.applied.stats.size()) + "개를 쓰는 중")
                                                  .c_str());
            else
                ui->text_disabled("단체 고정 꺼짐: 선수 카드에서 한 명씩 '적용'·'고정'한 것만 씁니다.");
            if (pending && settings.groupOn)
                ui->text_colored(kChangedText, "아래에서 바꾼 내용은 아직 적용 안 됨: '단체 적용 및 고정'을 눌러야 씁니다.");
            ui->separator_text("대상 고르기: 단체 설정을 받을 선수");
            const int mode = settings.all ? settings.scope : 3;
            const char *modes[4] = {"내 구단 전원", "고른 구단 전원", "모든 구단 모든 선수", "따로 정한 선수만"};
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
                    if (t % 6 != 0)
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
            ui->text_colored(kChangedText, ("단체 대상: " + scopeText(settings, team)).c_str());
            ui->text_disabled("'따로 정한 선수'(선수 카드의 S+ 항목 탭)는 단체 S+ 항목·스킬 대신 자기 것을 씁니다. 개별 고정은 늘 맨 위.");
            if (ui->collapsing_header("S+ 항목 · 스킬 (단체 대상 전원)", 1))
                drawItems(ui, settings.mask, settings.skills, changed);
            if (ui->collapsing_header(("능력치 수치·등급 고정 (단체 대상 전원) · " + std::to_string(settings.groupStats.size()) + "개").c_str(), 1))
            {
                ui->text_disabled("'쓰기'를 켠 능력을 대상 전원에게 매 프레임 그 값으로 (S+ 항목보다 우선, 선수 카드의 개별 고정이 더 우선).");
                if (!settings.groupStats.empty())
                {
                    ui->same_line();
                    if (ui->small_button("단체 고정 모두 해제"))
                    {
                        settings.groupStats.clear();
                        changed = true;
                    }
                }
                static const char *const aptitudes[6] = {"그대로", "D", "C", "B", "A", "S"};
                for (int group : {kPitching, kBatting, kFielding})
                {
                    ui->separator_text(groupTitle(group));
                    ui->push_id(700 + group);
                    if (ui->begin_table("groupstats", 5, PCSX2GS_TABLE_ROW_BG | PCSX2GS_TABLE_BORDERS, 0.0f, 0.0f))
                    {
                        ui->table_column("쓰기", 0, 36.0f);
                        ui->table_column("능력", 0, 90.0f);
                        ui->table_column("새 값", 0, 60.0f);
                        ui->table_column("등급", 0, 62.0f);
                        ui->table_column("득의도", 0, 0.0f);
                        ui->table_headers();
                        for (int stat = 0; stat < kStatCount; ++stat)
                        {
                            if (kStats[stat].group != group)
                                continue;
                            const auto locked = settings.groupStats.find(stat);
                            auto it = g_groupDrafts.find(stat);
                            if (it == g_groupDrafts.end())
                            {
                                Draft draft;
                                draft.value = locked != settings.groupStats.end() && locked->second.value >= 0 ? locked->second.value : 99;
                                draft.apt = locked != settings.groupStats.end() ? locked->second.apt : -1;
                                it = g_groupDrafts.emplace(stat, draft).first;
                            }
                            Draft &draft = it->second;
                            ui->push_id(stat);
                            ui->table_next_row();
                            if (locked != settings.groupStats.end())
                                ui->table_row_color(kLockedRow);
                            ui->table_next_column();
                            int use = locked != settings.groupStats.end() ? 1 : 0;
                            bool typed = false;
                            const bool toggled = ui->checkbox("##use", &use) != 0;
                            ui->table_next_column();
                            ui->text(kStats[stat].label);
                            ui->tooltip(kStats[stat].jp);
                            ui->table_next_column();
                            ui->set_next_width(56.0f);
                            if (ui->input_int("##value", &draft.value, 0))
                            {
                                draft.value = std::clamp(draft.value, 0, kValueMax);
                                typed = true;
                            }
                            ui->table_next_column();
                            int rank = gradeOf(draft.value);
                            ui->set_next_width(58.0f);
                            if (ui->combo("##rank", &rank, kGrades, kGradeCount))
                            {
                                draft.value = kGradeTop[rank];
                                typed = true;
                            }
                            ui->table_next_column();
                            int apt = draft.apt + 1;
                            ui->set_next_width(66.0f);
                            if (ui->combo("##apt", &apt, aptitudes, 6))
                            {
                                draft.apt = apt - 1;
                                typed = true;
                            }
                            if (toggled || (typed && use))
                            {
                                if (use)
                                    settings.groupStats[stat] = editFrom(draft, nullptr, stat);
                                else
                                    settings.groupStats.erase(stat);
                                changed = true;
                            }
                            ui->pop_id();
                        }
                        ui->end_table();
                    }
                    ui->pop_id();
                }
            }
        }

        // Everyone with settings of their own, on a roster now or not.
        void drawSummary(const Pcsx2GsUi *ui, Settings &settings, const std::array<std::vector<Row>, kTeams> &rows, int team, bool &changed)
        {
            ui->text_colored(kChangedText, ("단체: " + scopeText(settings, team) + " · S+ 항목 " + std::to_string(std::popcount(settings.mask)) +
                                            "개 · 스킬 " + std::to_string(settings.skills.size()) + "개 · 능력 고정 " +
                                            std::to_string(settings.groupStats.size()) + "개")
                                               .c_str());
            for (const auto &[stat, edit] : settings.groupStats)
                ui->text_disabled(("  " + std::string(kStats[stat].label) + ": " + editText(edit)).c_str());
            std::set<uint16_t> ids;
            for (const auto &[id, pin] : settings.pins)
                ids.insert(id);
            for (const auto &[id, edit] : settings.edits)
                ids.insert(id);
            ui->separator_text(("개별 설정한 선수 " + std::to_string(ids.size()) + "명").c_str());
            if (ids.empty())
                ui->text_disabled("아직 없습니다. '선수 찾기·편집'에서 선수 카드를 열어 고정하거나 따로 정하세요.");
            if (!ui->begin_panel("summary", 0.0f, 0.0f, 0))
            {
                ui->end_child();
                return;
            }
            for (uint16_t id : ids)
            {
                const Row *row = findRow(rows, id);
                const auto pin = settings.pins.find(id);
                const auto edit = settings.edits.find(id);
                std::string name = row ? row->name : pin != settings.pins.end() ? pin->second.name : edit->second.name;
                if (name.empty())
                    name = "ID " + std::to_string(id);
                ui->push_id(id);
                std::string line = name + " · " + (row ? kTeamNames[row->team] : "로스터에 없음 (돌아오면 다시 적용)");
                if (pin != settings.pins.end())
                    line += pin->second.on ? " · S+ 따로 " + std::to_string(std::popcount(pin->second.mask)) + "항목 · 스킬 " + std::to_string(pin->second.skills.size())
                                           : std::string(" · 적용 끔");
                ui->text(line.c_str());
                if (row)
                {
                    ui->same_line();
                    if (ui->small_button("카드 열기"))
                    {
                        g_focus = id;
                        g_bulk = false;
                    }
                }
                if (edit != settings.edits.end())
                {
                    ui->same_line();
                    if (ui->small_button("고정 모두 해제"))
                    {
                        settings.edits.erase(edit);
                        changed = true;
                        ui->pop_id();
                        break;
                    }
                }
                if (pin != settings.pins.end())
                {
                    ui->same_line();
                    if (ui->small_button("따로 정하기 해제"))
                    {
                        settings.pins.erase(pin);
                        changed = true;
                        ui->pop_id();
                        break;
                    }
                }
                if (edit != settings.edits.end())
                {
                    std::string locks = edit->second.on ? "  고정: " : "  고정 (꺼 둠): ";
                    int n = 0;
                    for (const auto &[stat, value] : edit->second.stats)
                        locks += (n++ ? ", " : "") + std::string(kStats[stat].label) + " " + editText(value);
                    ui->text_disabled(locks.c_str());
                }
                ui->pop_id();
            }
            ui->end_child();
        }

        // GS thread, while the menu is open. Works on a copy; a change is stored and saved at the end.
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
            if (ui->version < 3u)
            {
                ui->text_wrapped("pcsx2gs.dll이 이 실행 파일보다 오래되었습니다. 다시 빌드하세요.");
                return;
            }
            Settings settings;
            int team;
            std::array<std::vector<Row>, kTeams> rows;
            urban::ChangeLog log;
            bool mismatch = false, hasProfile = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                settings = g_settings;
                team = g_team;
                rows = g_rows;
                log = g_log;
                mismatch = g_teamMismatch;
                hasProfile = !g_settingsPath.empty();
            }
            bool changed = false;
            // Which save these settings belong to (urban::saveid): each save has its own.
            ui->text_disabled(("세이브: " + urban::saveid::describe(urban::saveid::current()) + " · 아래 설정은 이 세이브에만 저장되고 적용됩니다").c_str());
            if (!hasProfile)
                ui->text_disabled("게임이 시작되면 그 세이브의 설정을 엽니다. 지금은 아무것도 쓰지 않습니다.");
            if (mismatch)
            {
                ui->text_colored(0xFF5050FFu, (std::string("이 설정은 ") + kTeamNames[std::clamp(settings.team, 0, kTeams - 1)] +
                                               " 세이브에서 만든 것이라 이 게임(" + (team >= 0 ? kTeamNames[team] : "?") + ")에는 쓰지 않습니다.")
                                                  .c_str());
                ui->same_line();
                if (ui->button("이 게임은 새 게임으로 (세이브 설정은 그대로)"))
                    urban::saveid::startNew();
                ui->tooltip("이 게임을 '새 게임' 설정으로 따로 씁니다 (처음엔 비어 있음). 원래 세이브의 설정은 지우지 않습니다. "
                            "게임 안에서 메모리카드에 세이브하면 그 칸으로 설정이 따라갑니다.");
                ui->same_line();
                const std::string here = team >= 0 && team < kTeams ? kTeamNames[team] : "?";
                const std::string before = settings.team >= 0 && settings.team < kTeams ? kTeamNames[settings.team] : "?";
                if (ui->button(("이 설정을 " + here + " 게임용으로 고치기").c_str()) && team >= 0)
                {
                    // This save is this game now (e.g. a new game saved here while the settings still were another
                    // game's): keep what was set, drop the old team's players' own settings (that game's players).
                    std::set<uint16_t> old;
                    if (settings.team >= 0 && settings.team < kTeams)
                        for (const Row &row : rows[static_cast<size_t>(settings.team)])
                            old.insert(row.id);
                    size_t dropped = 0;
                    for (auto it = settings.pins.begin(); it != settings.pins.end();)
                        if (old.count(it->first) != 0u)
                        {
                            it = settings.pins.erase(it);
                            ++dropped;
                        }
                        else
                            ++it;
                    for (auto it = settings.edits.begin(); it != settings.edits.end();)
                        if (old.count(it->first) != 0u)
                        {
                            it = settings.edits.erase(it);
                            ++dropped;
                        }
                        else
                            ++it;
                    settings.team = team;
                    changed = true;
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_log.add("설정을 " + here + " 게임용으로 고침: " + before + " 선수의 개별 설정 " + std::to_string(dropped) + "건 뺌", "players");
                }
                ui->tooltip(("이 세이브의 설정을 지금 구단(" + here + ") 것으로 고쳐서 씁니다. 원래 구단(" + before +
                             ") 선수에게 걸어 둔 개별 고정·따로 정한 항목은 뺍니다. 다른 구단 선수 설정은 남습니다.")
                                .c_str());
            }
            size_t players = 0;
            for (const std::vector<Row> &list : rows)
                players += list.size();

            int enabled = settings.enabled ? 1 : 0;
            // イチネンモード (one-year mode) runs: its state object [0x62B45C] exists only then. Its new club borrows one
            // team's place, which holds that team's own players until the expansion draft is over.
            const bool ichinen = valid(read32(0x62B45Cu), 0x10u);
            if (ui->checkbox("선수 치트 켜기", &enabled))
            {
                settings.enabled = enabled != 0;
                changed = true;
            }
            ui->tooltip("켜져 있을 때만 S+ 항목·스킬·고정을 매 프레임 씁니다. '적용'(1회)은 꺼져 있어도 바로 씁니다.");
            ui->same_line();
            if (team < 0)
                ui->text_disabled("내 구단을 찾는 중: 세이브를 불러오거나 새 게임을 시작하면 자동으로 찾습니다");
            else
                ui->text_disabled((std::string(ichinen ? "이치넨 구단 (" : "내 구단 ") + kTeamNames[team] + (ichinen ? " 자리)" : " (자동 감지)") +
                                   " · 12개 구단 선수 " + std::to_string(players) + "명")
                                      .c_str());
            ui->same_line();
            int restore = settings.restore ? 1 : 0;
            if (ui->checkbox("끄거나 빼면 원래 능력치로", &restore))
            {
                settings.restore = restore != 0;
                changed = true;
            }
            ui->tooltip("S+ 항목·고정을 끄거나 대상에서 빼면, 이번 실행에서 처음 쓰기 전 값으로 되돌립니다.");
            ui->same_line();
            int ownOnly = settings.ownOnly ? 1 : 0;
            if (ui->checkbox("개별 고정은 내 구단 선수에게만", &ownOnly))
            {
                settings.ownOnly = ownOnly != 0;
                changed = true;
            }
            ui->tooltip("켜면(기본) 선수별 고정·'따로 정한 선수'의 S+ 항목은 그 선수가 내 구단에 있을 때만 씁니다. 트레이드·방출로 CPU 구단에 가면 멈추고, "
                        "다시 오면 이어서 씁니다. 끄면 어느 구단에 있든 그 선수를 따라가서 씁니다 (CPU 구단 선수를 일부러 고정할 때). 단체 고정은 '단체 적용' 탭의 대상대로.");
            if (ichinen)
            {
                ui->text_colored(0xFF5050FFu, "이치넨 모드: 드래프트가 끝나기 전에는 '내 구단' 자리에 원래 구단 선수들이 있습니다.");
                ui->text_colored(0xFF5050FFu, "단체 대상을 '따로 정한 선수만'으로 두고, 드래프트 뒤 내 선수를 고르세요.");
            }
            if (urban::drawRefresh(ui, log, "선수 목록"))
                g_refreshNow = true;
            if (urban::drawChanges(ui, log, "playerchanges", 150.0f))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_log.lines.clear();
            }
            if (ui->begin_tab_bar("playertabs"))
            {
                if (ui->begin_tab("선수 찾기 · 편집"))
                {
                    drawFinder(ui, settings, rows, team, changed);
                    ui->end_tab();
                }
                if (ui->begin_tab("단체 적용"))
                {
                    drawGroupTab(ui, settings, team, changed);
                    ui->end_tab();
                }
                if (ui->begin_tab("설정 현황"))
                {
                    drawSummary(ui, settings, rows, team, changed);
                    ui->end_tab();
                }
                ui->end_tab_bar();
            }

            if (changed)
            {
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    settings.camp = g_settings.camp; // changed only under the lock (the camp hook takes players out)
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
        // The settings are per save (cheat_saves/<id>/player_cheats.ini, chosen by the frame hook once a game runs); the
        // old file is kept for the owner's main save M00.
        g_legacySettingsPath = userDir / "player_cheats.ini";
        g_settingsPath.clear();
        g_settings = Settings{};
        std::fprintf(stderr, "[players] %zu items (%s), %td skills; player cheats %s, %zu picked, scope %d%s\n", g_items.size(),
                     utf8(g_itemsPath).c_str(), urban::skills::end() - urban::skills::begin(), g_settings.enabled ? "on" : "off", g_settings.pins.size(),
                     g_settings.all ? g_settings.scope : 3, g_safeMode ? "; safe start: off" : "");
#if defined(URBAN_PLAYER_MENU)
        ps2x::menu::addPanel("선수 치트", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
        if (!g_safeMode && !g_items.empty())
            ps2x::pnach::addFrameHook(frame);
    }

    void installCamp(PS2Runtime &runtime)
    {
        if (!g_rdram || g_safeMode || g_items.empty())
            return;
        for (const uint32_t address : {kCampProcess, kCampPitchRoll, kCampPitchApply, kCampSwitchRoll, kCampSwitchApply, kCampBonus})
            if (!runtime.hasFunction(address))
            {
                std::fprintf(stderr, "[players] camp hook: no function at 0x%X\n", address);
                return;
            }
        g_campPitchRoll = runtime.lookupFunction(kCampPitchRoll);
        g_campPitchApply = runtime.lookupFunction(kCampPitchApply);
        g_campSwitchRoll = runtime.lookupFunction(kCampSwitchRoll);
        g_campSwitchApply = runtime.lookupFunction(kCampSwitchApply);
        g_campBonus = runtime.lookupFunction(kCampBonus);
        g_campRuntime = &runtime;
        FunctionHooks &hooks = FunctionHooks::instance();
        hooks.replace(kCampProcess, "camp_special_practice", &campHook, ReplacementStatus::Native, "player cheats: camp queue");
        g_campProcess = hooks.original(kCampProcess);
        if (!g_campProcess || !g_campPitchRoll || !g_campPitchApply || !g_campSwitchRoll || !g_campSwitchApply || !g_campBonus)
            std::fprintf(stderr, "[players] camp hook not installed\n");
    }
}
