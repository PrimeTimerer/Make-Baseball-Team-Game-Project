#include "urban/scout_cheats.h"
#include "urban/change_log.h"
#include "urban/function_hooks.h"
#include "urban/save_identity.h"

#include "ps2_runtime.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_save_state.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_SCOUT_MENU 1
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
#include <atomic>
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

namespace urban::scout
{
    namespace
    {
        constexpr uint32_t kTeamTable = 0x5D1240u, kTeamRoster = 0x120u, kRosterClass = 0x4u;
        constexpr uint32_t kOwnTeamClass = 0x6659F0u;
        constexpr uint32_t kScouting = 0x134u;                                // team -> its scouting object
        constexpr uint32_t kDomesticStates = 0x490u, kForeignStates = 0x6CFu; // the own team's state per candidate
        constexpr int kDomestic = 234, kForeign = 180;                        // 6 areas x 3 x 13; 10 leagues x 18
        constexpr uint32_t kDomesticPool = 0x5D1298u, kForeignPool = 0x5D129Cu; // -> the entries (overseas: + 0xC)
        constexpr uint32_t kEntry = 0x90u;
        // An entry (from its id; built from the database record by sub_0017A030 domestic / sub_0012D190 overseas):
        // +0 id, +2 name (16 bytes Shift-JIS), +0x15 age, +0x1F status, +0x2F the 12 clubs' interest, +0x3C their
        // evaluations (u16), +0x63 set to 0x0C by a find, +0x74 the info bits shown (u16), +0x8C the database record.
        constexpr uint32_t kName = 0x2u, kAge = 0x15u, kStatus = 0x1Fu, kInterest = 0x2Fu, kEvaluation = 0x3Cu, kFresh = 0x63u,
                           kInfo = 0x74u, kRecord = 0x8Cu;
        // +0x62: on the list to draft or sign now (国内新人 / 外国人 of 報告リスト). The overseas builder sets 1; the yearly
        // domestic build (sub_00177C78) sets it for this year's candidates only, so the others wait for next season.
        constexpr uint32_t kListed = 0x62u;
        // +0x78: the own club's deal with the candidate: 1 交渉成立, 2 drafted / joining (sub_00194328 moves 1 to 2); 0 none.
        // Drafted and signed players stay in the pools (also the CPU clubs' picks, who are on their rosters); the game takes
        // a domestic one off 国内新人 (+0x62 = 0) when drafted. Owner's save, 11-06: Cy Young's model 6001 signed, own
        // roster, 78 = 1; 3314 drafted, 78 = 2, then on the roster; 王貞治 after 交渉, 78 = 1.
        constexpr uint32_t kDeal = 0x78u;
        // +0x1B: the position (the record's u16 @0x7E >> 12): 0 投手, 1 捕手, 2 一塁, 3 二塁, 4 三塁, 5 遊撃, 6 外野 (it matched
        // the player notes for all 414 candidates of the owner's save, 11-06).
        constexpr uint32_t kPosition = 0x1Bu;
        constexpr const char *kPositions[7] = {"투수", "포수", "1루수", "2루수", "3루수", "유격수", "외야수"};
        // The database record's uniform name (背ネーム, from +0x1A): 5-bit letters, A = 1 ... Z = 26, 0 ends.
        constexpr uint32_t kBackName = 0x1Au;
        // The database ([0x5D1270]: +0 the records, +0xC their count): 0xA6-byte records sorted by id.
        constexpr uint32_t kDatabase = 0x5D1270u, kRecordSize = 0xA6u;
        constexpr int kTeams = 12;
        // The roll: sub_0015CEA0(n) = rand() % n, called with 100 by both finds; their count of the day's finds sits in
        // the caller's frame.
        constexpr uint32_t kRng = 0x15CEA0u, kForeignRoll = 0x19424Cu, kDomesticRoll = 0x1939F0u;
        constexpr uint32_t kForeignCount = 0x94u, kDomesticCount = 0xB4u;
        constexpr uint32_t kRamSize = 0x2000000u;
        // パイプ (owner, 11-08: "스카우트들 파이프도 맥스로 고정"): the scouting object's +0x488 / +0x48C are the two scouts;
        // a scout record's +0x2E holds 12 u16, the 6 domestic areas then the 6 overseas regions, the bars of the office's
        // 国内パイプ / 海外パイプ screen (5 bars). The 5ch 스카우터 최대 code writes 8000 there when the scout screen is
        // drawn (sub_003FA580), which the owner saw as 4 bars overseas while the domestic ones had gone down again.
        constexpr uint32_t kScoutList = 0x488u, kPipes = 0x2Eu;
        constexpr uint32_t kPipeCount = 12u;
        constexpr uint16_t kPipeTop = 10000u;

        constexpr const char *kAreas[6] = {"홋카이도·도호쿠", "간토", "호쿠신에쓰·도카이", "긴키", "주고쿠·시코쿠", "규슈·오키나와"};
        constexpr const char *kKinds[3] = {"고교", "대학", "사회인"};
        constexpr const char *kLeagues[10] = {"고데스 리그 (미국)", "그랜드 리그 (미국)",   "카리브 리그 (미국)",  "텐갤런 리그 (미국)",
                                              "반리 리그 (아시아)", "칸우 리그 (아시아)",   "소크람 리그 (아시아)", "에어즈 리그 (호주)",
                                              "바로크 리그 (유럽)", "케이프 리그 (남아공)"};
        constexpr const char *kTeamNames[kTeams] = {"라이온즈", "호크스",   "파이터즈", "마린즈",   "버팔로즈", "골든이글스",
                                                    "드래곤즈", "스왈로즈", "자이언츠", "타이거즈", "카프",     "요코하마"};

        struct Settings
        {
            bool enabled = false;
            int perDay = 0;         // finds a scout and day (0: the game's own odds)
            bool guard = true;      // CPU clubs lose the protected players
            bool guardFound = true; // ... every player the own team found too, not only the wish list and the kept ones
            bool listNow = true;    // wished / inserted domestic players on this year's 国内新人 list (+0x62 = 1)
            bool pipes = false;     // the two scouts' 12 pipes held at the top (kPipeTop)
            std::set<uint16_t> want; // found at once
            std::set<uint16_t> keep; // protected without being on the wish list
            int team = -1;           // the own team when these settings were first used (another team: another game)
        };
        // insert=<id>,<overseas 0/1>,<group>[,<from overseas 0/1>,<from index>[,<slot to replace>]] lines: done once when
        // the file changes while the game runs (not at start), dropped by the next save. For tests and power users.
        struct FileInsert
        {
            uint16_t id = 0u;
            bool foreign = false;
            int group = 0;
            bool fromForeign = false;
            int from = -1;
            int target = -1;
        };
        std::vector<FileInsert> g_fileInserts;
        struct Row
        {
            uint16_t id = 0u;
            bool foreign = false;
            int index = 0; // in its pool: domestic area * 39 + kind * 13 + n, overseas league * 18 + n
            std::string name, back;
            int age = 0, state = 0, status = 0, cpu = 0; // status: the rank, 0 S ... 4 D (as the game's ランク)
            int club = -1, deal = 0;                     // the club whose roster has him; the own club's deal (+0x78)
            int position = 0;                            // +0x1B, kPositions
            bool taken() const { return club >= 0 || deal == 1 || deal == 2; }
        };
        struct Write
        {
            bool foreign = false;
            int index = 0, state = 0;
        };
        // Put database player `id` into an empty slot of `group` (domestic: area * 3 + kind, 0-17; overseas: league
        // 0-9), as found; `from` >= 0: the entry moved there (that slot is emptied); `target` >= 0: in place of the
        // candidate in that slot of the pool, picked in the menu (1:1).
        struct Insert
        {
            uint16_t id = 0u;
            bool foreign = false;
            int group = 0;
            bool fromForeign = false;
            int from = -1;
            int target = -1;
        };
        struct DbRow
        {
            uint16_t id = 0u;
            std::string name, back;
            int age = 0, rank = 0, position = 0;
        };

        uint8_t *g_rdram = nullptr;
        std::filesystem::path g_path;       // this save's scout_cheats.ini (cheat_saves/<id>/), empty before a game
        std::filesystem::path g_legacyPath; // <userDir>/scout_cheats.ini from before the settings were per save
        std::string g_profile;              // the save (urban::saveid)
        uint32_t g_profileGen = ~0u;
        bool g_teamMismatch = false;        // the settings were made for another team: not used
        std::mutex g_mutex; // settings, rows, writes: the menu (GS thread) and the frame hook (EE thread)
        Settings g_settings;
        std::filesystem::file_time_type g_time{};
        std::vector<Row> g_rows;
        std::set<uint16_t> g_onRoster; // ids on the 12 rosters now (for the menu's 선수 넣기)
        // Overseas players' real models: scout_models.tsv (name<TAB>model, made by tools/scout_models.py from
        // gamezukushi.com's lists; 架空 = made up, 現役 = the real player himself). Read once at start.
        struct Note
        {
            std::string model, position, summary, modelEn, modelKo; // the model in English / Korean (tools/scout_model_names.tsv)
        };
        std::map<uint16_t, Note> g_notes;               // by player id
        std::map<std::string, std::string> g_models;    // model by name (the site's list, older files)
        std::vector<Write> g_writes;
        std::vector<Insert> g_inserts;
        std::vector<DbRow> g_db;         // the database for the menu (built once a load)
        uint32_t g_dbBase = 0u;
        std::string g_message;           // the last insert's result, for the menu
        int g_team = -1;
        // The change log (under g_mutex): each read of the pools is compared with the one before.
        struct Seen
        {
            bool foreign = false;
            int state = 0, club = -1, deal = 0;
            std::string name;
        };
        urban::ChangeLog g_log;
        std::map<uint16_t, Seen> g_seen; // the last read, by id (empty: the next read is the new baseline)
        uint32_t g_seenLoads = 0u;       // save states loaded at that read
        std::atomic<bool> g_refreshNow{false};
        std::atomic<bool> g_on{false};
        std::atomic<int> g_perDay{0};
        GuestFunction g_rng = nullptr;

