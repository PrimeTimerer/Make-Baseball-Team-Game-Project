#include "urban/stadium_cheats.h"
#include "urban/change_log.h"
#include "urban/save_identity.h"

#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_STADIUM_MENU 1
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace urban::stadium
{
    namespace
    {
        // SLPM-65853, found 2026-11-06 (MngStadium: mng::sdm::CSdmMain; the club accessors at 0x16C000-0x171000).
        // The club object [0x5CA114] (the same one the camp and the weekly report use):
        //   +0x13A0 stadium, +0x13A3 turf, +0x13A6 seats: [level][weeks left][state] each. sub_0016D3F0 starts an upgrade
        //     (state 1, the weeks from the master data); the weekly update sub_0016D4C8 counts the weeks down and then
        //     raises the level by one (state 2, a news item); at stadium level 6 (ドーム球場建設) it also sets +0x1274 = 1.
        //     Tops: stadium 6, turf 3 (内外野天然芝), seats 4 (リクライニングシート).
        //   +0x13A9: 13 facility counts by type (0 トイレ ... 12 公衆電話); the effects are count x effect by type
        //     (sub_0016E050), the counts go up and down with sub_0016DEE0 / sub_0016DF68.
        //   +0x1420: 24 slots, the facility id (type + 1) that starts there, 0 none; +0x1438: 8 rows x 4 squares, the slot
        //     that covers each square, 0xFF free. Rows of 4, 3, 3, 3, 3, 2, 2, 4 squares (slots 0-3, 4-6, ... 20-23,
        //     sub_0023A618); the stadium's level opens rows: level 0 the two 1F rows (内野側 4, 外野側 3), each level one
        //     more, ドーム all 8. sub_0028FA50 places (the slot, its squares, count + 1), sub_0028FB70 removes.
        //   The 球場施設 screen works on a copy of +0x1420..+0x1457 and writes it back when it closes.
        //   +0x13D8: practice facilities, 11 x [level 0-3][weeks left][state] (メイン練習場 ... リラクゼーションルーム);
        //     the weekly update sub_0016CE18 raises the level by one when the weeks run out with state 1.
        // The master data [0x5D10E8] (0x30-byte entries): stadium levels 1-6 at entries 1-6, turf 1-3 at 8-10, seats 1-4 at
        // 12-15, facility type t at t + 16; +0x14 the year (club +0x88) from which the game offers it (sub_0028F668: the
        // dome from year 10, 託児所 / ファンクラブ / ギャラリー 2, コンビニ 3, カラオケ 4), +0x18 the squares, +0x1C the cost.
        // Checked in the game's own 球場施設 screen (2026-11-06): rows 1F-4F on both sides, the 8th row (4 squares) is
        // 4F 外野側, opened with the dome.
        constexpr uint32_t kClubPointer = 0x5CA114u, kMasterPointer = 0x5D10E8u, kMasterEntry = 0x30u;
        constexpr uint32_t kStadium = 0x13A0u, kDomeFlag = 0x1274u, kCounts = 0x13A9u, kPractice = 0x13D8u;
        constexpr uint32_t kSlotItems = 0x1420u, kSquares = 0x1438u, kClubSize = 0x1460u;
        constexpr uint32_t kTeamTable = 0x5D1240u, kTeamRoster = 0x120u, kRosterClass = 0x4u, kOwnTeamClass = 0x6659F0u;
        constexpr uint32_t kDate = 0x62F190u, kDateYear = 0x13Du; // year 255: the title's demo game
        constexpr uint32_t kClubYear = 0x88u;                       // the year the game's offers go by (1 = 1年目)
        // Secretary and stadium satisfaction (2026-11-08, from the 5ch 改造コード threads: 6回裏 #671 secretary 0086695C,
        // #751 stadium satisfaction 008668F7, i.e. club +0x141C / +0x13B7 with the PCSX2 club at 0x865540, the same base
        // that puts the money +0xE50 at the PAR money code's 0x866390). The club's new-game set-up sub_0011A498 writes
        // these offsets: +0x141C = 10, +0x13B6 = 90, +0x13B7 = 80.
        //   +0x141C secretary: 0-3 index the name table 0x5CD960 (伊東 奈緒, 早坂 瑞樹, 宮本 亜由美, ナンシー・ハリスン,
        //     sub_00130A28); anything else is "ナナシ" (no secretary yet; the new game's 10).
        //   +0x13B7 stadium satisfaction (the 5ch poster: "the top is 80 in decimal?"; a new game starts at 80).
        constexpr uint32_t kSecretary = 0x141Cu, kSatisfaction = 0x13B7u, kSatisfactionPair = 0x13B6u;
        constexpr int kSecretaries = 4, kNoSecretary = 10, kSatisfactionMax = 100;
        constexpr const char *kSecretaryNames[kSecretaries] = {"이토 나오 (伊東 奈緒)", "하야사카 미즈키 (早坂 瑞樹)", "미야모토 아유미 (宮本 亜由美)",
                                                               "낸시 해리슨 (ナンシー・ハリスン)"};
        constexpr uint32_t kRamSize = 0x2000000u;
        constexpr int kTypes = 13, kSlotCount = 24, kRows = 8, kPracticeCount = 11, kExtraMax = 99;
        constexpr int kRowSize[kRows] = {4, 3, 3, 3, 3, 2, 2, 4};
        constexpr int kRowFirst[kRows] = {0, 4, 7, 10, 13, 16, 18, 20};
        constexpr int kTop[3] = {6, 3, 4}; // stadium, turf, seats
        constexpr int kPracticeTop = 3;
        constexpr const char *kRowNames[kRows] = {"1층 내야", "1층 외야", "2층 내야", "2층 외야", "3층 내야", "3층 외야", "4층 내야", "4층 외야"};
        constexpr const char *kTypeNames[kTypes] = {"화장실", "매점", "레스토랑", "굿즈숍",   "탁아소", "흡연실",  "안내 센터",
                                                    "ATM",    "팬클럽 카운터", "편의점", "갤러리", "노래방", "공중전화"};
        constexpr const char *kTypeShort[kTypes] = {"화장실", "매점", "레스토랑", "굿즈", "탁아소", "흡연", "안내", "ATM", "팬클럽", "편의점", "갤러리", "노래방", "전화"};
        constexpr const char *kPartNames[3] = {"구장", "잔디", "관중석"};
        constexpr const char *kStadiumNames[7] = {"1층 내외야석",   "2층 내야석 증축", "2층 외야석 증축", "3층 내야석 증축",
                                                  "3층 외야석 증축", "4층 내야석 증축", "돔 구장"};
        constexpr const char *kTurfNames[4] = {"내야 흙·외야 인조잔디", "내외야 인조잔디", "내야 흙·외야 천연잔디", "내외야 천연잔디"};
        constexpr const char *kSeatNames[5] = {"보통 의자", "등받이 의자", "접이식 의자", "히터 의자", "리클라이닝 시트"};
        constexpr const char *kPracticeNames[kPracticeCount] = {"메인 연습장", "불펜",   "다목적 그라운드", "실내 연습장", "수영장", "피지컬 트레이닝룸",
                                                                "의무실",      "연구실", "식당",            "대욕장",      "릴랙세이션룸"};

        const char *partLevelName(int part, int level)
        {
            if (part == 0)
                return level >= 0 && level < 7 ? kStadiumNames[level] : "?";
            if (part == 1)
                return level >= 0 && level < 4 ? kTurfNames[level] : "?";
            return level >= 0 && level < 5 ? kSeatNames[level] : "?";
        }

        uint8_t *g_rdram = nullptr;
        bool g_safeMode = false;

        // What the menu shows: a copy taken by the frame hook.
        struct View
        {
            bool active = false;
            std::array<uint8_t, 9> stadium{}; // [level][weeks][state] x 3
            uint8_t dome = 0;
            std::array<uint8_t, kTypes> counts{};
            std::array<uint8_t, kSlotCount> items{};
            std::array<uint8_t, kRows * 4> squares{};
            std::array<uint8_t, kPracticeCount * 3> practice{};
            std::array<int, kTypes> size{}, needYear{};
            int year = 0;
            int secretary = -1, satisfaction = -1;
            int placed(int type) const
            {
                return static_cast<int>(std::count(items.begin(), items.end(), static_cast<uint8_t>(type + 1)));
            }
        };
        enum class Kind
        {
            MaxStadium,
            MaxPractice,
            Place,
            Fill,
            Remove,
            ClearAll,
            Extra,
            Secretary,    // type = 0-3, or kNoSecretary
            Satisfaction, // extra = the value
        };
        struct Action
        {
            Kind kind = Kind::MaxStadium;
            int type = 0, row = 0, col = 0, extra = 0;
        };
        std::mutex g_mutex; // the view, the actions and the log: the menu (GS thread) and the frame hook (EE thread)
        View g_view;
        std::vector<Action> g_actions;
        urban::ChangeLog g_log;
        std::atomic<bool> g_refreshNow{false};

        // The satisfaction lock, per save (cheat_saves/<save>/stadium_cheats.ini, satisfaction=<value>, -1 off): the frame
        // hook writes it while the own club exists. g_pathMutex: the frame hook (EE) and the menu.
        std::atomic<int> g_satisfactionLock{-1};
        std::mutex g_pathMutex;
        std::filesystem::path g_settingsPath;
        std::string g_profile;
        uint32_t g_generation = ~0u;

        int loadLock(const std::filesystem::path &path)
        {
            std::ifstream file(path, std::ios::binary);
            std::string line;
            int lock = -1;
            while (std::getline(file, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.rfind("satisfaction=", 0) == 0)
                    lock = std::clamp(std::atoi(line.c_str() + 13), -1, kSatisfactionMax);
            }
            return lock;
        }

        void saveLock(int lock)
        {
            std::filesystem::path path;
            {
                std::lock_guard<std::mutex> guard(g_pathMutex);
                path = g_settingsPath;
            }
            if (path.empty())
                return; // before any game: nothing to keep it for
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file << "# 구장·시설 설정: 게임 메뉴(F2)의 \"구장·시설\" 탭이 씁니다 (satisfaction=-1 고정 안 함)\n"
                 << "satisfaction=" << lock << "\n";
        }

        uint32_t read32(uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, g_rdram + address, sizeof(value));
            return value;
        }

        bool valid(uint32_t address, uint32_t size)
        {
            return (address & 3u) == 0u && address >= 0x100000u && address < kRamSize && size <= kRamSize - address;
        }

        // The own club once a game runs: a team of the player's own exists and it is not the title's demo.
        uint32_t club()
        {
            const uint32_t date = read32(kDate);
            if (!valid(date, kDateYear + 1u) || g_rdram[date + kDateYear] == 0xFFu)
                return 0u;
            bool own = false;
            for (int t = 0; t < 12 && !own; ++t)
            {
                const uint32_t team = read32(kTeamTable + 4u * static_cast<uint32_t>(t));
                if (!valid(team, kTeamRoster + 4u))
                    continue;
                const uint32_t roster = read32(team + kTeamRoster);
                own = valid(roster, kRosterClass + 4u) && read32(roster + kRosterClass) == kOwnTeamClass;
            }
            const uint32_t object = own ? read32(kClubPointer) : 0u;
            return valid(object, kClubSize) ? object : 0u;
        }

        uint8_t &at(uint32_t c, uint32_t offset)
        {
            return g_rdram[c + offset];
        }

        int openRows(uint32_t c)
        {
            return std::clamp(static_cast<int>(at(c, kStadium)) + 2, 2, kRows);
        }

        // The master data's squares and opening year for a facility type (2 and 0 when it cannot be read).
        void typeData(int type, int &size, int &needYear)
        {
            size = 2;
            needYear = 0;
            const uint32_t master = read32(kMasterPointer);
            const uint32_t entry = master + static_cast<uint32_t>(type + 16) * kMasterEntry;
            if (!valid(master, 29u * kMasterEntry))
                return;
            size = std::clamp<int>(g_rdram[entry + 0x18u], 1, 4);
            needYear = g_rdram[entry + 0x14u];
        }

        // The first run of free squares in the row long enough for the facility: its column, or -1.
        int freeRun(uint32_t c, int row, int size)
        {
            int run = 0;
            for (int col = 0; col < kRowSize[row]; ++col)
            {
                if (at(c, kSquares + 4u * row + col) == 0xFFu)
                {
                    if (++run == size)
                        return col - size + 1;
                }
                else
                    run = 0;
            }
            return -1;
        }

        // As the game's own placement (sub_0028FA50), without its cost: the slot holds the facility, its squares point at
        // the slot, its type's count goes up by one.
        bool place(uint32_t c, int type, int row)
        {
            int size = 2, needYear = 0;
            typeData(type, size, needYear);
            if (size > kRowSize[row])
                return false;
            const int col = freeRun(c, row, size);
            if (col < 0)
                return false;
            const int slot = kRowFirst[row] + col;
            if (at(c, kSlotItems + slot) != 0u)
                return false;
            at(c, kSlotItems + slot) = static_cast<uint8_t>(type + 1);
            for (int k = 0; k < size; ++k)
                at(c, kSquares + 4u * row + col + k) = static_cast<uint8_t>(slot);
            uint8_t &count = at(c, kCounts + type);
            count = static_cast<uint8_t>(std::min(count + 1, 255));
            return true;
        }

        // As the game's own removal (sub_0028FB70): the facility on the square leaves its slot and squares, its type's
        // count goes down by one. Returns its type, or -1.
        int remove(uint32_t c, int row, int col)
        {
            const uint8_t slot = at(c, kSquares + 4u * row + col);
            if (slot >= kSlotCount)
                return -1;
            const uint8_t item = at(c, kSlotItems + slot);
            if (item == 0u || item > kTypes)
                return -1;
            const int type = item - 1;
            uint8_t &count = at(c, kCounts + type);
            if (count > 0u)
                --count;
            at(c, kSlotItems + slot) = 0u;
            for (int k = 0; k < 4; ++k)
                if (at(c, kSquares + 4u * row + k) == slot)
                    at(c, kSquares + 4u * row + k) = 0xFFu;
            return type;
        }

        int placedNow(uint32_t c, int type)
        {
            int placed = 0;
            for (int slot = 0; slot < kSlotCount; ++slot)
                placed += at(c, kSlotItems + slot) == type + 1 ? 1 : 0;
            return placed;
        }

        std::string squaresText(int row, int col, int size)
        {
            return std::string(kRowNames[row]) + " " + std::to_string(col + 1) + (size > 1 ? "~" + std::to_string(col + size) : "") + "칸";
        }

        // EE thread (g_mutex held): one action, with what memory says afterwards in the change log.
        void apply(uint32_t c, const Action &action)
        {
            switch (action.kind)
            {
            case Kind::MaxStadium:
            {
                std::string done;
                for (int part = 0; part < 3; ++part)
                {
                    const uint32_t base = kStadium + 3u * static_cast<uint32_t>(part);
                    // A level below the top, or an upgrade under way: done now (no weeks left, state 2 as the game's own
                    // completion leaves it; under way at the top would raise it past the top).
                    if (at(c, base) != kTop[part] || at(c, base + 1u) != 0u || at(c, base + 2u) == 1u)
                    {
                        at(c, base) = static_cast<uint8_t>(kTop[part]);
                        at(c, base + 1u) = 0u;
                        at(c, base + 2u) = 2u;
                    }
                    done += std::string(done.empty() ? "" : " · ") + kPartNames[part] + " " + partLevelName(part, at(c, base)) + " (" +
                            std::to_string(at(c, base)) + ")";
                }
                at(c, kDomeFlag) = 1u; // the game's own step at stadium level 6
                g_log.add("구장 최고로: " + done + " · 돔 표시 " + std::to_string(at(c, kDomeFlag)) + " — 메모리에서 다시 읽은 값", "stadium");
                break;
            }
            case Kind::MaxPractice:
            {
                int top = 0;
                for (int i = 0; i < kPracticeCount; ++i)
                {
                    const uint32_t base = kPractice + 3u * static_cast<uint32_t>(i);
                    if (at(c, base) != kPracticeTop || at(c, base + 1u) != 0u || at(c, base + 2u) == 1u)
                    {
                        at(c, base) = static_cast<uint8_t>(kPracticeTop);
                        at(c, base + 1u) = 0u;
                        at(c, base + 2u) = 2u;
                    }
                    top += at(c, base) == kPracticeTop ? 1 : 0;
                }
                g_log.add("연습 시설 최고로: " + std::to_string(top) + "/11곳 레벨 3 — 메모리에서 다시 읽은 값", "stadium");
                break;
            }
            case Kind::Place:
            case Kind::Fill:
            {
                if (action.type < 0 || action.type >= kTypes)
                    break;
                int placed = 0;
                std::string where;
                for (int row = 0; row < openRows(c); ++row)
                {
                    if (action.kind == Kind::Place && row != action.row)
                        continue;
                    while (true)
                    {
                        int size = 2, needYear = 0;
                        typeData(action.type, size, needYear);
                        const int col = size <= kRowSize[row] ? freeRun(c, row, size) : -1;
                        if (col < 0 || !place(c, action.type, row))
                            break;
                        ++placed;
                        if (where.empty())
                            where = squaresText(row, col, size);
                        if (action.kind == Kind::Place)
                            break;
                    }
                }
                const std::string count = " (" + std::string(kTypeNames[action.type]) + " 개수 " + std::to_string(at(c, kCounts + action.type)) + ")";
                if (placed == 0)
                    g_log.add("배치 못함: " + std::string(kTypeNames[action.type]) +
                                  (action.kind == Kind::Place ? " — " + std::string(kRowNames[std::clamp(action.row, 0, kRows - 1)]) + "에 이어진 빈칸이 모자람"
                                                              : " — 열린 줄에 이어진 빈칸이 모자람") +
                                  count,
                              "stadium");
                else if (action.kind == Kind::Place)
                    g_log.add("배치: " + std::string(kTypeNames[action.type]) + " → " + where + count + " — 메모리에서 다시 읽은 값", "stadium");
                else
                    g_log.add("채우기: " + std::string(kTypeNames[action.type]) + " " + std::to_string(placed) + "개 놓음" + count + " — 메모리에서 다시 읽은 값",
                              "stadium");
                break;
            }
            case Kind::Remove:
            {
                if (action.row < 0 || action.row >= kRows || action.col < 0 || action.col >= kRowSize[action.row])
                    break;
                const int type = remove(c, action.row, action.col);
                if (type >= 0)
                    g_log.add("빼기: " + std::string(kTypeNames[type]) + " (" + kRowNames[action.row] + ") · 남은 " + kTypeNames[type] + " 개수 " +
                                  std::to_string(at(c, kCounts + type)) + " — 메모리에서 다시 읽은 값",
                              "stadium");
                break;
            }
            case Kind::ClearAll:
            {
                int removed = 0;
                for (int row = 0; row < kRows; ++row)
                    for (int col = 0; col < kRowSize[row]; ++col)
                        removed += remove(c, row, col) >= 0 ? 1 : 0;
                g_log.add("배치 모두 비움: " + std::to_string(removed) + "개 뺌 — 칸 밖 개수는 그대로", "stadium");
                break;
            }
            case Kind::Extra:
            {
                if (action.type < 0 || action.type >= kTypes)
                    break;
                const int placed = placedNow(c, action.type);
                at(c, kCounts + action.type) = static_cast<uint8_t>(std::clamp(placed + action.extra, 0, placed + kExtraMax));
                g_log.add("칸 밖 개수: " + std::string(kTypeNames[action.type]) + " 배치 " + std::to_string(placed) + " + 칸 밖 " +
                              std::to_string(at(c, kCounts + action.type) - placed) + " = 개수 " + std::to_string(at(c, kCounts + action.type)) +
                              " — 메모리에서 다시 읽은 값",
                          "stadium");
                break;
            }
            case Kind::Secretary:
            {
                if ((action.type < 0 || action.type >= kSecretaries) && action.type != kNoSecretary)
                    break;
                at(c, kSecretary) = static_cast<uint8_t>(action.type);
                const int now = at(c, kSecretary);
                g_log.add("비서: " + std::string(now < kSecretaries ? kSecretaryNames[now] : "없음 (ナナシ)") + " (값 " + std::to_string(now) +
                              ") — 메모리에서 다시 읽은 값",
                          "stadium");
                break;
            }
            case Kind::Satisfaction:
            {
                at(c, kSatisfaction) = static_cast<uint8_t>(std::clamp(action.extra, 0, kSatisfactionMax));
                g_log.add("구장 만족도: " + std::to_string(at(c, kSatisfaction)) + " — 메모리에서 다시 읽은 값", "stadium");
                break;
            }
            }
        }

        // The secretary's name as the game's table orders them, or the game's "no secretary".
        std::string secretaryName(int value)
        {
            return value >= 0 && value < kSecretaries ? kSecretaryNames[value] : "없음 (ナナシ, 값 " + std::to_string(value) + ")";
        }

        // EE thread, at every vblank after the .pnach patches: the menu's actions, and every 15 frames what it shows.
        void frame()
        {
            static unsigned tick = 0u;
            // Another save or a new game: that save's satisfaction lock (none for a new game until it is set).
            if (const uint32_t generation = urban::saveid::generation(); generation != g_generation)
            {
                g_generation = generation;
                const std::string profile = urban::saveid::current();
                const std::filesystem::path path = urban::saveid::settingsFile(profile, "stadium_cheats.ini");
                {
                    std::lock_guard<std::mutex> guard(g_pathMutex);
                    g_settingsPath = path;
                    g_profile = profile;
                }
                std::error_code error;
                g_satisfactionLock.store(!path.empty() && std::filesystem::exists(path, error) ? loadLock(path) : -1);
                std::fprintf(stderr, "[stadium] save %s: satisfaction lock %d\n", profile.empty() ? "(none)" : profile.c_str(),
                             g_satisfactionLock.load());
            }
            const bool manual = g_refreshNow.exchange(false);
            std::lock_guard<std::mutex> lock(g_mutex);
            const uint32_t c = club();
            if (c == 0u)
            {
                g_actions.clear();
                if (g_view.active)
                    g_view = View{};
                return;
            }
            const bool acted = !g_actions.empty();
            for (const Action &action : g_actions)
                apply(c, action);
            g_actions.clear();
            if (const int lock = g_satisfactionLock.load(std::memory_order_relaxed); lock >= 0 && at(c, kSatisfaction) != lock)
                at(c, kSatisfaction) = static_cast<uint8_t>(lock);
            // URBAN_STADIUM_LOG=1: the secretary and the satisfaction pair whenever they change (tests).
            static const bool logFields = std::getenv("URBAN_STADIUM_LOG") != nullptr;
            static int lastFields = -1;
            if (logFields)
            {
                const int fields = at(c, kSecretary) << 16 | at(c, kSatisfactionPair) << 8 | at(c, kSatisfaction);
                if (fields != lastFields)
                {
                    lastFields = fields;
                    const uint32_t date = read32(kDate);
                    std::fprintf(stderr, "[stadium] club %08x secretary %d satisfaction %d (+0x13B6 %d) year %d week %d\n", c, at(c, kSecretary),
                                 at(c, kSatisfaction), at(c, kSatisfactionPair), valid(date, 0x144u) ? g_rdram[date + kDateYear] : -1,
                                 valid(date, 0x144u) ? g_rdram[date + 0x140u] : -1);
                }
            }
            if (!acted && !manual && (++tick % 15u) != 0u && g_view.active)
                return;
            View view;
            view.active = true;
            std::memcpy(view.stadium.data(), g_rdram + c + kStadium, view.stadium.size());
            view.dome = at(c, kDomeFlag);
            std::memcpy(view.counts.data(), g_rdram + c + kCounts, view.counts.size());
            std::memcpy(view.items.data(), g_rdram + c + kSlotItems, view.items.size());
            std::memcpy(view.squares.data(), g_rdram + c + kSquares, view.squares.size());
            std::memcpy(view.practice.data(), g_rdram + c + kPractice, view.practice.size());
            for (int type = 0; type < kTypes; ++type)
                typeData(type, view.size[static_cast<size_t>(type)], view.needYear[static_cast<size_t>(type)]);
            view.year = static_cast<int>(read32(c + kClubYear));
            view.secretary = at(c, kSecretary);
            view.satisfaction = at(c, kSatisfaction);
            g_view = view;
            g_log.read(g_rdram);
        }

#if defined(URBAN_STADIUM_MENU)
        int g_type = 0, g_row = 0;
        int g_secretaryPick = 0, g_satisfactionDraft = 80;
        std::array<int, kTypes> g_extraDraft{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

        // GS thread, while the menu is open: reads the frame hook's copy, queues actions for the next frame.
        void draw(const Pcsx2GsUi *ui)
        {
            if (g_safeMode)
            {
                ui->text_wrapped("안전 모드로 시작해서 구장·시설 치트가 꺼져 있습니다.");
                return;
            }
            if (ui->version < 3u)
            {
                ui->text_wrapped("pcsx2gs.dll이 이 실행 파일보다 오래되었습니다. 다시 빌드하세요.");
                return;
            }
            View view;
            urban::ChangeLog log;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                view = g_view;
                log = g_log;
            }
            if (urban::drawRefresh(ui, log, "구장·시설"))
                g_refreshNow = true;
            if (urban::drawChanges(ui, log, "stadiumchanges", 120.0f))
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_log.lines.clear();
            }
            if (!view.active)
            {
                ui->text_wrapped("세이브를 불러오거나 새 게임을 시작하면 내 구단의 구장과 시설이 여기에 나옵니다.");
                return;
            }
            std::vector<Action> queue;
            ui->text_colored(0xFF50C8FFu, "경영 > 球場施設 / 練習施設 화면 밖(사무실 메뉴 등)에서 적용하세요. 그 화면을 연 채로 바꾸면 화면을 닫을 때 게임이 화면의 배치로 되돌립니다.");

            ui->separator_text("구장 · 잔디 · 관중석");
            for (int part = 0; part < 3; ++part)
            {
                const int level = view.stadium[static_cast<size_t>(3 * part)], weeks = view.stadium[static_cast<size_t>(3 * part + 1)];
                const bool building = view.stadium[static_cast<size_t>(3 * part + 2)] == 1u && weeks > 0;
                ui->text((std::string(kPartNames[part]) + ": " + partLevelName(part, level) + " (" + std::to_string(level) + "/" + std::to_string(kTop[part]) +
                          ")" + (building ? " · 공사 중 " + std::to_string(weeks) + "주 남음" : ""))
                             .c_str());
            }
            if (ui->button("구장·잔디·관중석 최고로"))
                queue.push_back(Action{Kind::MaxStadium});
            ui->tooltip("구장 6 (돔 구장, 게임이 돔 완공 때 켜는 표시도 켬) · 잔디 3 (내외야 천연잔디) · 관중석 4 (리클라이닝 시트). "
                        "공사 중이던 것은 바로 끝냅니다. 비용 없음. 게임은 원래 연차로 막습니다 (돔은 10년차부터). "
                        "구장 등급이 오르면 시설 칸 줄이 열립니다 (돔: 8줄 24칸).");

            ui->separator_text("비서 · 구장 만족도");
            ui->text(("비서: " + secretaryName(view.secretary)).c_str());
            ui->set_next_width(240.0f);
            ui->combo("##secretary", &g_secretaryPick, kSecretaryNames, kSecretaries);
            ui->same_line();
            if (ui->button("비서 바꾸기"))
                queue.push_back(Action{Kind::Secretary, g_secretaryPick});
            ui->tooltip("구단 +0x141C (5ch 改造コード 6回裏 #671, his씨 블로그의 秘書変更와 같은 칸). 0~3 = 게임의 비서 4명, "
                        "새 게임은 10 (ナナシ = 아직 비서 없음). 바로 바뀝니다.");
            ui->same_line();
            if (ui->small_button("비서 없음으로"))
                queue.push_back(Action{Kind::Secretary, kNoSecretary});
            {
                std::string profile;
                {
                    std::lock_guard<std::mutex> guard(g_pathMutex);
                    profile = g_profile;
                }
                const int lock = g_satisfactionLock.load();
                ui->text(("구장 만족도: " + std::to_string(view.satisfaction) + (lock >= 0 ? "  (고정 " + std::to_string(lock) + ")" : "")).c_str());
                ui->set_next_width(110.0f);
                if (ui->input_int("##satisfaction", &g_satisfactionDraft, 1))
                {
                    g_satisfactionDraft = std::clamp(g_satisfactionDraft, 0, kSatisfactionMax);
                    if (lock >= 0) // locked: the lock follows the new value
                    {
                        g_satisfactionLock.store(g_satisfactionDraft);
                        saveLock(g_satisfactionDraft);
                    }
                }
                ui->same_line();
                if (ui->button("1회 적용##satisfaction"))
                    queue.push_back(Action{Kind::Satisfaction, 0, 0, 0, g_satisfactionDraft});
                ui->same_line();
                int locked = lock >= 0 ? 1 : 0;
                if (ui->checkbox("이 값으로 고정", &locked))
                {
                    const int now = locked ? g_satisfactionDraft : -1;
                    g_satisfactionLock.store(now);
                    saveLock(now);
                }
                ui->tooltip("구단 +0x13B7 (5ch 6回裏 #751: '최대는 10진 80?'). 새 게임 시작값이 80입니다. 고정은 이 세이브에만 저장되고, "
                            "매 프레임 이 값으로 다시 씁니다 (게임이 주마다 바꿔도 유지).");
                ui->text_disabled(("세이브: " + urban::saveid::describe(profile) + " — 만족도 고정은 이 세이브의 설정").c_str());
            }

            ui->separator_text("연습 시설");
            int top = 0;
            std::string line;
            for (int i = 0; i < kPracticeCount; ++i)
            {
                const int level = view.practice[static_cast<size_t>(3 * i)], weeks = view.practice[static_cast<size_t>(3 * i + 1)];
                const bool building = view.practice[static_cast<size_t>(3 * i + 2)] == 1u && weeks > 0;
                top += level >= kPracticeTop ? 1 : 0;
                line += std::string(line.empty() ? "" : " · ") + kPracticeNames[i] + " " + std::to_string(level) + (building ? "(공사 중)" : "");
                if (i % 4 == 3 || i == kPracticeCount - 1)
                {
                    ui->text(line.c_str());
                    line.clear();
                }
            }
            if (ui->button(("연습 시설 모두 최고 레벨(3)로  (지금 " + std::to_string(top) + "/11곳)").c_str()))
                queue.push_back(Action{Kind::MaxPractice});
            ui->tooltip("11곳 모두 레벨 3. 공사 중이던 것은 바로 끝냅니다 (그대로 두면 완공 때 3을 넘음). 비용 없음.");

            const int level = view.stadium[0];
            const int open = std::clamp(level + 2, 2, kRows);
            int openSquares = 0, freeSquares = 0;
            for (int row = 0; row < open; ++row)
                for (int col = 0; col < kRowSize[row]; ++col)
                {
                    ++openSquares;
                    freeSquares += view.squares[static_cast<size_t>(4 * row + col)] == 0xFFu ? 1 : 0;
                }
            ui->separator_text(("구장 시설 배치 (열린 칸 " + std::to_string(openSquares) + "/24 · 빈칸 " + std::to_string(freeSquares) + ")").c_str());
            ui->text_disabled("칸을 누르면 그 시설을 뺍니다. 놓을 때 게임의 시설 종류 제한(연차)과 비용은 무시합니다.");
            for (int row = 0; row < kRows; ++row)
            {
                ui->text(kRowNames[row]);
                ui->same_line_at(80.0f);
                if (row >= open)
                {
                    ui->text_disabled(("잠김 — 구장 " + std::string(kStadiumNames[std::clamp(row - 1, 0, 6)]) + " (등급 " + std::to_string(row - 1) +
                                       ")부터")
                                          .c_str());
                    continue;
                }
                for (int col = 0; col < kRowSize[row]; ++col)
                {
                    const uint8_t slot = view.squares[static_cast<size_t>(4 * row + col)];
                    const uint8_t item = slot < kSlotCount ? view.items[slot] : 0u;
                    const std::string label = (item >= 1u && item <= kTypes ? std::string(kTypeShort[item - 1]) : std::string("빈칸")) + "##sq" +
                                              std::to_string(4 * row + col);
                    if (col > 0)
                        ui->same_line();
                    if (ui->button_colored(label.c_str(), item != 0u ? 0xFF6E9628u : 0u, 76.0f) && item != 0u)
                        queue.push_back(Action{Kind::Remove, 0, row, col});
                    if (item != 0u)
                        ui->tooltip((std::string(kTypeNames[item - 1]) + " — 누르면 뺍니다").c_str());
                }
            }
            const char *types[kTypes];
            std::array<std::string, kTypes> typeLabels;
            for (int type = 0; type < kTypes; ++type)
            {
                typeLabels[static_cast<size_t>(type)] = std::string(kTypeNames[type]) + " (" + std::to_string(view.size[static_cast<size_t>(type)]) + "칸" +
                                                        (view.needYear[static_cast<size_t>(type)] > view.year
                                                             ? ", 게임에서는 " + std::to_string(view.needYear[static_cast<size_t>(type)]) + "년차부터"
                                                             : "") +
                                                        ")";
                types[type] = typeLabels[static_cast<size_t>(type)].c_str();
            }
            ui->set_next_width(260.0f);
            ui->combo("##stype", &g_type, types, kTypes);
            ui->same_line();
            g_row = std::clamp(g_row, 0, open - 1);
            ui->set_next_width(110.0f);
            ui->combo("##srow", &g_row, kRowNames, open);
            ui->same_line();
            if (ui->button("이 줄 빈칸에 놓기"))
                queue.push_back(Action{Kind::Place, g_type, g_row});
            ui->tooltip("고른 줄에서 이어진 빈칸이 시설 칸 수만큼 있는 첫 자리에 놓습니다. 4칸 시설은 4칸 줄(1층 내야, 4층 외야)에만 들어갑니다.");
            if (ui->button("열린 빈칸 모두 이 시설로 채우기"))
                queue.push_back(Action{Kind::Fill, g_type});
            ui->same_line();
            if (ui->button("배치 모두 비우기"))
                queue.push_back(Action{Kind::ClearAll});
            ui->tooltip("놓인 시설을 모두 뺍니다 (종류별 개수도 그만큼 줄어듦). 칸 밖 개수는 그대로.");

            ui->separator_text("칸 없이 개수만 더하기 (효과만)");
            ui->text_disabled("게임은 시설 효과를 종류별 개수 × 효과로 더합니다. 여기서 더한 개수는 효과(와 유지비)만 있고 배치도에는 안 나옵니다.");
            if (ui->begin_table("sextra", 4, 0u, 0.0f, 0.0f))
            {
                ui->table_column("시설", 0u, 0.0f);
                ui->table_column("배치", 0u, 50.0f);
                ui->table_column("칸 밖", 0u, 80.0f);
                ui->table_column("", 0u, 60.0f);
                ui->table_headers();
                for (int type = 0; type < kTypes; ++type)
                {
                    const int placed = view.placed(type), extra = std::max(0, static_cast<int>(view.counts[static_cast<size_t>(type)]) - placed);
                    int &draft = g_extraDraft[static_cast<size_t>(type)];
                    if (draft < 0)
                        draft = extra;
                    ui->table_next_row();
                    ui->table_next_column();
                    ui->text(kTypeNames[type]);
                    ui->table_next_column();
                    ui->text(std::to_string(placed).c_str());
                    ui->table_next_column();
                    ui->push_id(type);
                    ui->set_next_width(70.0f);
                    if (ui->input_int("##extra", &draft, 1))
                        draft = std::clamp(draft, 0, kExtraMax);
                    ui->table_next_column();
                    ui->begin_disabled(draft == extra ? 1 : 0);
                    if (ui->small_button("적용"))
                    {
                        queue.push_back(Action{Kind::Extra, type, 0, 0, draft});
                        draft = -1;
                    }
                    ui->end_disabled();
                    ui->pop_id();
                }
                ui->end_table();
            }
            if (!queue.empty())
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_actions.insert(g_actions.end(), queue.begin(), queue.end());
            }
        }
