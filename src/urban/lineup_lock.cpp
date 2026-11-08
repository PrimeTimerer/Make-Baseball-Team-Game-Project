#include "urban/lineup_lock.h"
#include "urban/function_hooks.h"
#include "urban/save_identity.h"

#include "ps2_runtime.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_LINEUP_MENU 1
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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace urban::lineup
{
    namespace
    {
        constexpr uint32_t kMatchSetup = 0x29E7D8u;
        constexpr uint32_t kTeamObj0 = 0x5D1278u, kTeamObj1 = 0x5D1274u, kCopies = 0x2008u;
        constexpr uint32_t kTeamTable = 0x5D1240u, kTeamRoster = 0x120u, kRosterClass = 4u, kOwnRosterClass = 0x6659F0u;
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr int kTeams = 12, kPairs = 10, kCopyCount = 25, kNoPosition = 15;
        constexpr const char *kPositions[10] = {"투수", "포수", "1루", "2루", "3루", "유격", "좌익", "중견", "우익", "지명"};
        constexpr const char *kTeamNames[kTeams] = {"라이온즈", "호크스",   "파이터즈", "마린즈",   "버팔로즈", "골든이글스",
                                                    "드래곤즈", "스왈로즈", "자이언츠", "타이거즈", "카프",     "요코하마"};

        // A row: the player (ID; 0 = the game's choice) and the position (kNoPosition = none, the empty pair 9).
        struct Row
        {
            int id = 0;
            int pos = kNoPosition;
        };
        struct Order
        {
            std::array<Row, kPairs> rows{};
            bool saved = false;
        };
        struct Settings
        {
            bool lock = false;
            Order normal, dh; // without / with DH
            bool rotationLock = false;
            std::vector<int> rotation; // starters by ID, in turn
            int rotationNext = 0;
        };

        uint8_t *g_rdram = nullptr;
        std::mutex g_mutex; // g_settings, g_last*, g_report (EE thread: the hook and the frame; GS thread: the menu)
        Settings g_settings;
        std::filesystem::path g_path;
        std::string g_profile;
        uint32_t g_generation = ~0u;
        bool g_dirty = false; // the hook moved the rotation on: the frame writes the file
        GuestFunction g_setupOriginal = nullptr;
        // The last own-team match set up: the game's lineup (before the lock), with or without DH, when, and what the lock did.
        Order g_lastGame;
        bool g_lastDh = false, g_haveLast = false;
        std::string g_lastClock, g_report;

        uint32_t r32(uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, g_rdram + (address & kRamMask), 4);
            return value;
        }
        uint16_t r16(uint32_t address)
        {
            uint16_t value;
            std::memcpy(&value, g_rdram + (address & kRamMask), 2);
            return value;
        }
        bool valid(uint32_t address) { return address >= 0x100000u && address < 0x2000000u; }

        int ownTeam()
        {
            for (int t = 0; t < kTeams; ++t)
            {
                const uint32_t object = r32(kTeamTable + 4u * static_cast<uint32_t>(t));
                const uint32_t roster = valid(object) ? r32(object + kTeamRoster) : 0u;
                if (valid(roster) && r32(roster + kRosterClass) == kOwnRosterClass)
                    return t;
            }
            return -1;
        }

        std::string clockNow()
        {
            const std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            localtime_s(&local, &now);
#else
            localtime_r(&now, &local);
#endif
            char text[16];
            std::strftime(text, sizeof(text), "%H:%M:%S", &local);
            return text;
        }

        // lineup_lock.ini: lock=, normal=/dh=<id>:<pos>,... (10), rotlock=, rotation=<id>,..., rotnext=
        std::string orderText(const Order &order)
        {
            std::string text;
            for (const Row &row : order.rows)
                text += (text.empty() ? "" : ",") + std::to_string(row.id) + ":" + std::to_string(row.pos);
            return text;
        }

        Order orderOf(const std::string &text)
        {
            Order order;
            std::istringstream list(text);
            std::string item;
            size_t i = 0;
            while (std::getline(list, item, ',') && i < order.rows.size())
            {
                const size_t colon = item.find(':');
                order.rows[i].id = std::atoi(item.c_str());
                order.rows[i].pos = colon == std::string::npos ? kNoPosition : std::clamp(std::atoi(item.c_str() + colon + 1), 0, kNoPosition);
                ++i;
            }
            order.saved = i == order.rows.size();
            return order;
        }

        Settings load(const std::filesystem::path &path)
        {
            Settings settings;
            std::ifstream file(path, std::ios::binary);
            std::string line;
            while (std::getline(file, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                const size_t equals = line.find('=');
                if (line.empty() || line[0] == '#' || equals == std::string::npos)
                    continue;
                const std::string key = line.substr(0, equals), value = line.substr(equals + 1);
                if (key == "lock")
                    settings.lock = value == "1";
                else if (key == "normal")
                    settings.normal = orderOf(value);
                else if (key == "dh")
                    settings.dh = orderOf(value);
                else if (key == "rotlock")
                    settings.rotationLock = value == "1";
                else if (key == "rotnext")
                    settings.rotationNext = std::max(0, std::atoi(value.c_str()));
                else if (key == "rotation")
                {
                    std::istringstream list(value);
                    std::string id;
                    while (std::getline(list, id, ','))
                        if (std::atoi(id.c_str()) > 0)
                            settings.rotation.push_back(std::atoi(id.c_str()));
                }
            }
            return settings;
        }

        void save(const Settings &settings, const std::filesystem::path &path)
        {
            if (path.empty())
                return;
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << "# 라인업 고정: 게임 메뉴(F2)의 \"라인업\" 탭이 씁니다 (이 세이브의 설정)\n"
                 << "# normal / dh = 타순 10칸 <선수 ID>:<수비 위치 0 투 1 포 2 1루 3 2루 4 3루 5 유격 6 좌익 7 중견 8 우익 9 지명 15 없음>"
                    " (ID 0 = 게임이 정함)\n"
                 << "lock=" << (settings.lock ? 1 : 0) << "\n";
            if (settings.normal.saved)
                file << "normal=" << orderText(settings.normal) << "\n";
            if (settings.dh.saved)
                file << "dh=" << orderText(settings.dh) << "\n";
            file << "rotlock=" << (settings.rotationLock ? 1 : 0) << "\n";
            std::string rotation;
            for (const int id : settings.rotation)
                rotation += (rotation.empty() ? "" : ",") + std::to_string(id);
            file << "rotation=" << rotation << "\n"
                 << "rotnext=" << settings.rotationNext << "\n";
        }

        // A saved order the game can play: each position once (no DH: 0-8, DH: 0-9), no player twice, the rest empty.
        std::string problemOf(const Order &order, bool dh)
        {
            std::set<int> positions, players;
            for (const Row &row : order.rows)
            {
                if (row.pos == kNoPosition)
                    continue;
                if (row.pos > 9 || (!dh && row.pos == 9))
                    return dh ? "수비 위치가 맞지 않습니다" : "DH 없는 오더에 지명타자가 있습니다";
                if (!positions.insert(row.pos).second)
                    return std::string(kPositions[row.pos]) + "가 두 번 있습니다";
                if (row.pos != 0 && row.id != 0 && !players.insert(row.id).second)
                    return "같은 선수가 두 번 있습니다";
            }
            if (positions.size() != (dh ? 10u : 9u))
                return dh ? "투수와 1루~지명 9자리를 하나씩 채워야 합니다" : "투수와 포수~우익 8자리를 하나씩 채워야 합니다";
            return {};
        }

        // sub_0029E7D8 (EE thread): an own-team match being set up. Its lineups (a2) are the game's choice; the game's
        // lineup is kept for the tab, then the lock's lineup and starter go into the own side's pairs, then the game's
        // function (nothing after it).
        void setupHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t lineups = getRegU32(ctx, 6) & kRamMask;
            // The own side: the match team whose copies are the own roster's players (a3 / t0 are the sides' player
            // counts, 25, not the teams).
            const int own = ownTeam();
            std::set<int> ownIds;
            if (own >= 0)
            {
                const uint32_t object = r32(kTeamTable + 4u * static_cast<uint32_t>(own));
                const uint32_t roster = valid(object) ? r32(object + kTeamRoster) : 0u;
                for (uint32_t k = 0; valid(roster) && k < 40u; ++k)
                    if (const uint32_t record = r32(roster + 8u + 4u * k); valid(record) && r16(record) != 0u)
                        ownIds.insert(r16(record));
            }
            int side = -1;
            for (int s = 0; s < 2 && !ownIds.empty(); ++s)
            {
                const uint32_t object = r32(s == 0 ? kTeamObj0 : kTeamObj1);
                int matches = 0;
                for (int k = 0; valid(object) && k < kCopyCount; ++k)
                    if (const uint32_t copy = r32(object + kCopies + 4u * static_cast<uint32_t>(k)); valid(copy) && ownIds.count(r16(copy)))
                        ++matches;
                if (matches >= 9)
                    side = s;
            }
            const uint32_t team = side < 0 ? 0u : r32(side == 0 ? kTeamObj0 : kTeamObj1);
            static const bool logCalls = std::getenv("URBAN_LINEUP_LOG") != nullptr;
            if (logCalls)
                std::fprintf(stderr, "[lineup] setup a2 0x%08x own team %d side %d\n", lineups, own, side);
            if (side >= 0 && valid(lineups) && valid(team))
            {
                const uint32_t pairs = lineups + 0x14u * static_cast<uint32_t>(side);
                std::array<int, kCopyCount> ids{};
                for (int k = 0; k < kCopyCount; ++k)
                {
                    const uint32_t copy = r32(team + kCopies + 4u * static_cast<uint32_t>(k));
                    ids[static_cast<size_t>(k)] = valid(copy) ? r16(copy) : 0;
                }
                auto copyOf = [&ids](int id) {
                    for (int k = 0; k < kCopyCount; ++k)
                        if (id != 0 && ids[static_cast<size_t>(k)] == id)
                            return k;
                    return -1;
                };
                Order game;
                bool dh = false;
                int pitcherRow = -1;
                for (int i = 0; i < kPairs; ++i)
                {
                    const int copy = static_cast<int8_t>(rdram[pairs + 2u * static_cast<uint32_t>(i)]);
                    const int pos = rdram[pairs + 2u * static_cast<uint32_t>(i) + 1u];
                    game.rows[static_cast<size_t>(i)] = {copy >= 0 && copy < kCopyCount ? ids[static_cast<size_t>(copy)] : 0, pos};
                    dh = dh || pos == 9;
                    if (pos == 0)
                        pitcherRow = i;
                }
                game.saved = true;
                static const bool logGame = std::getenv("URBAN_LINEUP_LOG") != nullptr; // tests: every own setup's game lineup
                if (logGame)
                {
                    std::string text;
                    for (const Row &row : game.rows)
                        text += " " + std::to_string(row.id) + ":" + std::to_string(row.pos);
                    std::fprintf(stderr, "[lineup] game lineup side %d (%s):%s\n", side, dh ? "DH" : "no DH", text.c_str());
                }
                std::string report;
                std::lock_guard<std::mutex> lock(g_mutex);
                g_lastGame = game;
                g_lastDh = dh;
                g_haveLast = true;
                g_lastClock = clockNow();
                // The lineup: saved rows by player; a saved player not among the 25 leaves his row to one of the game's
                // own batters not placed yet (in the game's order); pitcher rows keep the game's pitcher.
                const Order &saved = dh ? g_settings.dh : g_settings.normal;
                std::array<std::pair<int, int>, kPairs> next{};
                for (int i = 0; i < kPairs; ++i)
                    next[static_cast<size_t>(i)] = {static_cast<int8_t>(rdram[pairs + 2u * static_cast<uint32_t>(i)]), rdram[pairs + 2u * static_cast<uint32_t>(i) + 1u]};
                const int gamePitcher = pitcherRow >= 0 ? next[static_cast<size_t>(pitcherRow)].first : -1;
                // The match copies must already be this match's (every pair of the game's lineup an own player), else the
                // IDs could point at the wrong copies: then nothing is changed.
                bool copiesReady = true;
                for (const Row &row : game.rows)
                    copiesReady = copiesReady && (row.pos == kNoPosition || ownIds.count(row.id) != 0);
                const std::string problem = g_settings.lock && saved.saved ? problemOf(saved, dh) : std::string();
                const bool skip = !copiesReady && (g_settings.lock || g_settings.rotationLock);
                if (skip)
                {
                    std::fprintf(stderr, "[lineup] own side %d: the match copies are not this match's yet, lineup left as the game's\n", side);
                    g_report = g_lastClock + " 경기 선수 목록을 아직 읽을 수 없어 이번 경기는 게임 오더 그대로";
                }
                else if (!problem.empty() || (g_settings.lock && saved.saved && gamePitcher < 0))
                    report = "저장한 오더를 쓰지 않음: " + (problem.empty() ? std::string("게임 오더에 투수가 없습니다") : problem);
                else if (g_settings.lock && saved.saved)
                {
                    std::set<int> used;
                    std::vector<int> missing;
                    for (int i = 0; i < kPairs; ++i)
                    {
                        const Row &row = saved.rows[static_cast<size_t>(i)];
                        if (row.pos == kNoPosition)
                            next[static_cast<size_t>(i)] = {-1, kNoPosition};
                        else if (row.pos == 0)
                            next[static_cast<size_t>(i)] = {gamePitcher, 0};
                        else if (const int copy = copyOf(row.id); copy >= 0)
                        {
                            next[static_cast<size_t>(i)] = {copy, row.pos};
                            used.insert(copy);
                        }
                        else
                            missing.push_back(i);
                    }
                    used.insert(gamePitcher);
                    std::vector<int> spare;
                    for (int i = 0; i < kPairs; ++i)
                    {
                        const int copy = static_cast<int8_t>(rdram[pairs + 2u * static_cast<uint32_t>(i)]);
                        if (copy >= 0 && rdram[pairs + 2u * static_cast<uint32_t>(i) + 1u] != 0 && !used.count(copy))
                            spare.push_back(copy);
                    }
                    if (missing.size() > spare.size())
                    {
                        // Not enough of the game's batters to fill in: this match keeps the game's lineup.
                        for (int i = 0; i < kPairs; ++i)
                            next[static_cast<size_t>(i)] = {static_cast<int8_t>(rdram[pairs + 2u * static_cast<uint32_t>(i)]), rdram[pairs + 2u * static_cast<uint32_t>(i) + 1u]};
                        report = "저장한 오더를 쓰지 않음: 1군(경기 25명)에 없는 선수가 많아 채울 수 없습니다";
                    }
                    else
                    {
                        for (size_t m = 0; m < missing.size(); ++m)
                            next[static_cast<size_t>(missing[m])] = {spare[m], saved.rows[static_cast<size_t>(missing[m])].pos};
                        report = std::string("저장한 오더(") + (dh ? "DH 있음" : "DH 없음") + ")로 시작" +
                                 (missing.empty() ? std::string() : " · 1군에 없는 " + std::to_string(missing.size()) + "명 자리는 게임 선수로 채움");
                    }
                }
                // The rotation: its next starter who is on the match roster (and not batting) into the pitcher's pair.
                if (!skip && g_settings.rotationLock && !g_settings.rotation.empty())
                {
                    int pitcherAt = -1;
                    for (int i = 0; i < kPairs; ++i)
                        if (next[static_cast<size_t>(i)].second == 0)
                            pitcherAt = i;
                    const int n = static_cast<int>(g_settings.rotation.size());
                    for (int step = 0; step < n && pitcherAt >= 0; ++step)
                    {
                        const int index = (g_settings.rotationNext + step) % n;
                        const int copy = copyOf(g_settings.rotation[static_cast<size_t>(index)]);
                        bool batting = false;
                        for (int i = 0; i < kPairs; ++i)
                            batting = batting || (i != pitcherAt && next[static_cast<size_t>(i)].first == copy);
                        if (copy < 0 || batting)
                            continue;
                        next[static_cast<size_t>(pitcherAt)].first = copy;
                        g_settings.rotationNext = (index + 1) % n;
                        g_dirty = true;
                        report += std::string(report.empty() ? "" : " · ") + "로테이션 " + std::to_string(index + 1) + "번째 선발";
                        break;
                    }
                }
                if (!report.empty())
                {
                    for (int i = 0; i < kPairs; ++i)
                    {
                        rdram[pairs + 2u * static_cast<uint32_t>(i)] = static_cast<uint8_t>(static_cast<int8_t>(next[static_cast<size_t>(i)].first));
                        rdram[pairs + 2u * static_cast<uint32_t>(i) + 1u] = static_cast<uint8_t>(next[static_cast<size_t>(i)].second);
                    }
                    std::string text;
                    for (const auto &[copy, pos] : next)
                        text += " " + std::to_string(copy >= 0 && copy < kCopyCount ? ids[static_cast<size_t>(copy)] : 0) + ":" + std::to_string(pos);
                    std::fprintf(stderr, "[lineup] own side %d: %s ->%s\n", side, report.c_str(), text.c_str());
                    g_report = g_lastClock + " " + report;
                }
            }
            g_setupOriginal(rdram, ctx, runtime);
        }

        // EE thread, every vblank: this save's settings, and the rotation's next starter written back.
        void frame()
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (const uint32_t generation = urban::saveid::generation(); generation != g_generation)
            {
                g_generation = generation;
                g_profile = urban::saveid::current();
                g_path = urban::saveid::settingsFile(g_profile, "lineup_lock.ini");
                std::error_code error;
                g_settings = !g_path.empty() && std::filesystem::exists(g_path, error) ? load(g_path) : Settings{};
                g_haveLast = false;
                g_report.clear();
                std::fprintf(stderr, "[lineup] save %s: lineup lock %s (normal %d, dh %d), rotation lock %s (%zu)\n",
                             g_profile.empty() ? "(none)" : g_profile.c_str(), g_settings.lock ? "on" : "off", g_settings.normal.saved ? 1 : 0,
                             g_settings.dh.saved ? 1 : 0, g_settings.rotationLock ? "on" : "off", g_settings.rotation.size());
            }
            if (g_dirty)
            {
                g_dirty = false;
                save(g_settings, g_path);
            }
        }