        uint32_t read32(uint32_t address)
        {
            if (address >= kRamSize - 4u)
                return 0u;
            uint32_t value;
            std::memcpy(&value, g_rdram + address, 4);
            return value;
        }
        uint16_t read16(uint32_t address)
        {
            if (address >= kRamSize - 2u)
                return 0u;
            uint16_t value;
            std::memcpy(&value, g_rdram + address, 2);
            return value;
        }
        void write16(uint32_t address, uint16_t value)
        {
            if (address < kRamSize - 2u)
                std::memcpy(g_rdram + address, &value, 2);
        }
        bool valid(uint32_t address, uint32_t size) { return address >= 0x100000u && address + size < kRamSize; }

        std::string trim(std::string text)
        {
            const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
            while (!text.empty() && space(static_cast<unsigned char>(text.back())))
                text.pop_back();
            size_t start = 0;
            while (start < text.size() && space(static_cast<unsigned char>(text[start])))
                ++start;
            return text.substr(start);
        }

        std::set<uint16_t> idsOf(const std::string &text)
        {
            std::set<uint16_t> ids;
            std::istringstream list(text);
            std::string item;
            while (std::getline(list, item, '+'))
            {
                const unsigned long id = std::strtoul(trim(item).c_str(), nullptr, 10);
                if (id > 0u && id <= 0xFFFFu)
                    ids.insert(static_cast<uint16_t>(id));
            }
            return ids;
        }
        std::string textOf(const std::set<uint16_t> &ids)
        {
            std::string text;
            for (uint16_t id : ids)
                text += (text.empty() ? "" : "+") + std::to_string(id);
            return text;
        }

        // scout_cheats.ini: enabled=, perday=<finds a scout and day, 0 = the game's odds>, guard=, guardfound=,
        // want=<id>+<id> (found at once), keep=<id>+<id> (protected).
        Settings loadSettings()
        {
            Settings settings;
            std::ifstream file(g_path, std::ios::binary);
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
                else if (key == "perday")
                    settings.perDay = std::clamp(std::atoi(value.c_str()), 0, 99);
                else if (key == "guard")
                    settings.guard = value != "0";
                else if (key == "guardfound")
                    settings.guardFound = value != "0";
                else if (key == "listnow")
                    settings.listNow = value != "0";
                else if (key == "pipes")
                    settings.pipes = value == "1";
                else if (key == "team")
                    settings.team = std::atoi(value.c_str());
                else if (key == "want")
                    settings.want = idsOf(value);
                else if (key == "keep")
                    settings.keep = idsOf(value);
                else if (key == "insert")
                {
                    int fields[6] = {0, 0, 0, 0, -1, -1};
                    std::istringstream list(value);
                    std::string item;
                    for (int k = 0; k < 6 && std::getline(list, item, ','); ++k)
                        fields[k] = std::atoi(trim(item).c_str());
                    if (fields[0] > 0 && fields[0] <= 0xFFFF)
                        g_fileInserts.push_back(
                            FileInsert{static_cast<uint16_t>(fields[0]), fields[1] != 0, fields[2], fields[3] != 0, fields[4], fields[5]});
                }
            }
            return settings;
        }

        void saveSettings(const Settings &settings)
        {
            if (g_path.empty())
                return; // no game yet: nothing to keep
            {
                std::error_code made;
                std::filesystem::create_directories(g_path.parent_path(), made);
                std::ofstream file(g_path, std::ios::binary | std::ios::trunc);
                file << "# 스카우트 치트: 게임 메뉴(F2)의 \"스카우트\" 탭이 씁니다 (이 세이브의 설정)\n"
                     << "team=" << settings.team << "\n"
                     << "enabled=" << (settings.enabled ? 1 : 0) << "\n"
                     << "perday=" << settings.perDay << "\n"
                     << "guard=" << (settings.guard ? 1 : 0) << "\n"
                     << "guardfound=" << (settings.guardFound ? 1 : 0) << "\n"
                     << "listnow=" << (settings.listNow ? 1 : 0) << "\n"
                     << "pipes=" << (settings.pipes ? 1 : 0) << "\n"
                     << "want=" << textOf(settings.want) << "\n"
                     << "keep=" << textOf(settings.keep) << "\n";
            }
            std::error_code error;
            g_time = std::filesystem::last_write_time(g_path, error);
        }

