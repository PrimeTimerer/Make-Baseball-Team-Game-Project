#include "urban/scout_cheats.h"
#include "urban/function_hooks.h"

#include "ps2_runtime.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"

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
        // The database ([0x5D1270]: +0 the records, +0xC their count): 0xA6-byte records sorted by id.
        constexpr uint32_t kDatabase = 0x5D1270u, kRecordSize = 0xA6u;
        constexpr int kTeams = 12;
        // The roll: sub_0015CEA0(n) = rand() % n, called with 100 by both finds; their count of the day's finds sits in
        // the caller's frame.
        constexpr uint32_t kRng = 0x15CEA0u, kForeignRoll = 0x19424Cu, kDomesticRoll = 0x1939F0u;
        constexpr uint32_t kForeignCount = 0x94u, kDomesticCount = 0xB4u;
        constexpr uint32_t kRamSize = 0x2000000u;

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
            std::set<uint16_t> want; // found at once
            std::set<uint16_t> keep; // protected without being on the wish list
        };
        // insert=<id>,<overseas 0/1>,<group>[,<from overseas 0/1>,<from index>] lines: done once when the file changes
        // while the game runs (not at start), dropped by the next save. For tests and power users.
        struct FileInsert
        {
            uint16_t id = 0u;
            bool foreign = false;
            int group = 0;
            bool fromForeign = false;
            int from = -1;
        };
        std::vector<FileInsert> g_fileInserts;
        struct Row
        {
            uint16_t id = 0u;
            bool foreign = false;
            int index = 0; // in its pool: domestic area * 39 + kind * 13 + n, overseas league * 18 + n
            std::string name;
            int age = 0, state = 0, status = 0, cpu = 0;
        };
        struct Write
        {
            bool foreign = false;
            int index = 0, state = 0;
        };
        // Put database player `id` into an empty slot of `group` (domestic: area * 3 + kind, 0-17; overseas: league
        // 0-9), as found; `from` >= 0: the entry moved there (that slot is emptied).
        struct Insert
        {
            uint16_t id = 0u;
            bool foreign = false;
            int group = 0;
            bool fromForeign = false;
            int from = -1;
        };
        struct DbRow
        {
            uint16_t id = 0u;
            std::string name;
            int age = 0;
        };

        uint8_t *g_rdram = nullptr;
        std::filesystem::path g_path;
        std::mutex g_mutex; // settings, rows, writes: the menu (GS thread) and the frame hook (EE thread)
        Settings g_settings;
        std::filesystem::file_time_type g_time{};
        std::vector<Row> g_rows;
        std::set<uint16_t> g_onRoster; // ids on the 12 rosters now (for the menu's 선수 넣기)
        // Overseas players' real models: scout_models.tsv (name<TAB>model, made by tools/scout_models.py from
        // gamezukushi.com's lists; 架空 = made up, 現役 = the real player himself). Read once at start.
        std::map<std::string, std::string> g_models;
        std::vector<Write> g_writes;
        std::vector<Insert> g_inserts;
        std::vector<DbRow> g_db;         // the database for the menu (built once a load)
        uint32_t g_dbBase = 0u;
        std::string g_message;           // the last insert's result, for the menu
        int g_team = -1;
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
                else if (key == "want")
                    settings.want = idsOf(value);
                else if (key == "keep")
                    settings.keep = idsOf(value);
                else if (key == "insert")
                {
                    int fields[5] = {0, 0, 0, 0, -1};
                    std::istringstream list(value);
                    std::string item;
                    for (int k = 0; k < 5 && std::getline(list, item, ','); ++k)
                        fields[k] = std::atoi(trim(item).c_str());
                    if (fields[0] > 0 && fields[0] <= 0xFFFF)
                        g_fileInserts.push_back(FileInsert{static_cast<uint16_t>(fields[0]), fields[1] != 0, fields[2], fields[3] != 0, fields[4]});
                }
            }
            return settings;
        }

        void saveSettings(const Settings &settings)
        {
            {
                std::ofstream file(g_path, std::ios::binary | std::ios::trunc);
                file << "# 스카우트 치트: 게임 메뉴(F2)의 \"스카우트\" 탭이 씁니다\n"
                     << "enabled=" << (settings.enabled ? 1 : 0) << "\n"
                     << "perday=" << settings.perDay << "\n"
                     << "guard=" << (settings.guard ? 1 : 0) << "\n"
                     << "guardfound=" << (settings.guardFound ? 1 : 0) << "\n"
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

        // EE thread, at every vblank: the own team's scouting object, the wish list and the protection.
        void frame()
        {
            static unsigned tick = 0u;
            if (++tick % 10u != 0u)
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (tick % 60u == 0u)
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
                        g_inserts.push_back(Insert{insert.id, insert.foreign, insert.group, insert.fromForeign, insert.from});
                    g_fileInserts.clear();
                    std::fprintf(stderr, "[scout] settings reloaded: %s, %d a day, %zu wanted\n", g_settings.enabled ? "on" : "off",
                                 g_settings.perDay, g_settings.want.size());
                }
            }
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
                return;
            }
            if (team != g_team)
                std::fprintf(stderr, "[scout] own team %d, scouting at 0x%08x\n", team, scouting);
            g_team = team;
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
                    continue;
                }
                // An empty slot; none: the first candidate there the own team has not found, wished or kept (taken out).
                int slot = -1;
                for (int k = 0; k < size && slot < 0; ++k)
                    if (read16(entryOf(insert.foreign, first + k)) == 0u)
                        slot = first + k;
                std::string replaced;
                for (int k = 0; k < size && slot < 0; ++k)
                {
                    const uint32_t other = entryOf(insert.foreign, first + k);
                    const uint16_t otherId = read16(other);
                    if (g_rdram[stateOf(insert.foreign, first + k)] == 0u && !g_settings.want.count(otherId) && !g_settings.keep.count(otherId) &&
                        !(insert.from >= 0 && insert.fromForeign == insert.foreign && insert.from == first + k))
                    {
                        slot = first + k;
                        replaced = nameAt(other + kName);
                    }
                }
                if (slot < 0)
                {
                    g_message = "그곳의 13칸·18칸이 모두 발굴·찜한 선수라 넣을 자리가 없습니다";
                    continue;
                }
                const uint32_t entry = entryOf(insert.foreign, slot);
                buildEntry(entry, record, insert.foreign, insert.group % 3);
                g_rdram[stateOf(insert.foreign, slot)] = 3u;
                g_rdram[entry + kFresh] = 0x0Cu;
                write16(entry + kInfo, 0xFFFFu);
                if (insert.from >= 0)
                {
                    std::memset(g_rdram + entryOf(insert.fromForeign, insert.from), 0, kEntry);
                    g_rdram[stateOf(insert.fromForeign, insert.from)] = 0u;
                }
                g_message = nameAt(entry + kName) + (insert.from >= 0 ? " 옮김: " : " 넣음: ") +
                            (insert.foreign ? std::string(kLeagues[insert.group])
                                            : std::string(kAreas[insert.group / 3]) + " · " + kKinds[insert.group % 3]) +
                            " (발굴 상태)" + (replaced.empty() ? std::string() : ", 빈 칸이 없어 미발굴 후보 " + replaced + "와(과) 바꿈");
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
                        g_db.push_back(std::move(row));
                    }
                g_dbBase = base;
            }
            const bool rebuild = tick % 30u == 0u;
            if (rebuild)
            {
                g_rows.clear();
                g_onRoster.clear();
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
                            g_onRoster.insert(read16(record));
                    }
                }
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
                    if (g_settings.enabled)
                    {
                        // The wish list: found now, everything shown (as a find does: state 3, +0x63 = 0x0C).
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
                        row.age = g_rdram[entry + kAge];
                        row.state = state;
                        row.status = g_rdram[entry + kStatus];
                        for (int t = 0; t < kTeams; ++t)
                            if (t != team && g_rdram[entry + kInterest + static_cast<uint32_t>(t)] >= 2u)
                                ++row.cpu;
                        g_rows.push_back(std::move(row));
                    }
                }
            }
        }