#if defined(URBAN_LINEUP_MENU)
        struct Player
        {
            int id = 0;
            bool pitcher = false;
            std::string label;
        };

        std::string nameOf(uint32_t record)
        {
#if defined(_WIN32)
            char sjis[16];
            std::memcpy(sjis, g_rdram + (record & kRamMask) + 4u, sizeof(sjis));
            int length = 0;
            while (length < 16 && sjis[length] != '\0')
                ++length;
            wchar_t wide[16];
            const int count = length > 0 ? MultiByteToWideChar(932, 0, sjis, length, wide, 16) : 0;
            std::wstring name;
            for (int i = 0; i < count; ++i)
                if (wide[i] != L' ' && wide[i] != 0x3000)
                    name += wide[i];
            char text[96];
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), text, sizeof(text) - 1, nullptr, nullptr);
            return std::string(text, bytes > 0 ? static_cast<size_t>(bytes) : 0u);
#else
            (void)record;
            return {};
#endif
        }

        // The own roster for the menu's choices (menu thread, reading RAM like the other tabs).
        std::vector<Player> ownPlayers(int own)
        {
            std::vector<Player> players;
            const uint32_t object = own >= 0 ? r32(kTeamTable + 4u * static_cast<uint32_t>(own)) : 0u;
            const uint32_t roster = valid(object) ? r32(object + kTeamRoster) : 0u;
            if (!valid(roster))
                return players;
            for (uint32_t k = 0; k < 40u; ++k)
            {
                const uint32_t record = r32(roster + 8u + 4u * k);
                if (!valid(record) || r16(record) == 0u)
                    continue;
                Player player;
                player.id = r16(record);
                player.pitcher = g_rdram[(record + 0x26u) & kRamMask] != 0u;
                player.label = "#" + std::to_string(g_rdram[(record + 0x17u) & kRamMask]) + " " + nameOf(record) + (player.pitcher ? " (투)" : "");
                players.push_back(player);
            }
            return players;
        }

        std::string labelOf(const std::vector<Player> &players, int id)
        {
            if (id == 0)
                return "(게임이 정함)";
            for (const Player &player : players)
                if (player.id == id)
                    return player.label;
            return "선수 ID " + std::to_string(id) + " (지금 로스터에 없음)";
        }

        void drawOrder(const Pcsx2GsUi *ui, const std::vector<Player> &players, Order &order, int id, bool &changed)
        {
            std::vector<std::string> labels{"(게임이 정함)"};
            for (const Player &player : players)
                labels.push_back(player.label);
            std::vector<const char *> items;
            for (const std::string &label : labels)
                items.push_back(label.c_str());
            const char *positions[] = {"투수", "포수", "1루", "2루", "3루", "유격", "좌익", "중견", "우익", "지명", "없음"};
            for (int i = 0; i < kPairs; ++i)
            {
                Row &row = order.rows[static_cast<size_t>(i)];
                ui->push_id(id * 100 + i);
                char head[16];
                std::snprintf(head, sizeof(head), "%2d번", i + 1);
                ui->text(head);
                ui->same_line();
                int pick = 0;
                for (size_t p = 0; p < players.size(); ++p)
                    if (players[p].id == row.id)
                        pick = static_cast<int>(p) + 1;
                ui->set_next_width(300.0f);
                if (ui->combo("##player", &pick, items.data(), static_cast<int>(items.size())))
                {
                    row.id = pick == 0 ? 0 : players[static_cast<size_t>(pick - 1)].id;
                    changed = true;
                }
                ui->same_line();
                int pos = row.pos >= 0 && row.pos <= 9 ? row.pos : 10;
                ui->set_next_width(90.0f);
                if (ui->combo("##pos", &pos, positions, 11))
                {
                    row.pos = pos == 10 ? kNoPosition : pos;
                    changed = true;
                }
                ui->same_line();
                if (ui->small_button("위로") && i > 0)
                {
                    std::swap(order.rows[static_cast<size_t>(i)], order.rows[static_cast<size_t>(i - 1)]);
                    changed = true;
                }
                ui->same_line();
                if (ui->small_button("아래로") && i + 1 < kPairs)
                {
                    std::swap(order.rows[static_cast<size_t>(i)], order.rows[static_cast<size_t>(i + 1)]);
                    changed = true;
                }
                ui->pop_id();
            }
        }

        void draw(const Pcsx2GsUi *ui)
        {
            Settings settings;
            Order last;
            bool lastDh = false, haveLast = false;
            std::string lastClock, report, profile;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                settings = g_settings;
                last = g_lastGame;
                lastDh = g_lastDh;
                haveLast = g_haveLast;
                lastClock = g_lastClock;
                report = g_report;
                profile = g_profile;
            }
            const int own = ownTeam();
            const std::vector<Player> players = ownPlayers(own);
            bool changed = false;
            ui->text_disabled(("세이브: " + urban::saveid::describe(profile) + " — 이 세이브의 라인업 고정" +
                               (own >= 0 ? std::string(" · 내 구단 ") + kTeamNames[own] : std::string()))
                                  .c_str());
            int lockOn = settings.lock ? 1 : 0;
            if (ui->checkbox("저장한 오더로 고정 (내 구단 경기가 시작될 때마다)", &lockOn))
            {
                settings.lock = lockOn != 0;
                changed = true;
            }
            ui->tooltip("경기를 준비할 때 게임이 정한 오더 대신 아래 저장한 오더를 씁니다(선수 ID 기준). 2군에서 올라온 선수나 새로 영입한 "
                        "선수가 있어도 저장한 스타팅으로 시작합니다. 저장한 선수가 1군(경기 25명)에 없으면 그 자리만 게임 선수로 채웁니다.");
            ui->text_disabled(report.empty() ? "아직 이번 실행에서 고정을 쓴 경기가 없습니다." : ("마지막 적용: " + report).c_str());

            ui->separator_text("지난 경기 시작 오더 (게임이 정한 것, 고정 전)");
            if (!haveLast)
                ui->text_disabled("내 구단 경기를 한 번 시작하면 그 경기의 오더가 여기 나옵니다 (3D·결과 표시·시뮬레이션 모두).");
            else
            {
                ui->text_disabled((lastClock + " · " + (lastDh ? "DH 있음" : "DH 없음")).c_str());
                for (int i = 0; i < kPairs; ++i)
                {
                    const Row &row = last.rows[static_cast<size_t>(i)];
                    if (row.pos == kNoPosition)
                        continue;
                    ui->text((std::to_string(i + 1) + "번 " + labelOf(players, row.id) + " · " + (row.pos <= 9 ? kPositions[row.pos] : "?")).c_str());
                }
                if (ui->button(lastDh ? "이 오더를 'DH 있음'으로 저장##takedh" : "이 오더를 'DH 없음'으로 저장##takenormal"))
                {
                    (lastDh ? settings.dh : settings.normal) = last;
                    changed = true;
                }
            }

            for (int which = 0; which < 2; ++which)
            {
                Order &order = which == 0 ? settings.normal : settings.dh;
                ui->separator_text(which == 0 ? "저장한 오더: DH 없음 (투수가 타석에)" : "저장한 오더: DH 있음 (지명타자)");
                if (!order.saved)
                {
                    ui->text_disabled("아직 저장하지 않았습니다. 위 '지난 경기 시작 오더'에서 저장하거나 아래로 새로 만드세요.");
                    if (ui->small_button(which == 0 ? "빈 오더 만들기##newnormal" : "빈 오더 만들기##newdh"))
                    {
                        order = Order{};
                        order.saved = true;
                        // No DH: 포수..우익 then the pitcher 9th; DH: 포수..지명 then the pitcher's pair 10th.
                        for (int i = 0; i < 9; ++i)
                            order.rows[static_cast<size_t>(i)].pos = which == 0 && i == 8 ? 0 : i + 1;
                        if (which == 1)
                            order.rows[9].pos = 0;
                        changed = true;
                    }
                    continue;
                }
                drawOrder(ui, players, order, which + 1, changed);
                if (const std::string problem = problemOf(order, which == 1); !problem.empty())
                    ui->text_colored(0xFF5050FFu, ("이 오더는 쓰지 않습니다: " + problem).c_str());
                if (ui->small_button(which == 0 ? "이 저장 지우기##clearnormal" : "이 저장 지우기##cleardh"))
                {
                    order = Order{};
                    changed = true;
                }
            }
            ui->text_disabled("투수 칸(수비 위치 '투수')은 그 경기 선발이 들어갑니다(아래 로테이션 고정이면 그 순서대로). '없음'은 비운 칸입니다.");

            ui->separator_text("로테이션 고정 (선발 순서)");
            int rotationOn = settings.rotationLock ? 1 : 0;
            if (ui->checkbox("이 순서대로 선발", &rotationOn))
            {
                settings.rotationLock = rotationOn != 0;
                changed = true;
            }
            ui->tooltip("내 구단 경기를 준비할 때마다 아래 순서의 다음 투수를 선발로 넣습니다(1군에 없거나 타자로 나가면 그다음 투수). "
                        "오더 고정과 따로 켤 수 있습니다.");
            std::vector<Player> pitchers;
            for (const Player &player : players)
                if (player.pitcher)
                    pitchers.push_back(player);
            std::vector<std::string> pitcherLabels;
            for (const Player &player : pitchers)
                pitcherLabels.push_back(player.label);
            std::vector<const char *> pitcherItems;
            for (const std::string &label : pitcherLabels)
                pitcherItems.push_back(label.c_str());
            int remove = -1;
            for (size_t i = 0; i < settings.rotation.size(); ++i)
            {
                ui->push_id(900 + static_cast<int>(i));
                const bool nextOne = settings.rotationLock && static_cast<int>(i) == settings.rotationNext % static_cast<int>(settings.rotation.size());
                ui->text((std::to_string(i + 1) + "번째" + (nextOne ? " (다음)" : "")).c_str());
                ui->same_line();
                int pick = -1;
                for (size_t p = 0; p < pitchers.size(); ++p)
                    if (pitchers[p].id == settings.rotation[i])
                        pick = static_cast<int>(p);
                if (pick < 0)
                    ui->text_disabled(labelOf(players, settings.rotation[i]).c_str());
                else
                {
                    ui->set_next_width(300.0f);
                    if (!pitcherItems.empty() && ui->combo("##rot", &pick, pitcherItems.data(), static_cast<int>(pitcherItems.size())))
                    {
                        settings.rotation[i] = pitchers[static_cast<size_t>(pick)].id;
                        changed = true;
                    }
                }
                ui->same_line();
                if (ui->small_button("빼기"))
                    remove = static_cast<int>(i);
                ui->same_line();
                if (ui->small_button("다음 선발로"))
                {
                    settings.rotationNext = static_cast<int>(i);
                    changed = true;
                }
                ui->pop_id();
            }
            if (remove >= 0)
            {
                settings.rotation.erase(settings.rotation.begin() + remove);
                settings.rotationNext = 0;
                changed = true;
            }
            if (!pitchers.empty() && settings.rotation.size() < 8u && ui->small_button("선발 한 명 더하기"))
            {
                settings.rotation.push_back(pitchers.front().id);
                changed = true;
            }
            if (haveLast && !pitchers.empty())
            {
                ui->same_line();
                for (const Row &row : last.rows)
                    if (row.pos == 0 && row.id != 0 && ui->small_button("지난 경기 선발을 더하기"))
                    {
                        settings.rotation.push_back(row.id);
                        changed = true;
                    }
            }
            if (!changed)
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_settings = settings;
            save(settings, g_path);
        }
#endif
    }

    void install(PS2Runtime &runtime, uint8_t *rdram)
    {
        if (!rdram || !runtime.hasFunction(kMatchSetup))
            return;
        const char *mods = std::getenv("URBAN_MODS");
        if (mods && std::strcmp(mods, ";") == 0) // the launcher's safe start
            return;
        g_rdram = rdram;
        FunctionHooks &hooks = FunctionHooks::instance();
        if (!hooks.replace(kMatchSetup, "lineup_lock_setup", &setupHook, ReplacementStatus::Native,
                           "라인업 고정: the own team's lineup and starter written before sub_0029E7D8 copies them"))
            return;
        g_setupOriginal = hooks.original(kMatchSetup);
        ps2x::pnach::addFrameHook(frame);
#if defined(URBAN_LINEUP_MENU)
        ps2x::menu::addPanel("라인업", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
        std::fprintf(stderr, "[lineup] match setup hook installed (sub_0029E7D8)\n");
    }
}