        std::string nameAt(uint32_t address);
        std::string nameOf(uint32_t entry) { return nameAt(entry + kName); }
        // The roll of both finds won (0 < every chance) while the scout's finds of the day are under the setting.
        void rngHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            if ((ra == kForeignRoll || ra == kDomesticRoll) && g_on.load(std::memory_order_relaxed))
            {
                const int perDay = g_perDay.load(std::memory_order_relaxed);
                const uint32_t found = read32(getRegU32(ctx, 29) + (ra == kForeignRoll ? kForeignCount : kDomesticCount));
                if (perDay > 0 && found < static_cast<uint32_t>(perDay))
                {
                    setReturnU32(ctx, 0u);
                    ctx->pc = ra;
                    return;
                }
            }
            g_rng(rdram, ctx, runtime);
        }

        std::string nameAt(uint32_t address)
        {
#if defined(_WIN32)
            char sjis[16];
            std::memcpy(sjis, g_rdram + address, sizeof(sjis));
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
            (void)address;
            return std::string();
#endif
        }

        std::string backNameOf(uint32_t record)
        {
            if (!valid(record, kRecordSize))
                return std::string();
            uint64_t bits = 0u;
            for (uint32_t k = 0; k < 8u; ++k)
                bits |= static_cast<uint64_t>(g_rdram[record + kBackName + k]) << (8u * k);
            std::string text;
            for (int k = 0; k < 12; ++k)
            {
                const unsigned letter = static_cast<unsigned>(bits >> (5 * k)) & 31u;
                if (letter == 0u)
                    break;
                text += letter <= 26u ? static_cast<char>('A' + letter - 1u) : letter == 27u ? '.' : letter == 28u ? '-' : ' ';
            }
            return text;
        }

        // The database record of `id` (binary search, as sub_00125488), 0 when there is none.
        uint32_t recordOf(uint16_t id)
        {
            const uint32_t table = read32(kDatabase);
            if (!valid(table, 0x10u))
                return 0u;
            const uint32_t base = read32(table), count = read32(table + 0xCu);
            if (!valid(base, count * kRecordSize) || count > 20000u)
                return 0u;
            int low = 0, high = static_cast<int>(count) - 1;
            while (low <= high)
            {
                const int middle = (low + high) / 2;
                const uint32_t record = base + static_cast<uint32_t>(middle) * kRecordSize;
                const uint16_t at = read16(record);
                if (at == id)
                    return record;
                if (at < id)
                    low = middle + 1;
                else
                    high = middle - 1;
            }
            return 0u;
        }

        // A candidate entry from a database record, field by field as the game's builders do it (sub_0017A030 domestic,
        // sub_0012D190 overseas); no club knows the player yet.
        void buildEntry(uint32_t entry, uint32_t record, bool overseas, int kind)
        {
            uint8_t *e = g_rdram + entry;
            const uint8_t *r = g_rdram + record;
            const auto at16 = [&](uint32_t offset) { return static_cast<uint32_t>(r[offset] | (r[offset + 1u] << 8)); };
            std::memset(e, 0, kEntry);
            std::memcpy(e, r, 0x12u); // id and name
            e[0x12] = r[0x12];
            e[0x13] = r[0x2A] & 1u;
            e[0x14] = static_cast<uint8_t>((at16(0x2A) >> 1) & 3u);
            e[0x15] = r[0x15];
            if (!overseas)
            {
                // As sub_00178A50: the record's age belongs to its own kind (to 18 高校, 19-22 大学, 23- 社会人); put in
                // another kind, the player gets that kind's age (18, 22, 24).
                const int natural = r[0x15] < 19u ? 0 : r[0x15] > 22u ? 2 : 1;
                if (kind != natural)
                    e[0x15] = kind == 0 ? 18u : kind == 1 ? 22u : 24u;
            }
            const uint32_t money = at16(0x34) & 0x3FFu;
            e[0x16] = static_cast<uint8_t>(money);
            e[0x17] = static_cast<uint8_t>(money >> 8);
            e[0x1A] = static_cast<uint8_t>(((at16(0x38) & 7u) << 4) | (at16(0x36) >> 12));
            e[0x1B] = static_cast<uint8_t>((at16(0x7E) >> 12) & 7u);
            e[0x1F] = static_cast<uint8_t>((at16(0x4E) >> 2) & 0xFu);
            if (overseas)
                e[0x22] = r[0x4F];
            else
            {
                e[0x20] = static_cast<uint8_t>((at16(0x80) >> 3) & 0xFu);
                e[0x22] = 0xFFu;
            }
            e[0x23] = static_cast<uint8_t>(at16(0x80) >> 13);
            for (uint32_t k = 0; k < 5u; ++k)
                e[0x24 + k] = static_cast<uint8_t>((at16(0x82) >> (3u * k)) & 7u);
            e[0x29] = static_cast<uint8_t>(((at16(0x84) & 3u) << 1) | (at16(0x82) >> 15));
            for (uint32_t k = 0; k < 4u; ++k)
                e[0x2A + k] = static_cast<uint8_t>((at16(0x84) >> (2u + 3u * k)) & 7u);
            e[0x2E] = static_cast<uint8_t>(((at16(0x86) & 1u) << 2) | (at16(0x84) >> 14));
            e[0x62] = overseas ? 1u : 0u;
            e[0x64] = static_cast<uint8_t>(at16(0x38) >> 12);
            std::memcpy(e + kRecord, &record, 4);
        }

        std::string namesOf(const std::vector<std::string> &names) { return urban::joinNames(names); }

        // The change log: this read of the pools against the one before (g_mutex held). After a save state is loaded or
        // a save is opened, the read is the new baseline instead.
        void logChanges(int team, bool manual)
        {
            g_log.read(g_rdram);
            if (g_rows.empty())
            {
                g_seen.clear(); // no candidates yet (before the season): nothing to compare, nothing to log
                return;
            }
            std::map<uint16_t, Seen> now;
            size_t domestic = 0u, foreign = 0u, taken = 0u;
            for (const Row &row : g_rows)
            {
                now[row.id] = Seen{row.foreign, row.state, row.club, row.deal, row.name.empty() ? std::string("?") : row.name};
                ++(row.foreign ? foreign : domestic);
                taken += row.taken() ? 1u : 0u;
            }
            const uint32_t loads = ps2x::savestate::loadCount();
            int added = 0;
            const auto add = [&](const std::string &text) {
                g_log.add(text, "scout");
                ++added;
            };
            const std::string counts = "국내 " + std::to_string(domestic) + "명 · 해외 " + std::to_string(foreign) + "명";
            if (g_seen.empty() || loads != g_seenLoads)
                add(std::string(g_seen.empty() ? "후보 목록 읽음: " : "상태를 불러와서 다시 읽음: ") + counts + " · 영입·지명 끝난 " +
                    std::to_string(taken) + "명");
            else
            {
                std::vector<std::string> found, lost, cpu;
                int newDomestic = 0, newForeign = 0, goneDomestic = 0, goneForeign = 0;
                for (const auto &[id, seen] : now)
                {
                    const auto old = g_seen.find(id);
                    if (old == g_seen.end())
                    {
                        ++(seen.foreign ? newForeign : newDomestic);
                        continue;
                    }
                    const Seen &was = old->second;
                    const std::string where = seen.foreign ? " (해외 후보)" : " (국내 후보)";
                    if (seen.deal != was.deal && seen.deal == 1)
                        add("교섭 성공: " + seen.name + where);
                    else if (seen.deal != was.deal && seen.deal == 2)
                        add("지명 · 입단 확정: " + seen.name + where);
                    if (seen.club != was.club)
                    {
                        if (seen.club == team)
                            add("우리 구단 입단: " + seen.name + where);
                        else if (seen.club >= 0 && seen.club < kTeams)
                            cpu.push_back(seen.name + " → " + kTeamNames[seen.club]);
                        else if (was.club == team)
                            add("우리 구단 로스터에서 빠짐: " + seen.name + where);
                    }
                    if (seen.state >= 3 && was.state < 3)
                        found.push_back(seen.name);
                    else if (seen.state < 3 && was.state >= 3)
                        lost.push_back(seen.name);
                }
                for (const auto &[id, was] : g_seen)
                    if (!now.count(id))
                        ++(was.foreign ? goneForeign : goneDomestic);
                if (!found.empty())
                    add("발굴 " + std::to_string(found.size()) + "명: " + namesOf(found));
                if (!lost.empty())
                    add("발굴 취소 " + std::to_string(lost.size()) + "명: " + namesOf(lost));
                if (!cpu.empty())
                    add("CPU 구단 지명 · 영입 " + std::to_string(cpu.size()) + "명: " + namesOf(cpu));
                if (newDomestic > 0 || goneDomestic > 0)
                    add("국내 후보 칸 바뀜: 새로 " + std::to_string(newDomestic) + "명, 빠짐 " + std::to_string(goneDomestic) + "명");
                if (newForeign > 0 || goneForeign > 0)
                    add("해외 후보 칸 바뀜: 새로 " + std::to_string(newForeign) + "명, 빠짐 " + std::to_string(goneForeign) + "명");
                if (manual && added == 0)
                    add("새로고침: 바뀐 것 없음 (" + counts + ")");
            }
            g_seen = std::move(now);
            g_seenLoads = loads;
        }

        // Another save (urban::saveid): its own settings file, cheat_saves/<id>/scout_cheats.ini (none: nothing active).
        // g_mutex held.
        void switchProfile(const std::string &id)
        {
            g_profile = id;
            g_fileInserts.clear();
            g_inserts.clear();
            g_writes.clear();
            g_seen.clear();
            g_teamMismatch = false;
            std::error_code error;
            if (id.empty())
            {
                g_path.clear();
                g_settings = Settings{};
            }
            else
            {
                g_path = urban::saveid::folder(id) / "scout_cheats.ini";
                // Once: the settings from before they were per save were made on the owner's main save, M00.
                if (id == "M00" && !std::filesystem::exists(g_path, error) && std::filesystem::exists(g_legacyPath, error))
                {
                    std::filesystem::create_directories(g_path.parent_path(), error);
                    std::filesystem::copy_file(g_legacyPath, g_path, error);
                    std::fprintf(stderr, "[scout] the old scout_cheats.ini now belongs to save M00\n");
                }
                g_settings = std::filesystem::exists(g_path, error) ? loadSettings() : Settings{};
                g_fileInserts.clear(); // insert= lines are not replayed when a save is opened
            }
            g_time = g_path.empty() ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(g_path, error);
            g_on = false; // until the own team is checked against the settings' team
            g_perDay = g_settings.perDay;
            g_log.add("세이브: " + urban::saveid::describe(id) + " — 이 세이브의 스카우트 설정 (찜 " + std::to_string(g_settings.want.size()) + "명)", "scout");
        }

        // EE thread, at every vblank: the own team's scouting object, the wish list and the protection.
        void frame()
        {
            static unsigned tick = 0u;
            if (++tick % 10u != 0u)
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (const uint32_t generation = urban::saveid::generation(); generation != g_profileGen)
            {
                g_profileGen = generation;
                switchProfile(urban::saveid::current());
            }
            if (tick % 60u == 0u && !g_path.empty())
            {
                std::error_code error;
                const auto time = std::filesystem::last_write_time(g_path, error);
                if (!error && time != g_time)
                {
                    g_time = time;
                    g_fileInserts.clear();
                    g_settings = loadSettings();
                    g_on = g_settings.enabled;
                    g_perDay = g_settings.perDay;
                    for (const FileInsert &insert : g_fileInserts)
                        g_inserts.push_back(Insert{insert.id, insert.foreign, insert.group, insert.fromForeign, insert.from, insert.target});
                    g_fileInserts.clear();
                    std::fprintf(stderr, "[scout] settings reloaded: %s, %d a day, %zu wanted\n", g_settings.enabled ? "on" : "off",
                                 g_settings.perDay, g_settings.want.size());
                }
            }
            const bool manual = g_refreshNow.exchange(false); // the menu's 새로고침: read everything now
            if (manual)
                g_dbBase = 0u;
            int team = -1;
            uint32_t scouting = 0u;
            for (int t = 0; t < kTeams && team < 0; ++t)
            {
                const uint32_t object = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                if (!valid(object, kScouting + 4u))
                    continue;
                const uint32_t roster = read32(object + kTeamRoster);
                if (!valid(roster, 8u) || read32(roster + kRosterClass) != kOwnTeamClass)
                    continue;
                team = t;
                scouting = read32(object + kScouting);
            }
            const uint32_t domestic = read32(kDomesticPool), foreign = read32(kForeignPool);
            if (team < 0 || !valid(scouting, kForeignStates + kForeign) || !valid(domestic, kEntry * kDomestic) ||
                !valid(foreign, 0xCu + kEntry * kForeign))
            {
                g_team = -1;
                g_rows.clear();
                g_writes.clear();
                g_seen.clear();
                return;
            }
            if (team != g_team)
                std::fprintf(stderr, "[scout] own team %d, scouting at 0x%08x\n", team, scouting);
            g_team = team;
            // The settings belong to one game: the own team they were first used with (another team: another game's).
            if (g_settings.team < 0 && !g_path.empty())
            {
                g_settings.team = team;
                saveSettings(g_settings);
            }
            // A memory card save's own file (per save since 11-08) is that game's whatever team it says: a wrong first
            // team (the owner's M00 and M01 had team=0 from the file before the settings were per save, so the wish list
            // never ran on 요코하마) becomes this game's. Only "new" (any new game) keeps the check.
            const bool cardSave = g_profile.size() == 3u && (g_profile[0] == 'M' || g_profile[0] == 'V');
            if (cardSave && g_settings.team >= 0 && g_settings.team != team && !g_path.empty())
            {
                std::fprintf(stderr, "[scout] save %s: settings team %d -> %d (this save's own team)\n", g_profile.c_str(), g_settings.team, team);
                g_log.add("세이브 " + g_profile + "의 스카우트 설정이 다른 구단 번호로 저장돼 있어 이 세이브의 구단으로 고쳤습니다 (찜 " +
                              std::to_string(g_settings.want.size()) + "명이 이제 적용됩니다)",
                          "scout");
                g_settings.team = team;
                saveSettings(g_settings);
            }
            g_teamMismatch = g_settings.team >= 0 && g_settings.team != team;
            const bool active = g_settings.enabled && !g_teamMismatch && !g_path.empty();
            g_on = active;
            // パイプ 최대: the two scouts' 12 pipes at the top at every check (the game lowers and raises them weekly).
            if (active && g_settings.pipes)
                for (uint32_t s = 0; s < 2u; ++s)
                {
                    const uint32_t scout = read32(scouting + kScoutList + 4u * s);
                    if (!valid(scout, kPipes + 2u * kPipeCount))
                        continue;
                    int raised = 0;
                    for (uint32_t k = 0; k < kPipeCount; ++k)
                        if (read16(scout + kPipes + 2u * k) != kPipeTop)
                        {
                            write16(scout + kPipes + 2u * k, kPipeTop);
                            ++raised;
                        }
                    static bool logged[2] = {false, false};
                    if (raised > 0 && !logged[s])
                    {
                        logged[s] = true;
                        std::fprintf(stderr, "[scout] pipes of scout %u (0x%08x): %d raised to %u\n", s + 1u, scout, raised, kPipeTop);
                    }
                }
            const auto entryOf = [&](bool overseas, int index) {
                return overseas ? foreign + 0xCu + kEntry * static_cast<uint32_t>(index) : domestic + kEntry * static_cast<uint32_t>(index);
            };
            const auto stateOf = [&](bool overseas, int index) {
                return scouting + (overseas ? kForeignStates : kDomesticStates) + static_cast<uint32_t>(index);
            };
            for (const Write &write : g_writes)
            {
                g_rdram[stateOf(write.foreign, write.index)] = static_cast<uint8_t>(write.state);
                std::fprintf(stderr, "[scout] %s %d: state %d\n", write.foreign ? "overseas" : "domestic", write.index, write.state);
            }
            g_writes.clear();
            for (const Insert &insert : g_inserts)
            {
                const uint32_t record = recordOf(insert.id);
                const int first = insert.foreign ? insert.group * 18 : insert.group * 13;
                const int size = insert.foreign ? 18 : 13;
                if (record == 0u || first < 0 || first + size > (insert.foreign ? kForeign : kDomestic))
                {
                    g_message = "DB에 없는 선수 ID입니다: " + std::to_string(insert.id);
                    g_log.add(g_message, "scout");
                    continue;
                }
                // The slot: the one picked in the menu (1:1), else an empty one, else a candidate the newcomer may replace
                // (owner, 11-06): any C or D; at the newcomer's own position also one of a lower rank than his (an A
                // newcomer: a B there; an S one: an A or B there). Never a kept one or one with a deal. In turn one the own
                // team has not found, a memo one, a found but unwished one, then a wished one; in each, the lowest rank first.
                const int newRank = (read16(record + 0x4Eu) >> 2) & 0xF, newPosition = (read16(record + 0x7Eu) >> 12) & 7;
                const auto rankPosition = [&](uint32_t at) {
                    return std::string(1, "SABCD"[std::min<int>(g_rdram[at + kStatus], 4)]) + "급·" +
                           kPositions[std::min<int>(g_rdram[at + kPosition], 6)];
                };
                const std::string newcomer = std::string(1, "SABCD"[std::min(newRank, 4)]) + "급·" + kPositions[std::min(newPosition, 6)];
                int slot = -1;
                std::string replaced;
                if (insert.target >= 0)
                {
                    const uint32_t other = entryOf(insert.foreign, std::clamp(insert.target, 0, (insert.foreign ? kForeign : kDomestic) - 1));
                    const bool deal = g_rdram[other + kDeal] == 1u || g_rdram[other + kDeal] == 2u;
                    if (insert.target < first || insert.target >= first + size || deal)
                    {
                        g_message = deal ? "1:1 교체: " + nameAt(other + kName) + "은(는) 교섭·지명이 진행 중이라 바꾸지 않았습니다"
                                         : std::string("1:1 교체: 고른 칸이 넣을 곳에 없습니다");
                        g_log.add(g_message, "scout");
                        continue;
                    }
                    slot = insert.target;
                    if (read16(other) != 0u)
                    {
                        replaced = nameAt(other + kName) + "(" + rankPosition(other) + ")";
                        g_settings.want.erase(read16(other));
                    }
                }
                for (int k = 0; k < size && slot < 0; ++k)
                    if (read16(entryOf(insert.foreign, first + k)) == 0u)
                        slot = first + k;
                for (int pass = 0; pass < 4 && slot < 0; ++pass)
                {
                    int worst = -1;
                    for (int k = 0; k < size; ++k)
                    {
                        const int index = first + k;
                        const uint32_t other = entryOf(insert.foreign, index);
                        const uint16_t otherId = read16(other);
                        const uint8_t otherState = g_rdram[stateOf(insert.foreign, index)];
                        const bool wanted = g_settings.want.count(otherId) != 0u;
                        const bool deal = g_rdram[other + kDeal] == 1u || g_rdram[other + kDeal] == 2u; // never lose a deal
                        if (deal || g_settings.keep.count(otherId) || (insert.from >= 0 && insert.fromForeign == insert.foreign && insert.from == index))
                            continue;
                        const int rank = g_rdram[other + kStatus];
                        if (rank < 3 && !(rank > newRank && g_rdram[other + kPosition] == newPosition))
                            continue; // S, A, B: only below the newcomer's rank, at his position
                        const bool fits = pass == 0   ? otherState == 0u && !wanted
                                          : pass == 1 ? otherState < 3u && !wanted
                                          : pass == 2 ? !wanted
                                                      : true;
                        if (!fits)
                            continue;
                        if (worst < 0 || g_rdram[other + kStatus] > g_rdram[entryOf(insert.foreign, worst) + kStatus])
                            worst = index; // the lowest rank (D) first
                    }
                    if (worst >= 0)
                    {
                        slot = worst;
                        const uint32_t other = entryOf(insert.foreign, slot);
                        replaced = nameAt(other + kName) + "(" + rankPosition(other) + ")";
                        g_settings.want.erase(read16(other));
                    }
                }
                if (slot < 0)
                {
                    g_message = std::string("그곳 ") + (insert.foreign ? "18칸" : "13칸") + "에 바꿔도 되는 후보가 없어 넣지 않았습니다. 넣는 선수(" +
                                newcomer + ")로는 C·D급, 또는 같은 포지션의 더 낮은 등급만 바꿉니다 (독점·교섭한 선수는 빼고). "
                                           "'1:1 교체'로 바꿀 선수를 직접 고를 수 있습니다.";
                    g_log.add(g_message, "scout");
                    continue;
                }
                const uint32_t entry = entryOf(insert.foreign, slot);
                buildEntry(entry, record, insert.foreign, insert.group % 3);
                g_rdram[stateOf(insert.foreign, slot)] = 3u;
                g_rdram[entry + kFresh] = 0x0Cu;
                write16(entry + kInfo, 0xFFFFu);
                if (!insert.foreign && g_settings.listNow)
                    g_rdram[entry + kListed] = 1u;
                if (insert.from >= 0)
                {
                    std::memset(g_rdram + entryOf(insert.fromForeign, insert.from), 0, kEntry);
                    g_rdram[stateOf(insert.fromForeign, insert.from)] = 0u;
                }
                g_message = nameAt(entry + kName) + (insert.from >= 0 ? " 옮김: " : " 넣음: ") +
                            (insert.foreign ? std::string(kLeagues[insert.group])
                                            : std::string(kAreas[insert.group / 3]) + " · " + kKinds[insert.group % 3]) +
                            " (" + newcomer + ", 발굴 상태)" +
                            (replaced.empty() ? std::string() : (insert.target >= 0 ? ", 1:1 교체로 " : ", 빈 칸이 없어 ") + replaced + " 대신");
                g_log.add(g_message, "scout");
                std::fprintf(stderr, "[scout] %u %s %s slot %d%s\n", insert.id, insert.from >= 0 ? "moved to" : "put in",
                             insert.foreign ? "overseas" : "domestic", slot, insert.from >= 0 ? " (old slot emptied)" : "");
            }
            g_inserts.clear();
            // The database for the menu's 선수 넣기, once a load.
            if (const uint32_t table = read32(kDatabase); valid(table, 0x10u) && read32(table) != g_dbBase)
            {
                const uint32_t base = read32(table), count = read32(table + 0xCu);
                g_db.clear();
                if (valid(base, count * kRecordSize) && count <= 20000u)
                    for (uint32_t k = 0; k < count; ++k)
                    {
                        const uint32_t record = base + k * kRecordSize;
                        DbRow row;
                        row.id = read16(record);
                        row.name = nameAt(record + 2u);
                        row.age = g_rdram[record + 0x15u];
                        row.rank = (read16(record + 0x4Eu) >> 2) & 0xFu;
                        row.position = (read16(record + 0x7Eu) >> 12) & 7u;
                        row.back = backNameOf(record);
                        g_db.push_back(std::move(row));
                    }
                g_dbBase = base;
            }
            const bool rebuild = tick % 30u == 0u || manual;
            // Who is on a club now (the 12 rosters): drafted and signed players stay in the pools.
            std::map<uint16_t, int> clubOf;
            for (int t = 0; t < kTeams; ++t)
            {
                const uint32_t object = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                const uint32_t roster = valid(object, kTeamRoster + 4u) ? read32(object + kTeamRoster) : 0u;
                if (!valid(roster, 8u + 4u * 40u))
                    continue;
                for (uint32_t k = 0; k < 40u; ++k)
                {
                    const uint32_t record = read32(roster + 8u + 4u * k);
                    if (valid(record, 2u) && read16(record) != 0u)
                        clubOf.emplace(read16(record), t);
                }
            }
            if (rebuild)
            {
                g_rows.clear();
                g_onRoster.clear();
                for (const auto &member : clubOf)
                    g_onRoster.insert(member.first);
            }
            for (int pass = 0; pass < 2; ++pass)
            {
                const bool overseas = pass == 1;
                for (int index = 0; index < (overseas ? kForeign : kDomestic); ++index)
                {
                    const uint32_t entry = entryOf(overseas, index);
                    const uint16_t id = read16(entry);
                    if (id == 0u)
                        continue;
                    uint8_t &state = g_rdram[stateOf(overseas, index)];
                    const auto club = clubOf.find(id);
                    const uint8_t deal = g_rdram[entry + kDeal];
                    const bool taken = club != clubOf.end() || deal == 1u || deal == 2u;
                    if (active && taken)
                    {
                        // Drafted or signed: the wish list and the protection leave him alone. A domestic one goes back
                        // off 国内新人 as the game takes him off at the draft (listnow had put him back on).
                        if (!overseas && (club != clubOf.end() || deal == 2u) && g_rdram[entry + kListed] != 0u)
                        {
                            g_rdram[entry + kListed] = 0u;
                            std::fprintf(stderr, "[scout] %u taken (club %d, deal %u): off the list\n", id,
                                         club != clubOf.end() ? club->second : -1, deal);
                        }
                    }
                    else if (active)
                    {
                        // The wish list: found now, everything shown (as a find does: state 3, +0x63 = 0x0C).
                        if (!overseas && g_settings.listNow && g_settings.want.count(id) && g_rdram[entry + kListed] == 0u)
                            g_rdram[entry + kListed] = 1u;
                        if (g_settings.want.count(id) && state < 3u)
                        {
                            state = 3u;
                            g_rdram[entry + kFresh] = std::max<uint8_t>(g_rdram[entry + kFresh], 0x0Cu);
                            write16(entry + kInfo, 0xFFFFu);
                            std::fprintf(stderr, "[scout] wanted %u found (%s %d)\n", id, overseas ? "overseas" : "domestic", index);
                        }
                        const bool guarded = g_settings.guard && (g_settings.want.count(id) || g_settings.keep.count(id) ||
                                                                  (g_settings.guardFound && state >= 3u));
                        if (guarded)
                            for (int t = 0; t < kTeams; ++t)
                                if (t != team)
                                {
                                    g_rdram[entry + kInterest + static_cast<uint32_t>(t)] = 0u;
                                    write16(entry + kEvaluation + 2u * static_cast<uint32_t>(t), 0u);
                                }
                    }
                    if (rebuild)
                    {
                        Row row;
                        row.id = id;
                        row.foreign = overseas;
                        row.index = index;
                        row.name = nameOf(entry);
                        row.back = backNameOf(read32(entry + kRecord));
                        row.age = g_rdram[entry + kAge];
                        row.state = state;
                        row.status = g_rdram[entry + kStatus];
                        row.position = g_rdram[entry + kPosition];
                        row.club = club != clubOf.end() ? club->second : -1;
                        row.deal = deal;
                        for (int t = 0; t < kTeams; ++t)
                            if (t != team && g_rdram[entry + kInterest + static_cast<uint32_t>(t)] >= 2u)
                                ++row.cpu;
                        g_rows.push_back(std::move(row));
                    }
                }
            }
            if (rebuild)
            {
                static size_t lastTaken = ~size_t{0};
                const size_t taken = static_cast<size_t>(std::count_if(g_rows.begin(), g_rows.end(), [](const Row &row) { return row.taken(); }));
                if (taken != lastTaken)
                {
                    lastTaken = taken;
                    std::string own;
                    for (const Row &row : g_rows)
                        if (row.club == team || ((row.deal == 1 || row.deal == 2) && row.club < 0))
                            own += " " + std::to_string(row.id) + (row.club == team ? "(roster)" : "(deal " + std::to_string(row.deal) + ")");
                    std::fprintf(stderr, "[scout] %zu of %zu candidates drafted or signed (hidden in the menu); own club:%s\n", taken,
                                 g_rows.size(), own.c_str());
                }
                logChanges(team, manual);
            }
        }