#endif
    }

    void install(uint8_t *rdram)
    {
        if (!rdram)
            return;
        g_rdram = rdram;
        const char *mods = std::getenv("URBAN_MODS");
        g_safeMode = mods && std::strcmp(mods, ";") == 0; // the launcher's safe start
        std::fprintf(stderr, "[stadium] cheats %s\n", g_safeMode ? "off (safe start)" : "ready");
#if defined(URBAN_STADIUM_MENU)
        ps2x::menu::addPanel("구장·시설", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
        if (g_safeMode)
            return;
        ps2x::pnach::addFrameHook(frame);
        // Tests: URBAN_TEST_STADIUM="max;practice;fill:<type>;place:<type>:<row>;extra:<type>:<n>;clear;sec:<0-3|10>;sat:<n>;
        // satlock:<n|-1>" once a game runs.
        if (const char *test = std::getenv("URBAN_TEST_STADIUM"); test && *test)
        {
            static std::string script = test;
            ps2x::pnach::addFrameHook([] {
                static bool queued = false;
                if (queued || club() == 0u)
                    return;
                {
                    std::lock_guard<std::mutex> guard(g_pathMutex);
                    if (g_settingsPath.empty())
                        return; // the save is not known yet: satlock would have nowhere to go
                }
                queued = true;
                std::lock_guard<std::mutex> lock(g_mutex);
                size_t start = 0;
                while (start <= script.size())
                {
                    const size_t end = script.find(';', start);
                    const std::string item = script.substr(start, end == std::string::npos ? std::string::npos : end - start);
                    int a = 0, b = 0;
                    if (item == "max")
                        g_actions.push_back(Action{Kind::MaxStadium});
                    else if (item == "practice")
                        g_actions.push_back(Action{Kind::MaxPractice});
                    else if (item == "clear")
                        g_actions.push_back(Action{Kind::ClearAll});
                    else if (std::sscanf(item.c_str(), "fill:%d", &a) == 1)
                        g_actions.push_back(Action{Kind::Fill, a});
                    else if (std::sscanf(item.c_str(), "place:%d:%d", &a, &b) == 2)
                        g_actions.push_back(Action{Kind::Place, a, b});
                    else if (std::sscanf(item.c_str(), "extra:%d:%d", &a, &b) == 2)
                        g_actions.push_back(Action{Kind::Extra, a, 0, 0, b});
                    else if (std::sscanf(item.c_str(), "sec:%d", &a) == 1)
                        g_actions.push_back(Action{Kind::Secretary, a});
                    else if (std::sscanf(item.c_str(), "satlock:%d", &a) == 1)
                    {
                        g_satisfactionLock.store(std::clamp(a, -1, kSatisfactionMax));
                        saveLock(g_satisfactionLock.load());
                    }
                    else if (std::sscanf(item.c_str(), "sat:%d", &a) == 1)
                        g_actions.push_back(Action{Kind::Satisfaction, 0, 0, 0, a});
                    if (end == std::string::npos)
                        break;
                    start = end + 1;
                }
                std::fprintf(stderr, "[stadium] test: %zu actions\n", g_actions.size());
            });
        }
    }
}