#if defined(URBAN_SCOUT_MENU)
        char g_search[64] = "";
        int g_place = 0;  // 0 all, else area / league + 1
        int g_show = 0;   // 0 all, 1 found, 2 not found, 3 wanted or kept
        int g_tab = 0;
        int g_toArea = 0, g_toKind = 0, g_toLeague = 0; // where 선수 넣기 / 옮기기 put a player
        char g_dbSearch[64] = "";

        // "이름 (모델: …)" when the lists know a real model.
        std::string displayName(const std::string &name, bool overseas)
        {
            if (!overseas)
                return name.empty() ? "?" : name;
            const auto model = g_models.find(name);
            if (model == g_models.end() || model->second.empty() || model->second == "架空" || model->second == "現役")
                return name.empty() ? "?" : name;
            return name + " (모델: " + model->second + ")";
        }

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

        void drawPool(const Pcsx2GsUi *ui, Settings &settings, const std::vector<Row> &rows, bool overseas, bool &changed)
        {
            ui->set_next_width(200.0f);
            ui->input_text("##search", "이름 · ID", g_search, sizeof(g_search));
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
            std::string query = trim(g_search);
            std::vector<const Row *> shown;
            for (const Row &row : rows)
            {
                if (row.foreign != overseas)
                    continue;
                if (g_place > 0 && (overseas ? row.index / 18 : row.index / 39) != g_place - 1)
                    continue;
                const bool wanted = settings.want.count(row.id) || settings.keep.count(row.id);
                if ((g_show == 1 && row.state < 3) || (g_show == 2 && row.state >= 3) || (g_show == 3 && !wanted))
                    continue;
                if (!query.empty() && row.name.find(query) == std::string::npos && std::to_string(row.id) != query)
                    continue;
                shown.push_back(&row);
            }
            ui->text(overseas ? "국내 목록으로 옮길 곳:" : "해외 목록으로 옮길 곳:");
            ui->same_line();
            drawTarget(ui, !overseas);
            ui->same_line();
            ui->text_disabled("(줄의 '옮기기' = 그곳 빈 칸에 발굴 상태로, 원래 칸은 비움)");
            ui->text_disabled(("보이는 후보 " + std::to_string(shown.size()) + "명").c_str());
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
            if (!ui->begin_table(overseas ? "foreign" : "domestic", 8,
                                 PCSX2GS_TABLE_SCROLL | PCSX2GS_TABLE_ROW_BG | PCSX2GS_TABLE_BORDERS | PCSX2GS_TABLE_RESIZABLE, 0.0f, 0.0f))
                return;
            ui->table_freeze(0, 1);
            ui->table_column("찜", 0, 34.0f);
            ui->table_column("독점", 0, 40.0f);
            ui->table_column("이름", 0, overseas ? 300.0f : 130.0f);
            ui->table_column("나이", 0, 36.0f);
            ui->table_column(overseas ? "리그" : "지역 · 구분", 0, 170.0f);
            ui->table_column("우리 구단", 0, 100.0f);
            ui->table_column("CPU 관심", 0, 64.0f);
            ui->table_column("", 0, 0.0f);
            ui->table_headers();
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
                    ui->text(displayName(row.name, row.foreign).c_str());
                    ui->tooltip(("선수 ID " + std::to_string(row.id)).c_str());
                    ui->table_next_column();
                    ui->text(std::to_string(row.age).c_str());
                    ui->table_next_column();
                    ui->text(placeText(row).c_str());
                    ui->table_next_column();
                    if (row.state >= 3)
                        ui->text_colored(0xFF66CCFFu, stateText(row.state));
                    else
                        ui->text_disabled(stateText(row.state));
                    ui->table_next_column();
                    if (row.cpu > 0)
                        ui->text_colored(0xFF5050FFu, (std::to_string(row.cpu) + "구단").c_str());
                    else
                        ui->text_disabled("-");
                    ui->table_next_column();
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

        // Any database player into an empty slot of a domestic area and kind or an overseas league, found.
        void drawDatabase(const Pcsx2GsUi *ui, const std::vector<DbRow> &db, const std::vector<Row> &rows,
                          const std::set<uint16_t> &onRoster, const std::string &message)
        {
            ui->text_wrapped("게임 DB의 선수(현역·OB·아마추어·외국인 모두)를 올해 국내 신인 목록이나 해외 리그 목록의 빈 칸에 발굴 상태로 넣습니다. "
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
            ui->set_next_width(220.0f);
            ui->input_text("##dbsearch", "이름 · ID로 찾기", g_dbSearch, sizeof(g_dbSearch));
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
                    if (row.name.find(query) != std::string::npos || compact.find(query) != std::string::npos || std::to_string(row.id) == query)
                        shown.push_back(&row);
                }
            if (shown.empty())
                return;
            std::map<uint16_t, const Row *> pooled;
            for (const Row &row : rows)
                pooled[row.id] = &row;
            if (!ui->begin_table("database", 5, PCSX2GS_TABLE_SCROLL | PCSX2GS_TABLE_ROW_BG | PCSX2GS_TABLE_BORDERS, 0.0f, 0.0f))
                return;
            ui->table_freeze(0, 1);
            ui->table_column("ID", 0, 50.0f);
            ui->table_column("이름", 0, 320.0f);
            ui->table_column("나이", 0, 36.0f);
            ui->table_column("지금", 0, 170.0f);
            ui->table_column("", 0, 0.0f);
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
                    ui->text(displayName(row.name, true).c_str());
                    ui->table_next_column();
                    ui->text(std::to_string(row.age).c_str());
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
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                settings = g_settings;
                rows = g_rows;
                team = g_team;
                db = g_db;
                onRoster = g_onRoster;
                message = g_message;
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
            ui->text_disabled(("찜 " + std::to_string(settings.want.size()) + "명 · 독점 " + std::to_string(settings.keep.size()) +
                               "명. 찜한 선수는 '선수 치트 켜기'처럼 이 탭이 켜져 있을 때 바로 발굴됩니다.")
                                  .c_str());
            if (ui->begin_tab_bar("scouttabs"))
            {
                if (ui->begin_tab("국내 신인"))
                {
                    if (g_tab != 0)
                        g_place = 0;
                    g_tab = 0;
                    drawPool(ui, settings, rows, false, changed);
                    ui->end_tab();
                }
                if (ui->begin_tab("외국인"))
                {
                    if (g_tab != 1)
                        g_place = 0;
                    g_tab = 1;
                    drawPool(ui, settings, rows, true, changed);
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
        g_path = userDir / "scout_cheats.ini";
        g_settings = loadSettings();
        g_fileInserts.clear(); // not at start
        for (const std::filesystem::path &file : {userDir / "scout_models.tsv", std::filesystem::path("scout_models.tsv")})
        {
            std::ifstream models(file, std::ios::binary);
            std::string line;
            while (std::getline(models, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                const size_t tab = line.find('\t');
                if (line.empty() || line[0] == '#' || tab == std::string::npos)
                    continue;
                const size_t second = line.find('\t', tab + 1);
                g_models.emplace(line.substr(0, tab), line.substr(tab + 1, second == std::string::npos ? std::string::npos : second - tab - 1));
            }
            if (!g_models.empty())
                break;
        }
        std::error_code error;
        g_time = std::filesystem::last_write_time(g_path, error);
        g_on = g_settings.enabled;
        g_perDay = g_settings.perDay;
        FunctionHooks &hooks = FunctionHooks::instance();
        hooks.replace(kRng, "scout_roll", &rngHook, ReplacementStatus::Native, "scout cheats: finds a day");
        g_rng = hooks.original(kRng);
        if (!g_rng)
        {
            std::fprintf(stderr, "[scout] hook not installed\n");
            return;
        }
        ps2x::pnach::addFrameHook(frame);
        std::fprintf(stderr, "[scout] %s, %d a day, %zu wanted, %zu overseas models\n", g_settings.enabled ? "on" : "off", g_settings.perDay,
                     g_settings.want.size(), g_models.size());
#if defined(URBAN_SCOUT_MENU)
        ps2x::menu::addPanel("스카우트", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
    }
}