#if defined(URBAN_SCOUT_MENU)
        char g_search[64] = "";
        int g_place = 0;  // 0 all, else area / league + 1
        int g_show = 0;   // 0 all, 1 found, 2 not found, 3 wanted or kept
        int g_showTaken = 0; // also the drafted / signed players (hidden by default)
        int g_tab = 0;
        int g_toArea = 0, g_toKind = 0, g_toLeague = 0; // where 선수 넣기 / 옮기기 put a player
        char g_dbSearch[64] = "";
        uint16_t g_pick = 0u; // the database player for a 1:1 swap (0: none)
        int g_pickWhere = 0;  // 0 the domestic lists, 1 the overseas ones

        // "이름 (모델: …)" when the notes know a real model (OB / 現役 / 架空: the name is the player himself or made up).
        std::string displayName(uint16_t id, const std::string &name)
        {
            const std::string shown = name.empty() ? "?" : name;
            std::string model;
            if (const auto note = g_notes.find(id); note != g_notes.end())
                model = note->second.model;
            if (model.empty())
                if (const auto byName = g_models.find(name); byName != g_models.end())
                    model = byName->second;
            if (model.empty() || model == "架空" || model == "現役" || model == "OB" || model == "-")
                return shown;
            if (model == "特殊")
                return shown + " (특수 선수)";
            if (const auto note = g_notes.find(id); note != g_notes.end() && !note->second.modelKo.empty())
                return shown + " (모델: " + note->second.modelKo + " · " + note->second.modelEn + ")";
            return shown + " (모델: " + model + ")";
        }
        std::string modelOf(uint16_t id, const std::string &name)
        {
            if (const auto note = g_notes.find(id); note != g_notes.end() && !note->second.model.empty())
                return note->second.model;
            const auto byName = g_models.find(name);
            return byName == g_models.end() ? std::string() : byName->second;
        }
        const Note *noteOf(uint16_t id)
        {
            const auto note = g_notes.find(id);
            return note == g_notes.end() ? nullptr : &note->second;
        }
        // A query: part of the name, of the uniform name (letters, any case) or the id.
        // Letters and digits only, lower case (English); spaces dropped (any other text).
        std::string folded(const std::string &text)
        {
            std::string out;
            for (unsigned char c : text)
                if (c >= 0x80u)
                    out += static_cast<char>(c);
                else if (std::isalnum(c))
                    out += static_cast<char>(std::tolower(c));
            return out;
        }
        bool matchesQuery(const std::string &name, const std::string &back, uint16_t id, const std::string &query)
        {
            if (name.find(query) != std::string::npos || std::to_string(id) == query)
                return true;
            const std::string key = folded(query);
            if (key.empty())
                return false;
            if (folded(name).find(key) != std::string::npos || folded(back).find(key) != std::string::npos)
                return true;
            const std::string model = modelOf(id, name);
            if (!model.empty() && (model.find(query) != std::string::npos || folded(model).find(key) != std::string::npos))
                return true;
            const auto note = g_notes.find(id);
            return note != g_notes.end() &&
                   (folded(note->second.modelEn).find(key) != std::string::npos || folded(note->second.modelKo).find(key) != std::string::npos);
        }
        constexpr const char *kRanks[5] = {"S", "A", "B", "C", "D"};
        // S gold, A red, B orange, C green, D grey (R in the low byte).
        constexpr uint32_t kRankColors[5] = {0xFF2CD7FFu, 0xFF5A5AFFu, 0xFF50BEFFu, 0xFF78C878u, 0xFF968C8Cu};
        void rankCell(const Pcsx2GsUi *ui, int rank)
        {
            if (rank >= 0 && rank < 5)
                ui->text_colored(kRankColors[rank], kRanks[rank]);
            else
                ui->text_disabled("?");
            ui->tooltip("게임의 랭크(ランク): S가 가장 좋고 D가 가장 낮음. 성장 폭발 횟수·코멘트에 영향 (S/A는 발굴 때 '유력 선수' 메일)");
        }
        int g_rankFilter = 0; // 0 all, 1 S, 2 A or better, 3 B or better

        // The place pickers: a domestic area and kind, or an overseas league.
        void drawTarget(const Pcsx2GsUi *ui, bool overseas)
        {
            if (overseas)
            {
                ui->set_next_width(200.0f);
                ui->combo("##toleague", &g_toLeague, kLeagues, 10);
            }
            else
            {
                ui->set_next_width(170.0f);
                ui->combo("##toarea", &g_toArea, kAreas, 6);
                ui->same_line();
                ui->set_next_width(90.0f);
                ui->combo("##tokind", &g_toKind, kKinds, 3);
            }
        }

        void request(const Insert &insert)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_inserts.push_back(insert);
        }

        const char *stateText(int state)
        {
            switch (state)
            {
            case 0: return "미발굴";
            case 2: return "메모";
            case 3: return "발굴 (이번 주)";
            case 4: return "발굴";
            default: return "?";
            }
        }
        std::string placeText(const Row &row)
        {
            if (row.foreign)
                return kLeagues[std::clamp(row.index / 18, 0, 9)];
            return std::string(kAreas[std::clamp(row.index / 39, 0, 5)]) + " · " + kKinds[std::clamp(row.index % 39 / 13, 0, 2)];
        }

        // The deal column: on whose roster, or the own club's deal before he joins.
        void dealCell(const Pcsx2GsUi *ui, const Row &row, int team)
        {
            if (row.club >= 0 && row.club == team)
                ui->text_colored(0xFF78C878u, "우리 구단 입단");
            else if (row.club >= 0)
                ui->text_colored(0xFF5050FFu, (std::string(kTeamNames[std::clamp(row.club, 0, kTeams - 1)]) + " 소속").c_str());
            else
                ui->text_colored(0xFF78C878u, row.deal == 2 ? "지명 (입단 전)" : "교섭 성공 (입단 전)");
            ui->tooltip("영입·지명이 끝난 선수: 찜·독점 치트가 더는 건드리지 않습니다 (목록에서는 기본으로 숨김).");
        }

        void drawPool(const Pcsx2GsUi *ui, Settings &settings, const std::vector<Row> &rows, bool overseas, int team, bool &changed)
        {
            ui->set_next_width(200.0f);
            ui->input_text("##search", "이름 · 영문 · 실존 모델 · ID", g_search, sizeof(g_search));
            ui->tooltip("게임 이름, 등 이름(영문), 실존 모델 이름(가타카나·영어·한국어, 예: 사이 영, cy young), 선수 ID로 찾습니다.");
            ui->same_line();
            std::vector<std::string> places{overseas ? "모든 리그" : "모든 지역·구분"};
            if (overseas)
                for (const char *league : kLeagues)
                    places.push_back(league);
            else
                for (const char *area : kAreas)
                    places.push_back(area);
            std::vector<const char *> pointers;
            for (const std::string &place : places)
                pointers.push_back(place.c_str());
            ui->set_next_width(190.0f);
            ui->combo("##place", &g_place, pointers.data(), static_cast<int>(pointers.size()));
            ui->same_line();
            const char *shows[4] = {"모두", "발굴한 선수", "미발굴", "찜·독점"};
            ui->set_next_width(120.0f);
            ui->combo("##show", &g_show, shows, 4);
            ui->same_line();
            const char *ranks[4] = {"모든 랭크", "S만", "A 이상", "B 이상"};
            ui->set_next_width(100.0f);
            ui->combo("##rank", &g_rankFilter, ranks, 4);
            ui->same_line();
            ui->checkbox("영입·지명 끝난 선수도 보기", &g_showTaken);
            ui->tooltip("우리 구단이 교섭에 성공했거나 지명한 선수, 이미 어느 구단에 들어간 선수(CPU 지명·영입 포함)는 기본으로 숨깁니다.");
            std::string query = trim(g_search);
            std::vector<const Row *> shown;
            int hidden = 0;
            for (const Row &row : rows)
            {
                if (row.foreign != overseas)
                    continue;
                if (row.taken() && !g_showTaken)
                {
                    ++hidden;
                    continue;
                }
                if (g_place > 0 && (overseas ? row.index / 18 : row.index / 39) != g_place - 1)
                    continue;
                const bool wanted = settings.want.count(row.id) || settings.keep.count(row.id);
                if ((g_show == 1 && row.state < 3) || (g_show == 2 && row.state >= 3) || (g_show == 3 && !wanted))
                    continue;
                if (!query.empty() && !matchesQuery(row.name, row.back, row.id, query))
                    continue;
                if (g_rankFilter > 0 && row.status > g_rankFilter - 1)
                    continue;
                shown.push_back(&row);
            }
            ui->text(overseas ? "국내 목록으로 옮길 곳:" : "해외 목록으로 옮길 곳:");
            ui->same_line();
            drawTarget(ui, !overseas);
            ui->same_line();
            ui->text_disabled("(줄의 '옮기기' = 그곳 빈 칸에 발굴 상태로, 원래 칸은 비움. 빈 칸이 없으면 C·D급이나 같은 포지션의 더 낮은 등급과만 바꿈)");
            ui->text_disabled(("보이는 후보 " + std::to_string(shown.size()) + "명" +
                               (hidden > 0 ? " · 영입·지명 끝난 " + std::to_string(hidden) + "명 숨김" : std::string()))
                                  .c_str());
            ui->same_line();
            if (ui->small_button("보이는 후보 모두 찜 (바로 발굴)"))
            {
                for (const Row *row : shown)
                    settings.want.insert(row->id);
                changed = true;
            }
            ui->same_line();
            if (ui->small_button("보이는 후보 찜 해제"))
            {
                for (const Row *row : shown)
                    settings.want.erase(row->id);
                changed = true;
            }
            if (!ui->begin_table(overseas ? "foreign" : "domestic", 12,
                                 PCSX2GS_TABLE_SCROLL | PCSX2GS_TABLE_ROW_BG | PCSX2GS_TABLE_BORDERS | PCSX2GS_TABLE_RESIZABLE |
                                     PCSX2GS_TABLE_SORTABLE | PCSX2GS_TABLE_SCROLL_X,
                                 0.0f, 0.0f))
                return;
            ui->table_freeze(3, 1);
            ui->table_column("찜", PCSX2GS_COLUMN_NO_SORT, 34.0f);
            ui->table_column("독점", PCSX2GS_COLUMN_NO_SORT, 40.0f);
            ui->table_column("이름", 0, 300.0f);
            ui->table_column("영문", 0, 90.0f);
            ui->table_column("랭크", 0, 40.0f);
            ui->table_column("나이", 0, 36.0f);
            ui->table_column("포지션", 0, 80.0f);
            ui->table_column("능력 (상한)", PCSX2GS_COLUMN_NO_SORT, 270.0f);
            ui->table_column(overseas ? "리그" : "지역 · 구분", 0, 170.0f);
            ui->table_column("우리 구단", 0, 100.0f);
            ui->table_column("CPU 관심", PCSX2GS_COLUMN_DESCENDING_FIRST, 64.0f);
            ui->table_column("", PCSX2GS_COLUMN_NO_SORT, 180.0f);
            ui->table_headers();
            int column = -1, down = 0;
            ui->table_sort(&column, &down);
            if (column >= 0)
                std::stable_sort(shown.begin(), shown.end(), [&](const Row *a, const Row *b) {
                    const auto key = [&](const Row *row) -> long long {
                        switch (column)
                        {
                        case 4: return row->status;
                        case 5: return row->age;
                        case 8: return row->index;
                        case 9: return row->state;
                        case 10: return row->cpu;
                        default: return 0;
                        }
                    };
                    if (column == 2 || column == 3 || column == 6)
                    {
                        const Note *na = noteOf(a->id), *nb = noteOf(b->id);
                        const std::string x = column == 2 ? a->name : column == 3 ? a->back : na ? na->position : std::string();
                        const std::string y = column == 2 ? b->name : column == 3 ? b->back : nb ? nb->position : std::string();
                        return down ? x > y : x < y;
                    }
                    return down ? key(a) > key(b) : key(a) < key(b);
                });
            ui->clipper_begin(static_cast<int>(shown.size()));
            int first = 0, end = 0;
            while (ui->clipper_step(&first, &end))
                for (int i = first; i < end; ++i)
                {
                    const Row &row = *shown[static_cast<size_t>(i)];
                    ui->table_next_row();
                    ui->push_id((overseas ? 100000 : 0) + row.index);
                    ui->table_next_column();
                    int want = settings.want.count(row.id) ? 1 : 0;
                    if (ui->checkbox("##want", &want))
                    {
                        if (want)
                            settings.want.insert(row.id);
                        else
                            settings.want.erase(row.id);
                        changed = true;
                    }
                    ui->tooltip("찜: 스카우트가 가지 않아도 바로 발굴 (능력 정보도 모두 공개)");
                    ui->table_next_column();
                    int keep = settings.keep.count(row.id) ? 1 : 0;
                    if (ui->checkbox("##keep", &keep))
                    {
                        if (keep)
                            settings.keep.insert(row.id);
                        else
                            settings.keep.erase(row.id);
                        changed = true;
                    }
                    ui->tooltip("독점: CPU 구단의 관심·평가를 계속 지워서 이 선수를 쫓지 못하게 (찜·발굴한 선수는 설정에 따라 자동)");
                    ui->table_next_column();
                    ui->text(displayName(row.id, row.name).c_str());
                    {
                        const std::string model = modelOf(row.id, row.name);
                        ui->tooltip(("선수 ID " + std::to_string(row.id) + (model.empty() ? std::string() : " · 모델: " + model)).c_str());
                    }
                    ui->table_next_column();
                    ui->text(row.back.c_str());
                    ui->table_next_column();
                    rankCell(ui, row.status);
                    ui->table_next_column();
                    ui->text(std::to_string(row.age).c_str());
                    const Note *note = noteOf(row.id);
                    ui->table_next_column();
                    ui->text(note ? note->position.c_str() : "");
                    ui->table_next_column();
                    if (note && !note->summary.empty())
                        ui->text(note->summary.c_str());
                    else
                        ui->text_disabled("(선수 데이터 파일 없음)");
                    ui->table_next_column();
                    ui->text(placeText(row).c_str());
                    ui->table_next_column();
                    if (row.taken())
                        dealCell(ui, row, team);
                    else if (row.state >= 3)
                        ui->text_colored(0xFF66CCFFu, stateText(row.state));
                    else
                        ui->text_disabled(stateText(row.state));
                    ui->table_next_column();
                    if (row.cpu > 0 && !row.taken())
                        ui->text_colored(0xFF5050FFu, (std::to_string(row.cpu) + "구단").c_str());
                    else
                        ui->text_disabled("-");
                    ui->table_next_column();
                    if (row.taken())
                    {
                        ui->text_disabled("-");
                        ui->pop_id();
                        continue;
                    }
                    if (ui->small_button(overseas ? "국내로 옮기기" : "해외로 옮기기"))
                        request(Insert{row.id, !overseas, overseas ? g_toArea * 3 + g_toKind : g_toLeague, row.foreign, row.index});
                    if (row.state >= 3)
                    {
                        ui->same_line();
                        if (ui->small_button("발굴 취소"))
                        {
                            std::lock_guard<std::mutex> lock(g_mutex);
                            g_writes.push_back(Write{row.foreign, row.index, 0});
                            settings.want.erase(row.id);
                            changed = true;
                        }
                    }
                    ui->pop_id();
                }
            ui->end_table();
        }

        // 1:1: the picked database player in place of a candidate chosen in the list of one domestic area and kind or
        // one overseas league (any candidate but one with a deal).
        void drawPick(const Pcsx2GsUi *ui, const std::vector<DbRow> &db, const std::vector<Row> &rows)
        {
            const auto picked = std::find_if(db.begin(), db.end(), [](const DbRow &row) { return row.id == g_pick; });
            if (picked == db.end())
            {
                g_pick = 0u;
                return;
            }
            ui->separator_text("1:1 교체: 바꿀 후보를 직접 고르기");
            ui->text(("넣을 선수: " + displayName(picked->id, picked->name) + " (" + kRanks[std::clamp(picked->rank, 0, 4)] + "급 · " +
                      kPositions[std::clamp(picked->position, 0, 6)] + ")")
                         .c_str());
            ui->same_line();
            if (ui->small_button("닫기##pick"))
            {
                g_pick = 0u;
                return;
            }
            const char *wheres[2] = {"국내 신인 목록", "해외 리그 목록"};
            ui->set_next_width(150.0f);
            ui->combo("##pickwhere", &g_pickWhere, wheres, 2);
            ui->same_line();
            const bool overseas = g_pickWhere == 1;
            drawTarget(ui, overseas);
            const int group = overseas ? g_toLeague : g_toArea * 3 + g_toKind;
            const int first = overseas ? group * 18 : group * 13, size = overseas ? 18 : 13;
            std::vector<const Row *> slots;
            for (const Row &row : rows)
                if (row.foreign == overseas && row.index >= first && row.index < first + size)
                    slots.push_back(&row);
            std::sort(slots.begin(), slots.end(), [](const Row *a, const Row *b) { return a->index < b->index; });
            if (static_cast<int>(slots.size()) < size)
                ui->text_disabled(("빈 칸 " + std::to_string(size - static_cast<int>(slots.size())) +
                                   "개: 아래 표의 '국내에 넣기' · '해외에 넣기'가 빈 칸부터 씁니다.")
                                      .c_str());
            if (!ui->begin_table("pick", 6, PCSX2GS_TABLE_SCROLL | PCSX2GS_TABLE_ROW_BG | PCSX2GS_TABLE_BORDERS, 0.0f, 260.0f))
                return;
            ui->table_freeze(1, 1);
            ui->table_column("이름", 0, 300.0f);
            ui->table_column("랭크", 0, 40.0f);
            ui->table_column("포지션", 0, 60.0f);
            ui->table_column("나이", 0, 36.0f);
            ui->table_column("상태", 0, 130.0f);
            ui->table_column("", 0, 150.0f);
            ui->table_headers();
            for (const Row *row : slots)
            {
                ui->table_next_row();
                ui->push_id(300000 + row->index);
                ui->table_next_column();
                ui->text(displayName(row->id, row->name).c_str());
                ui->table_next_column();
                rankCell(ui, row->status);
                ui->table_next_column();
                ui->text(kPositions[std::clamp(row->position, 0, 6)]);
                ui->table_next_column();
                ui->text(std::to_string(row->age).c_str());
                ui->table_next_column();
                const bool deal = row->deal == 1 || row->deal == 2;
                if (deal)
                    ui->text_colored(0xFF78C878u, row->deal == 2 ? "지명 (입단 전)" : "교섭 성공 (입단 전)");
                else if (row->club >= 0)
                    ui->text_colored(0xFF5050FFu, (std::string(kTeamNames[std::clamp(row->club, 0, kTeams - 1)]) + " 소속").c_str());
                else
                    ui->text_disabled(stateText(row->state));
                ui->table_next_column();
                if (deal)
                    ui->text_disabled("교섭 중이라 못 바꿈");
                else if (ui->small_button("이 선수와 바꾸기"))
                {
                    request(Insert{g_pick, overseas, group, false, -1, row->index});
                    g_pick = 0u;
                }
                ui->pop_id();
            }
            ui->end_table();
            ui->separator();
        }

        // Any database player into an empty slot of a domestic area and kind or an overseas league, found.
        void drawDatabase(const Pcsx2GsUi *ui, const std::vector<DbRow> &db, const std::vector<Row> &rows,
                          const std::set<uint16_t> &onRoster, const std::string &message)
        {
            ui->text_wrapped("게임 DB의 선수(현역·OB·아마추어·외국인 모두)를 올해 국내 신인 목록이나 해외 리그 목록의 빈 칸에 발굴 상태로 넣습니다. "
                             "빈 칸이 없으면 그곳의 C·D급 후보(D급 먼저)나, 넣는 선수보다 낮은 등급의 같은 포지션 선수(A급이면 B급, S급이면 A·B급)만 "
                             "바꾸고 독점·교섭한 선수는 바꾸지 않습니다. '1:1 교체…'로는 바꿀 선수를 직접 고릅니다. "
                             "항목은 게임이 후보를 만들 때와 같은 방식으로 DB에서 만듭니다.");
            ui->text("국내에 넣을 곳:");
            ui->same_line();
            drawTarget(ui, false);
            ui->same_line();
            ui->text("해외에 넣을 곳:");
            ui->same_line();
            drawTarget(ui, true);
            if (!message.empty())
                ui->text_colored(0xFF66CCFFu, message.c_str());
            if (g_pick != 0u)
                drawPick(ui, db, rows);
            ui->set_next_width(220.0f);
            ui->input_text("##dbsearch", "이름 · 영문 · 실존 모델 · ID로 찾기", g_dbSearch, sizeof(g_dbSearch));
            ui->tooltip("게임 이름, 등 이름(영문), 실존 모델 이름(가타카나·영어·한국어, 예: 랜디 존슨, randy johnson), 선수 ID로 찾습니다.");
            ui->same_line();
            ui->text_disabled(("DB 선수 " + std::to_string(db.size()) + "명 (찾을 말을 쓰면 목록이 나옵니다)").c_str());
            const std::string query = trim(g_dbSearch);
            std::vector<const DbRow *> shown;
            if (!query.empty())
                for (const DbRow &row : db)
                {
                    std::string name = row.name;
                    std::string compact;
                    for (char c : name)
                        if (c != ' ')
                            compact += c;
                    if (compact.find(query) != std::string::npos || matchesQuery(row.name, row.back, row.id, query))
                        shown.push_back(&row);
                }
            if (shown.empty())
                return;
            std::map<uint16_t, const Row *> pooled;
            for (const Row &row : rows)
                pooled[row.id] = &row;
            if (!ui->begin_table("database", 9, PCSX2GS_TABLE_SCROLL | PCSX2GS_TABLE_ROW_BG | PCSX2GS_TABLE_BORDERS | PCSX2GS_TABLE_SCROLL_X,
                                 0.0f, 0.0f))
                return;
            ui->table_freeze(2, 1);
            ui->table_column("ID", 0, 50.0f);
            ui->table_column("이름", 0, 320.0f);
            ui->table_column("영문", 0, 90.0f);
            ui->table_column("랭크", 0, 40.0f);
            ui->table_column("나이", 0, 36.0f);
            ui->table_column("포지션", 0, 80.0f);
            ui->table_column("능력 (상한)", 0, 270.0f);
            ui->table_column("지금", 0, 110.0f);
            ui->table_column("", 0, 290.0f);
            ui->table_headers();
            ui->clipper_begin(static_cast<int>(shown.size()));
            int first = 0, end = 0;
            while (ui->clipper_step(&first, &end))
                for (int i = first; i < end; ++i)
                {
                    const DbRow &row = *shown[static_cast<size_t>(i)];
                    ui->table_next_row();
                    ui->push_id(200000 + row.id);
                    ui->table_next_column();
                    ui->text(std::to_string(row.id).c_str());
                    ui->table_next_column();
                    ui->text(displayName(row.id, row.name).c_str());
                    ui->table_next_column();
                    ui->text(row.back.c_str());
                    ui->table_next_column();
                    rankCell(ui, row.rank);
                    ui->table_next_column();
                    ui->text(std::to_string(row.age).c_str());
                    const Note *note = noteOf(row.id);
                    ui->table_next_column();
                    ui->text(note ? note->position.c_str() : "");
                    ui->table_next_column();
                    ui->text(note ? note->summary.c_str() : "");
                    ui->table_next_column();
                    const auto pool = pooled.find(row.id);
                    if (onRoster.count(row.id))
                        ui->text_colored(0xFF5050FFu, "구단 소속");
                    else if (pool != pooled.end())
                        ui->text(pool->second->foreign ? "해외 후보" : "국내 후보");
                    else
                        ui->text_disabled("-");
                    ui->table_next_column();
                    if (ui->small_button("국내에 넣기"))
                        request(Insert{row.id, false, g_toArea * 3 + g_toKind, false, -1});
                    ui->same_line();
                    if (ui->small_button("해외에 넣기"))
                        request(Insert{row.id, true, g_toLeague, false, -1});
                    if (onRoster.count(row.id) || pool != pooled.end())
                        ui->tooltip("이미 구단이나 후보 목록에 있는 선수입니다. 넣으면 같은 선수가 두 곳에 생깁니다.");
                    ui->same_line();
                    if (ui->small_button("1:1 교체…"))
                    {
                        g_pick = row.id;
                        g_pickWhere = pool != pooled.end() && pool->second->foreign ? 1 : g_pickWhere;
                    }
                    ui->tooltip("넣을 곳의 후보 목록을 열어 바꿀 선수를 직접 고릅니다 (등급·포지션 제한 없이, 교섭 중인 선수만 빼고).");
                    ui->pop_id();
                }
            ui->end_table();
        }

        void draw(const Pcsx2GsUi *ui)
        {
            if (ui->version < 3u)
            {
                ui->text_wrapped("pcsx2gs.dll이 이 실행 파일보다 오래되었습니다. 다시 빌드하세요.");
                return;
            }
            Settings settings;
            std::vector<Row> rows;
            std::vector<DbRow> db;
            std::set<uint16_t> onRoster;
            std::string message;
            int team;
            urban::ChangeLog log;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                settings = g_settings;
                rows = g_rows;
                team = g_team;
                db = g_db;
                onRoster = g_onRoster;
                message = g_message;
                log = g_log;
            }
            bool changed = false;
            int enabled = settings.enabled ? 1 : 0;
            if (ui->checkbox("스카우트 치트 켜기", &enabled))
            {
                settings.enabled = enabled != 0;
                changed = true;
            }
            ui->same_line();
            ui->text_disabled(team < 0 ? "세이브를 불러오면 후보 명단이 나옵니다"
                                       : (std::string("내 구단 ") + kTeamNames[team] + " · 국내 후보 · 해외 후보를 매 순간 확인").c_str());
            // The settings of another team's game (a new game's "new" file) do nothing: say so, and offer to take them.
            if (team >= 0 && settings.team >= 0 && settings.team < kTeams && settings.team != team)
            {
                ui->text_colored(0xFF5050FFu, ("이 설정은 " + std::string(kTeamNames[settings.team]) + " 게임에서 만든 것이라 지금은 쓰지 않습니다 (찜·발굴·독점 모두 멈춤).").c_str());
                ui->same_line();
                if (ui->small_button("이 구단 설정으로 쓰기"))
                {
                    settings.team = team;
                    changed = true;
                }
            }
            ui->text("하루 발굴 수 (스카우트 1명, 하루):");
            ui->same_line();
            ui->set_next_width(220.0f);
            int perDay = settings.perDay;
            if (ui->slider_int("##perday", &perDay, 0, 18))
            {
                settings.perDay = perDay;
                changed = true;
            }
            ui->same_line();
            ui->text_disabled(settings.perDay == 0 ? "0 = 게임 그대로 (확률)" : "그날 간 곳의 후보 중 이만큼은 꼭 발굴");
            ui->tooltip("스카우트를 '발굴'로 보낸 날마다, 그곳(국내 대회 지역·해외 리그)의 후보를 이 수만큼 발굴합니다. 후보가 적으면 있는 만큼.");
            int guard = settings.guard ? 1 : 0;
            if (ui->checkbox("독점: 찜·독점한 선수를 CPU 구단이 쫓지 못하게", &guard))
            {
                settings.guard = guard != 0;
                changed = true;
            }
            ui->same_line();
            int guardFound = settings.guardFound ? 1 : 0;
            ui->begin_disabled(settings.guard ? 0 : 1);
            if (ui->checkbox("발굴한 선수도 모두 독점", &guardFound))
            {
                settings.guardFound = guardFound != 0;
                changed = true;
            }
            ui->end_disabled();
            ui->tooltip("CPU 구단의 관심·평가 값을 계속 0으로 둡니다. 드래프트·외국인 영입에서 CPU가 노리지 않게 하려는 것으로, 드래프트에서 직접 확인이 필요합니다.");
            int listNow = settings.listNow ? 1 : 0;
            if (ui->checkbox("찜·넣은 국내 선수를 지금 국내신인 리스트에 올리기", &listNow))
            {
                settings.listNow = listNow != 0;
                changed = true;
            }
            ui->tooltip("게임은 올해 지명·교섭 대상인 국내 후보만 보고 리스트(国内新人)에 보입니다(+0x62). 켜면 찜하거나 DB에서 넣은 국내 선수도 지금 대상이 됩니다. 실험 기능: 드래프트에서 확인 필요.");
            int pipes = settings.pipes ? 1 : 0;
            if (ui->checkbox("스카우트 파이프 최대 고정 (국내 6곳 · 해외 6곳, 스카우트 2명)", &pipes))
            {
                settings.pipes = pipes != 0;
                changed = true;
            }
            ui->tooltip("오피스의 国内パイプ·海外パイプ 막대를 모두 끝까지(값 10000) 매 순간 둡니다. 파이프가 굵을수록 국내 신인 정보와 외국인 발굴에 유리합니다. "
                        "기능별 '스카우터 최대'는 스카우트 화면을 열 때만 8000(막대 4칸)을 씁니다.");
            ui->text_disabled(("찜 " + std::to_string(settings.want.size()) + "명 · 독점 " + std::to_string(settings.keep.size()) +
                               "명. 찜한 선수는 '선수 치트 켜기'처럼 이 탭이 켜져 있을 때 바로 발굴됩니다.")
                                  .c_str());
            if (urban::drawRefresh(ui, log, "후보 목록"))
                g_refreshNow = true;
            if (urban::drawChanges(ui, log, "scoutchanges", 150.0f))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_log.lines.clear();
            }
            if (ui->begin_tab_bar("scouttabs"))
            {
                if (ui->begin_tab("국내 신인"))
                {
                    if (g_tab != 0)
                        g_place = 0;
                    g_tab = 0;
                    drawPool(ui, settings, rows, false, team, changed);
                    ui->end_tab();
                }
                if (ui->begin_tab("외국인"))
                {
                    if (g_tab != 1)
                        g_place = 0;
                    g_tab = 1;
                    drawPool(ui, settings, rows, true, team, changed);
                    ui->end_tab();
                }
                if (ui->begin_tab("DB에서 넣기"))
                {
                    drawDatabase(ui, db, rows, onRoster, message);
                    ui->end_tab();
                }
                ui->end_tab_bar();
            }
            if (changed)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_settings = settings;
                g_on = settings.enabled;
                g_perDay = settings.perDay;
                saveSettings(settings);
            }
        }
#endif
    }

    void install(PS2Runtime &runtime, uint8_t *rdram, const std::filesystem::path &userDir)
    {
        if (!rdram || !runtime.hasFunction(kRng))
            return;
        const char *mods = std::getenv("URBAN_MODS");
        if (mods && std::strcmp(mods, ";") == 0) // the launcher's safe start
            return;
        g_rdram = rdram;
        // The settings are per save (cheat_saves/<id>/scout_cheats.ini, chosen by the frame hook once a game runs); the
        // old file is kept for the owner's main save M00.
        g_legacyPath = userDir / "scout_cheats.ini";
        g_path.clear();
        g_settings = Settings{};
        g_fileInserts.clear(); // not at start
        for (const std::filesystem::path &file : {userDir / "scout_models.tsv", std::filesystem::path("scout_models.tsv")})
        {
            std::ifstream models(file, std::ios::binary);
            std::string line;
            while (std::getline(models, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                std::vector<std::string> fields;
                std::istringstream cells(line);
                std::string cell;
                while (std::getline(cells, cell, '\t'))
                    fields.push_back(cell);
                if (fields.size() < 2u)
                    continue;
                const bool byId = !fields[0].empty() && std::all_of(fields[0].begin(), fields[0].end(), [](char c) { return c >= '0' && c <= '9'; });
                if (!byId) // name, model, league (the site's list alone)
                {
                    g_models.emplace(fields[0], fields[1]);
                    continue;
                }
                const unsigned long id = std::strtoul(fields[0].c_str(), nullptr, 10);
                Note note;
                note.model = fields.size() > 2u ? fields[2] : std::string();
                note.position = fields.size() > 3u ? fields[3] : std::string();
                note.summary = fields.size() > 4u ? fields[4] : std::string();
                note.modelEn = fields.size() > 5u ? fields[5] : std::string();
                note.modelKo = fields.size() > 6u ? fields[6] : std::string();
                if (fields.size() > 1u && !note.model.empty())
                    g_models.emplace(fields[1], note.model);
                if (id > 0u && id <= 0xFFFFu)
                    g_notes[static_cast<uint16_t>(id)] = std::move(note);
            }
            if (!g_models.empty() || !g_notes.empty())
                break;
        }
        g_on = false;
        g_perDay = 0;
        FunctionHooks &hooks = FunctionHooks::instance();
        hooks.replace(kRng, "scout_roll", &rngHook, ReplacementStatus::Native, "scout cheats: finds a day");
        g_rng = hooks.original(kRng);
        if (!g_rng)
        {
            std::fprintf(stderr, "[scout] hook not installed\n");
            return;
        }
        ps2x::pnach::addFrameHook(frame);
        std::fprintf(stderr, "[scout] %s, %d a day, %zu wanted, %zu player notes, %zu models\n", g_settings.enabled ? "on" : "off",
                     g_settings.perDay, g_settings.want.size(), g_notes.size(), g_models.size());
#if defined(URBAN_SCOUT_MENU)
        ps2x::menu::addPanel("스카우트", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
    }
}
