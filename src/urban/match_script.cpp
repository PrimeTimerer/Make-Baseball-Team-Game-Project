#include "urban/match_script.h"
#include "urban/function_hooks.h"
#include "urban/ichinen_cheats.h"
#include "urban/save_identity.h"

#include "ps2_runtime.h"
#include "runtime/ps2_game_menu.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define URBAN_SCRIPT_MENU 1
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace urban::matchscript
{
    namespace
    {
        constexpr uint32_t kRealgamePtr = 0x614E88u; // realgame object ("rg"), 0x17F0 bytes
        constexpr uint32_t kMatchPtr = 0x5E783Cu;    // simgame match: +0x18 batting team, +0xD8 strikes, +0xD9 balls
        constexpr uint32_t kTeamObj0 = 0x5D1278u;    // match team objects: +0x2008 + slot*4 = player's match copy
        constexpr uint32_t kTeamObj1 = 0x5D1274u;
        constexpr uint32_t kTeamTable = 0x5D1240u;   // the 12 clubs: +0x120 roster (+4 class, +8+4k records)
        constexpr uint32_t kOwnRosterClass = 0x6659F0u;
        constexpr int kTeams = 12;

        // The guest functions hooked. The runtime may pause a guest call and resume it later from its scheduler, so a
        // hook only changes registers and memory first and then hands over to the original (nothing runs after it).
        constexpr uint32_t kSwing = 0x3299E8u;      // in sub_329F78 just before the swing decision (sign-forced swing)
        constexpr uint32_t kNoContact = 0x3298D8u;  // strike / ball / strikeout / walk of a pitch not in play
        constexpr uint32_t kPlaySetup = 0x32CF38u;  // the 3D play of a ball in play (called by sub_32BB30)
        constexpr uint32_t kStealResult = 0x32DF18u; // a steal's result (1 = safe), read when the pitch's result is set

        // The sign fields of rg that sub_329F78 obeys while +0x124 (the sign) is set: +0x128 = 1 swings, +0x12C = 1 makes
        // the swing a fair contact (else a miss), and with sign 4 +0x134 != 1 makes the ball in play a hit.
        constexpr uint32_t kSign = 0x124u, kSignSwing = 0x128u, kSignContact = 0x12Cu, kSignOut = 0x134u;

        enum class Action
        {
            None,
            HomeRun,
            Hit,
            Strikeout,
            Walk,
        };
        constexpr const char *kActionKeys[] = {"", "hr", "hit", "k", "bb"};

        // When a rule or a stack works (owner 11-07: "경기의 상황과 스코어에 따라서"): the match situation as the subject's
        // club sees it (a batter rule or batter stat: the batting club; a pitcher rule or pitcher stat: the fielding club).
        // From the simgame match [0x5E783C]: +0x14 inning (0-based), +0x18 the batting club (0 top, 1 bottom), +0xD0
        // outs, +0x5874 the runners (3 bytes apart: 1B, 2B, 3B; 0xFF empty), +0x5880 + 4 x club the runs. Checked in hidden
        // runs from a mid-match state: forced HRs moved +0x5880 0 -> 4 -> 5 (scoreboard 4-0) and emptied the bases,
        // forced strikeouts moved +0xD0 1 -> 2 (the HUD's O dots).
        struct Condition
        {
            int innFrom = 0, innTo = 0;   // innings from 1 (0: from the first / to the last, extra innings too)
            int score = 0;                // 0 any, 1 ahead, 2 behind, 3 tied, 4 ahead or tied, 5 behind or tied
            int diffMin = 0, diffMax = 0; // the run difference, either way (0: no bound)
            int runners = 0;              // 0 any, 1 none on, 2 someone on, 3 scoring position (2B or 3B), 4 loaded
            int outs = 0;                 // 0 any, else 1 + the outs (0-2)
            bool any() const { return innFrom || innTo || score || diffMin || diffMax || runners || outs; }
            bool operator==(const Condition &) const = default;
        };

        struct Rule
        {
            int target = 0; // >= 0: global player ID; -1 own team, -2 opponents, -3 everyone
            bool pitcher = false;
            Action action = Action::None;
            Condition when;
            bool operator==(const Rule &other) const
            {
                return target == other.target && pitcher == other.pitcher && action == other.action && when == other.when;
            }
        };

        // Stacking: an ability's match value times a multiplier, past the 99 the match copy holds.
        // Batter: the at-bat fields of rg (sub_328A50: 巧打 vs the pitcher's hand, 長打, バント, 選球眼, 走力, 走塁), which
        // the rolls of the same pitch read. Pitcher: the pitch object [gp-0x7FF0] (sub_2B0748: 球速 +0x60, 球威 +0x64,
        // 制球 +0x68, breaking ball levels +0x14+8k), which the next pitch's choice reads.
        struct Stat
        {
            const char *key;
            const char *label;
            bool pitcher;
            uint32_t offset; // rg (batter) or pitch object (pitcher); 0x14 = the 10 breaking ball levels
        };
        constexpr Stat kStats[] = {
            {"contact", "교타 (巧打)", false, 0xACu}, {"power", "장타 (長打)", false, 0xB0u}, {"speed", "주력 (走力)", false, 0xC0u},
            {"running", "주루 (走塁)", false, 0xC4u}, {"eye", "선구안 (選球眼)", false, 0xB8u}, {"bunt", "번트 (バント)", false, 0xB4u},
            {"velocity", "구속 (球速)", true, 0x60u}, {"stuff", "구위 (球威)", true, 0x64u}, {"control", "제구 (制球)", true, 0x68u},
            {"breaking", "변화구 (変化球)", true, 0x14u},
            // Swing-and-miss and hits are rolled from the batter's 選球眼 and 巧打 (sub_329F78), not from the pitcher's
            // speed or stuff: this one divides the facing batter's 巧打 and 選球眼 while the pitcher pitches.
            {"dominance", "탈삼진력 (상대 타자 교타·선구안 ÷)", true, 0u},
            // Hits mostly land in the shallow outfield, where no HR is rolled: this one moves them deeper.
            {"distance", "타구 비거리 (얕은 외야 안타 → 깊은 외야)", false, 1u},
        };
        constexpr uint32_t kDominance = 0u, kDistance = 1u; // the offsets of the two items above
        constexpr int kStatCount = static_cast<int>(sizeof(kStats) / sizeof(kStats[0]));

        struct Boost
        {
            int target = 0; // as Rule::target
            int stat = 0;   // index into kStats
            int percent = 100; // the multiplier, in percent
            int add = 0;       // added after the multiplier (any value; the result is kept within 0..9999)
            Condition when;
        };

        // Steals and BSO with conditions, several of each (owner, 11-08: "도루나 여타 다른것들도", like the rules): the
        // first entry of the side whose condition holds wins, entries with a condition before those without. Side 0 =
        // the own club bats (its runners steal), 1 = the opponents bat in the own club's games.
        struct StealRule
        {
            int side = 0;
            bool safe = true; // else out (sub_32DF88: a result other than 1 puts the running runner out)
            Condition when;
            bool operator==(const StealRule &) const = default;
        };
        struct BsoRule
        {
            int side = 0;
            int count[3] = {-1, -1, -1}; // balls, strikes, outs (-1: as the game has it)
            Condition when;
        };

        struct Settings
        {
            bool enabled = false;
            bool steal = false; // the own team's steals are always safe
            std::vector<StealRule> steals;
            std::vector<BsoRule> bsoRules; // with conditions; the unconditional holds are bso below
            int limit = 0;      // forced HR / hit / walk per half-inning, 0 = no limit (an all-HR team never makes an out)
            std::vector<Rule> rules;
            std::vector<Boost> boosts;
            // BSO: [0] while the own club bats, [1] while the opponents bat in the own club's games; each {balls, strikes,
            // outs}, -1 = as the game has it. bsoCap: holding outs at 0 or 1 stops once the batting club has this many runs
            // in the half-inning (0 = no limit).
            int bso[2][3] = {{-1, -1, -1}, {-1, -1, -1}};
            int bsoCap = 20;
        };

        uint8_t *g_rdram = nullptr;
        std::filesystem::path g_path;
        std::filesystem::file_time_type g_stamp{};
        bool g_haveStamp = false;
        Settings g_settings; // game thread (the hooks); the menu keeps its own copy and writes the file

        GuestFunction g_swing = nullptr, g_noContact = nullptr, g_playSetup = nullptr, g_stealResult = nullptr;

        // The pitch being decided (all hooks run inside one sub_329F78 call on the game thread).
        Action g_action = Action::None;
        uint16_t g_batter = 0, g_pitcher = 0;
        uint32_t g_rg = 0;
        bool g_injected = false;
        uint32_t g_saved[4] = {};

        // The half-inning (match +0x14 inning, +0x18 top/bottom) and the forced HR / hit / walk results in it.
        uint32_t g_half = ~0u;
        int g_forcedInHalf = 0;

        // Stacking: the values the game set (base) and the ones written over them, per object; a value that differs
        // from what was written means the game set it again (a new at-bat or pitcher), so it becomes the new base.
        struct Stacked
        {
            uint32_t object = 0;
            std::vector<std::pair<uint32_t, uint32_t>> base;    // (address, value the game set)
            std::vector<std::pair<uint32_t, uint32_t>> written; // (address, value written)
        };
        Stacked g_batterStack, g_pitcherStack;

        uint32_t r32(uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, g_rdram + (address & 0x01FFFFFFu), 4);
            return value;
        }
        uint16_t r16(uint32_t address)
        {
            uint16_t value;
            std::memcpy(&value, g_rdram + (address & 0x01FFFFFFu), 2);
            return value;
        }
        uint8_t r8(uint32_t address) { return g_rdram[address & 0x01FFFFFFu]; }
        void w32(uint32_t address, uint32_t value) { std::memcpy(g_rdram + (address & 0x01FFFFFFu), &value, 4); }
        bool valid(uint32_t address) { return address >= 0x100000u && address < 0x2000000u; }

        void setReg(R5900Context *ctx, int reg, uint32_t value)
        {
            ctx->r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
        }

        std::string trim(const std::string &text)
        {
            const size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return {};
            return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
        }

        // The condition's seven fields after a rule's or a boost's own (none: always).
        Condition conditionOf(const std::vector<std::string> &parts, size_t first)
        {
            Condition when;
            if (parts.size() < first + 7u)
                return when;
            int f[7];
            for (size_t k = 0; k < 7u; ++k)
                f[k] = std::atoi(parts[first + k].c_str());
            when.innFrom = std::clamp(f[0], 0, 30);
            when.innTo = std::clamp(f[1], 0, 30);
            when.score = std::clamp(f[2], 0, 5);
            when.diffMin = std::clamp(f[3], 0, 99);
            when.diffMax = std::clamp(f[4], 0, 99);
            when.runners = std::clamp(f[5], 0, 4);
            when.outs = std::clamp(f[6], 0, 3);
            return when;
        }

        std::string conditionText(const Condition &when)
        {
            return "," + std::to_string(when.innFrom) + "," + std::to_string(when.innTo) + "," + std::to_string(when.score) + "," +
                   std::to_string(when.diffMin) + "," + std::to_string(when.diffMax) + "," + std::to_string(when.runners) + "," +
                   std::to_string(when.outs);
        }

        // match_script.ini: enabled=0|1, steal=0|1, rule=<own|opp|all|ID>,<bat|pit>,<hr|hit|k|bb> (one line per rule)
        Settings load(const std::filesystem::path &path)
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
                std::vector<std::string> parts;
                for (size_t start = 0;;)
                {
                    const size_t comma = value.find(',', start);
                    parts.push_back(trim(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));
                    if (comma == std::string::npos)
                        break;
                    start = comma + 1;
                }
                const int target = parts[0] == "own" ? -1 : parts[0] == "opp" ? -2 : parts[0] == "all" ? -3 : std::atoi(parts[0].c_str());
                if (key == "enabled")
                    settings.enabled = value == "1";
                else if (key == "steal")
                    settings.steal = value == "1";
                else if (key == "limit")
                    settings.limit = std::atoi(value.c_str());
                else if (key == "bso" && parts.size() >= 4 && (target == -1 || target == -2)) // bso=<own|opp>,<balls>,<strikes>,<outs>[,condition]
                {
                    const int side = target == -1 ? 0 : 1;
                    const int top[3] = {3, 2, 2};
                    BsoRule rule;
                    rule.side = side;
                    for (int k = 0; k < 3; ++k)
                        rule.count[k] = std::clamp(std::atoi(parts[k + 1].c_str()), -1, top[k]);
                    rule.when = conditionOf(parts, 4);
                    if (rule.when.any())
                        settings.bsoRules.push_back(rule);
                    else
                        std::copy(rule.count, rule.count + 3, settings.bso[side]);
                }
                else if (key == "stealrule" && parts.size() >= 2 && (target == -1 || target == -2)) // stealrule=<own|opp>,<safe|out>[,condition]
                {
                    StealRule rule;
                    rule.side = target == -1 ? 0 : 1;
                    rule.safe = parts[1] != "out";
                    rule.when = conditionOf(parts, 2);
                    settings.steals.push_back(rule);
                }
                else if (key == "bsocap")
                    settings.bsoCap = std::clamp(std::atoi(value.c_str()), 0, 99);
                else if (key == "rule" && parts.size() >= 3)
                {
                    Rule rule;
                    rule.target = target;
                    rule.pitcher = parts[1] == "pit";
                    for (int a = 1; a < 5; ++a)
                        if (parts[2] == kActionKeys[a])
                            rule.action = static_cast<Action>(a);
                    rule.when = conditionOf(parts, 3);
                    if (rule.action != Action::None && rule.target >= -3)
                        settings.rules.push_back(rule);
                }
                else if (key == "boost" && parts.size() >= 3) // boost=<target>,<stat key>,<percent>
                {
                    Boost boost;
                    boost.target = target;
                    boost.stat = -1;
                    for (int s = 0; s < kStatCount; ++s)
                        if (parts[1] == kStats[s].key)
                            boost.stat = s;
                    boost.percent = std::atoi(parts[2].c_str());
                    boost.add = parts.size() >= 4 ? std::atoi(parts[3].c_str()) : 0;
                    boost.when = conditionOf(parts, 4);
                    if (boost.stat >= 0 && boost.target >= -3 && boost.percent > 0)
                        settings.boosts.push_back(boost);
                }
            }
            return settings;
        }

        std::filesystem::path g_menuPath; // the menu's file: this save's (menu thread)

        void save(const Settings &settings)
        {
            if (g_menuPath.empty())
                return; // before any game: nothing to keep it for
            const std::filesystem::path temp = g_menuPath.string() + ".tmp";
            {
                std::ofstream file(temp, std::ios::binary | std::ios::trunc);
                file << "# 경기 스크립트 (게임 메뉴 F2 > 경기 스크립트 탭이 씀)\r\n";
                file << "# rule=<own|opp|all|선수 ID>,<bat|pit>,<hr|hit|k|bb>  steal=1: 내 구단 도루 무조건 성공\r\n";
                file << "# limit=한 이닝에 강제하는 홈런·안타·볼넷 수  boost=<own|opp|all|선수 ID>,<능력치>,<배율 %>,<더하는 값>\r\n";
                file << "# 끝의 7칸 = 조건: 이닝부터,이닝까지,점수(1 이김 2 짐 3 동점 4 이기거나 동점 5 지거나 동점),점수차 최소,최대,주자(1 없음 2 있음 3 득점권 4 만루),아웃(1+아웃 수)\r\n";
                file << "enabled=" << (settings.enabled ? 1 : 0) << "\r\n";
                file << "steal=" << (settings.steal ? 1 : 0) << "\r\n";
                file << "limit=" << settings.limit << "\r\n";
                file << "# bso=<own 내 구단 공격 | opp 상대 공격>,<볼>,<스트라이크>,<아웃> (-1 그대로)  bsocap=아웃 고정(0·1)을 멈추는 한 이닝 득점 (0 제한 없음)\r\n";
                file << "bso=own," << settings.bso[0][0] << ',' << settings.bso[0][1] << ',' << settings.bso[0][2] << "\r\n";
                file << "bso=opp," << settings.bso[1][0] << ',' << settings.bso[1][1] << ',' << settings.bso[1][2] << "\r\n";
                for (const BsoRule &rule : settings.bsoRules) // the same with the 7 condition fields
                    file << "bso=" << (rule.side == 0 ? "own" : "opp") << ',' << rule.count[0] << ',' << rule.count[1] << ',' << rule.count[2]
                         << conditionText(rule.when) << "\r\n";
                file << "bsocap=" << settings.bsoCap << "\r\n";
                file << "# stealrule=<own 내 구단 도루 | opp 상대 도루>,<safe 성공 | out 실패>[,조건 7칸]\r\n";
                for (const StealRule &rule : settings.steals)
                    file << "stealrule=" << (rule.side == 0 ? "own" : "opp") << ',' << (rule.safe ? "safe" : "out")
                         << (rule.when.any() ? conditionText(rule.when) : std::string()) << "\r\n";
                auto targetKey = [](int target) {
                    return target == -1 ? std::string("own") : target == -2 ? std::string("opp") : target == -3 ? std::string("all") : std::to_string(target);
                };
                for (const Rule &rule : settings.rules)
                    file << "rule=" << targetKey(rule.target) << ',' << (rule.pitcher ? "pit" : "bat") << ','
                         << kActionKeys[static_cast<int>(rule.action)] << (rule.when.any() ? conditionText(rule.when) : std::string()) << "\r\n";
                for (const Boost &boost : settings.boosts)
                    file << "boost=" << targetKey(boost.target) << ',' << kStats[boost.stat].key << ',' << boost.percent << ','
                         << boost.add << (boost.when.any() ? conditionText(boost.when) : std::string()) << "\r\n";
            }
            std::error_code error;
            std::filesystem::rename(temp, g_menuPath, error); // whole file at once: the game thread reads it when it changes
        }

        uint32_t g_generation = ~0u; // urban::saveid::generation() g_path belongs to (game thread)

        void reloadIfChanged()
        {
            // The settings are per save (owner, 11-08): cheat_saves/<save>/match_script.ini; none before any game.
            if (const uint32_t generation = urban::saveid::generation(); generation != g_generation)
            {
                g_generation = generation;
                g_path = urban::saveid::settingsFile(urban::saveid::current(), "match_script.ini");
                g_haveStamp = false;
            }
            if (g_path.empty())
            {
                g_settings = Settings{};
                g_haveStamp = false;
                return;
            }
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(g_path, error);
            if (error)
            {
                g_settings = Settings{};
                g_haveStamp = false;
                return;
            }
            if (g_haveStamp && stamp == g_stamp)
                return;
            g_stamp = stamp;
            g_haveStamp = true;
            g_settings = load(g_path);
            std::fprintf(stderr, "[script] match scripts %s, %zu rules, %zu boosts, limit %d per half-inning, steals %s (+%zu rules), bso own %d/%d/%d opp %d/%d/%d (+%zu with conditions) cap %d\n",
                         g_settings.enabled ? "on" : "off", g_settings.rules.size(), g_settings.boosts.size(), g_settings.limit,
                         g_settings.steal ? "always safe" : "as they are", g_settings.steals.size(), g_settings.bso[0][0], g_settings.bso[0][1],
                         g_settings.bso[0][2], g_settings.bso[1][0], g_settings.bso[1][1], g_settings.bso[1][2], g_settings.bsoRules.size(),
                         g_settings.bsoCap);
        }

        bool isOwn(uint16_t id)
        {
            for (int team = 0; team < kTeams; ++team)
            {
                const uint32_t object = r32(kTeamTable + 4u * static_cast<uint32_t>(team));
                if (!valid(object))
                    continue;
                const uint32_t roster = r32(object + 0x120u);
                if (!valid(roster) || r32(roster + 4u) != kOwnRosterClass)
                    continue;
                for (uint32_t k = 0; k < 40; ++k)
                {
                    const uint32_t record = r32(roster + 8u + 4u * k);
                    if (valid(record) && r16(record) == id)
                        return true;
                }
            }
            return false;
        }

        uint16_t batterId(uint32_t rg)
        {
            const uint32_t batter = r32(rg + 0xD4u); // the batter's match copy
            return valid(batter) ? r16(batter) : 0;
        }

        uint16_t pitcherId()
        {
            const uint32_t match = r32(kMatchPtr);
            if (!valid(match))
                return 0;
            const uint32_t fielding = r32(match + 0x18u) == 0u ? 1u : 0u;
            for (uint32_t i = 0; i < 10; ++i)
            {
                if (r8(match + 0x6Cu + fielding * 10u + i) != 0u) // fielding position 0 = pitcher
                    continue;
                const int slot = static_cast<int8_t>(r8(match + 0x1Cu + fielding * 0x14u + i * 2u));
                const uint32_t team = r32(fielding ? kTeamObj1 : kTeamObj0);
                if (slot < 0 || !valid(team))
                    return 0;
                const uint32_t record = r32(team + 0x2008u + 4u * static_cast<uint32_t>(slot));
                return valid(record) ? r16(record) : 0;
            }
            return 0;
        }

        // The batter's and the pitcher's club this pitch (own roster or not), set once per pitch by the swing hook.
        bool g_batterOwn = false, g_pitcherOwn = false;

        // A group target: -1 the own club's players, -2 the opponents in the own club's games, -3 everyone (every game,
        // the CPU clubs' games too: those are decided by the same code when the schedule moves on).
        bool groupMatches(int target, bool selfOwn, bool otherOwn)
        {
            return target == -3 || (target == -1 && selfOwn) || (target == -2 && !selfOwn && otherOwn);
        }

        // The pitch's situation (read once per pitch by the swing hook).
        struct Situation
        {
            bool valid = false;
            int inning = 0;  // from 1
            int batting = 0; // the batting club: 0 top, 1 bottom
            int outs = 0;
            int runs[2] = {0, 0};
            bool base[3] = {false, false, false}; // 1B, 2B, 3B
        };
        Situation g_situation;

        Situation readSituation()
        {
            Situation situation;
            const uint32_t match = r32(kMatchPtr);
            if (!valid(match))
                return situation;
            situation.valid = true;
            situation.inning = static_cast<int>(r32(match + 0x14u)) + 1;
            situation.batting = r32(match + 0x18u) != 0u ? 1 : 0;
            situation.outs = static_cast<int>(r32(match + 0xD0u));
            for (uint32_t b = 0; b < 3u; ++b)
                situation.base[b] = r8(match + 0x5874u + 3u * b) != 0xFFu;
            // The line score at +0x5880: a byte per half-inning (1回表, 1回裏, 2回表, ...), 16 innings; the runs are its sums
            // (simulated games' dumps, 2026-11-08: reading +0x5880 / +0x5884 as totals was only right while all runs came
            // in the first inning). +0x58A0 on is something else.
            for (uint32_t k = 0; k < 32u; ++k)
                situation.runs[k & 1u] += r8(match + 0x5880u + k);
            return situation;
        }

        // Whether the pitch's situation meets a condition, as the subject's club sees it (pitcherSide: the fielding club).
        bool holds(const Condition &when, bool pitcherSide)
        {
            if (!when.any())
                return true;
            const Situation &now = g_situation;
            if (!now.valid)
                return false;
            if ((when.innFrom > 0 && now.inning < when.innFrom) || (when.innTo > 0 && now.inning > when.innTo))
                return false;
            const int self = pitcherSide ? 1 - now.batting : now.batting;
            const int lead = now.runs[self] - now.runs[1 - self];
            if ((when.score == 1 && lead <= 0) || (when.score == 2 && lead >= 0) || (when.score == 3 && lead != 0) ||
                (when.score == 4 && lead < 0) || (when.score == 5 && lead > 0))
                return false;
            const int gap = lead < 0 ? -lead : lead;
            if ((when.diffMin > 0 && gap < when.diffMin) || (when.diffMax > 0 && gap > when.diffMax))
                return false;
            const int on = (now.base[0] ? 1 : 0) + (now.base[1] ? 1 : 0) + (now.base[2] ? 1 : 0);
            if ((when.runners == 1 && on != 0) || (when.runners == 2 && on == 0) || (when.runners == 3 && !now.base[1] && !now.base[2]) ||
                (when.runners == 4 && on != 3))
                return false;
            return when.outs == 0 || now.outs == when.outs - 1;
        }

        Action pick(uint16_t batter, uint16_t pitcher)
        {
            const std::vector<Rule> &rules = g_settings.rules;
            // A rule with a condition before one without (the same target and role): the conditional one wins when it holds.
            auto byId = [&rules](uint16_t id, bool pitcherRule) {
                for (const bool conditional : {true, false})
                    for (const Rule &rule : rules)
                        if (rule.pitcher == pitcherRule && rule.target == id && rule.when.any() == conditional && holds(rule.when, pitcherRule))
                            return rule.action;
                return Action::None;
            };
            auto byGroup = [&rules](bool pitcherRule) {
                const bool self = pitcherRule ? g_pitcherOwn : g_batterOwn, other = pitcherRule ? g_batterOwn : g_pitcherOwn;
                for (const bool conditional : {true, false})
                    for (const Rule &rule : rules)
                        if (rule.pitcher == pitcherRule && rule.target < 0 && groupMatches(rule.target, self, other) &&
                            rule.when.any() == conditional && holds(rule.when, pitcherRule))
                            return rule.action;
                return Action::None;
            };
            // A picked player's rule first, the batter before the pitcher; then the group rules.
            if (batter)
                if (const Action action = byId(batter, false); action != Action::None)
                    return action;
            if (pitcher)
                if (const Action action = byId(pitcher, true); action != Action::None)
                    return action;
            if (batter)
                if (const Action action = byGroup(false); action != Action::None)
                    return action;
            if (pitcher)
                if (const Action action = byGroup(true); action != Action::None)
                    return action;
            return Action::None;
        }

        void restoreSign()
        {
            if (!g_injected)
                return;
            w32(g_rg + kSign, g_saved[0]);
            w32(g_rg + kSignSwing, g_saved[1]);
            w32(g_rg + kSignContact, g_saved[2]);
            w32(g_rg + kSignOut, g_saved[3]);
            g_injected = false;
        }

        void log(Action action, const char *what, uint32_t from, uint32_t to)
        {
            std::fprintf(stderr, "[script] batter %u vs pitcher %u (%s): %s 0x%x -> 0x%x\n", g_batter, g_pitcher,
                         kActionKeys[static_cast<int>(action)], what, from, to);
        }

        // A stat's factor for the batter or the pitcher: a picked player's boost first, then the groups.
        struct Factor
        {
            int percent = 100;
            int add = 0;
            int cap = 0; // the 이치넨 tab's final-value cap (0 = none), after percent and add
            bool identity() const { return percent == 100 && add == 0 && cap == 0; }
        };

        Factor boostFor(uint16_t id, int stat, bool pitcherSide)
        {
            for (const bool conditional : {true, false})
                for (const Boost &boost : g_settings.boosts)
                    if (boost.stat == stat && boost.target == id && boost.when.any() == conditional && holds(boost.when, pitcherSide))
                        return Factor{boost.percent, boost.add};
            const bool self = pitcherSide ? g_pitcherOwn : g_batterOwn, other = pitcherSide ? g_batterOwn : g_pitcherOwn;
            for (const bool conditional : {true, false})
                for (const Boost &boost : g_settings.boosts)
                    if (boost.stat == stat && boost.target < 0 && groupMatches(boost.target, self, other) && boost.when.any() == conditional &&
                        holds(boost.when, pitcherSide))
                        return Factor{boost.percent, boost.add};
            return Factor{};
        }

        struct Field
        {
            uint32_t address;
            Factor factor;
            uint16_t player = 0; // for the cap's report
            int stat = -1;       // urban::ichinen::CapStat
        };

        // The final-value cap of a player's stat (urban::ichinen::CapStat), 0 = none.
        int capOf(uint16_t player, int stat)
        {
            return player ? urban::ichinen::finalCap(player, isOwn(player), stat) : 0;
        }

        // Writes base x percent + add over the fields; a field back at x1 +0 gets its base again. key is the player (and
        // object): a new one starts from the values the game set.
        void applyStack(Stacked &stack, uint32_t key, const std::vector<Field> &fields)
        {
            static const bool debug = std::getenv("URBAN_SCRIPT_DEBUG") != nullptr;
            const bool fresh = stack.object != key;
            if (fresh)
            {
                stack.object = key;
                stack.base.clear();
                stack.written.clear();
            }
            std::vector<std::pair<uint32_t, uint32_t>> base, written;
            for (const Field &field : fields)
            {
                uint32_t value = r32(field.address);
                for (size_t i = 0; i < stack.written.size(); ++i)
                    if (stack.written[i].first == field.address && stack.written[i].second == value)
                        value = stack.base[i].second; // still ours: the game's value is the base kept
                if (field.factor.identity())
                {
                    w32(field.address, value);
                    continue;
                }
                const int64_t boosted = static_cast<int64_t>(static_cast<int32_t>(value)) * field.factor.percent / 100 + field.factor.add;
                uint32_t result = static_cast<uint32_t>(std::clamp<int64_t>(boosted, 0, 9999));
                if (field.factor.cap > 0)
                {
                    result = std::min(result, static_cast<uint32_t>(field.factor.cap));
                    urban::ichinen::noteFinal(field.player, field.stat, static_cast<int>(value), static_cast<int>(result));
                    static const bool capLog = std::getenv("URBAN_CAP_LOG") != nullptr;
                    if (capLog && result != value)
                        std::fprintf(stderr, "[cap] player %u stat %d %u -> %u\n", field.player, field.stat, value, result);
                }
                w32(field.address, result);
                base.emplace_back(field.address, value);
                written.emplace_back(field.address, result);
            }
            // Fields boosted before and not listed now: their base goes back if nothing changed them since.
            for (size_t i = 0; i < stack.written.size(); ++i)
            {
                bool listed = false;
                for (const Field &field : fields)
                    listed = listed || field.address == stack.written[i].first;
                if (!listed && r32(stack.written[i].first) == stack.written[i].second)
                    w32(stack.written[i].first, stack.base[i].second);
            }
            if (debug && fresh && !base.empty())
            {
                std::string text;
                for (size_t i = 0; i < base.size(); ++i)
                    text += " " + std::to_string(base[i].first & 0xFFFu) + ":" + std::to_string(base[i].second) + "->" + std::to_string(written[i].second);
                std::fprintf(stderr, "[script] stack key %08x batter %u pitcher %u:%s\n", key, g_batter, g_pitcher, text.c_str());
            }
            stack.base = std::move(base);
            stack.written = std::move(written);
        }

        // 타구 비거리: the chance (0-100) that a hit to the shallow outfield (zones 18, 21, 24, in front of the
        // fielders, where sub_32BB30 rolls no HR) goes to the deep outfield of the same side instead. x N gives
        // 1 - 1/N, + C adds C points.
        int g_deepChance = 0;

        int deepChance(const Factor &factor)
        {
            const int fromFactor = factor.percent > 100 ? 100 - 10000 / factor.percent : 0;
            return std::clamp(fromFactor + factor.add, 0, 100);
        }

        void applyBoosts(R5900Context *ctx, uint32_t rg)
        {
            g_deepChance = 0;
            const bool caps = urban::ichinen::finalCapsOn();
            if (g_batterStack.written.empty() && g_pitcherStack.written.empty() && (!g_settings.enabled || g_settings.boosts.empty()) && !caps)
                return;
            const bool on = g_settings.enabled && !g_settings.boosts.empty();
            // Batter: the at-bat fields of rg; the batter's match copy pointer tells at-bats apart.
            std::vector<Field> fields;
            Factor dominance;
            for (int s = 0; s < kStatCount; ++s)
                if (kStats[s].pitcher && kStats[s].offset == kDominance && on && g_pitcher)
                    dominance = boostFor(g_pitcher, s, true);
            for (int s = 0; s < kStatCount; ++s)
            {
                if (kStats[s].pitcher)
                    continue;
                Factor factor = on && g_batter ? boostFor(g_batter, s, false) : Factor{};
                if (kStats[s].offset == kDistance)
                {
                    g_deepChance = deepChance(factor);
                    continue;
                }
                if (!dominance.identity() && (kStats[s].offset == 0xACu || kStats[s].offset == 0xB8u))
                {
                    // The pitcher's 탈삼진력: the facing batter's 巧打 and 選球眼 divided by its multiplier, less its value.
                    factor.percent = std::max(1, factor.percent * 100 / std::max(1, dominance.percent));
                    factor.add -= dominance.add;
                }
                if (caps)
                    factor.cap = capOf(g_batter, s); // kStats' first ten are the cap stats
                fields.push_back(Field{rg + kStats[s].offset, factor, g_batter, s});
            }
            if (caps) // the batter's 精神 (+0xC8, sub_328A50 from the copy's +0x68 and skill kind 4): a cap only
                fields.push_back(Field{rg + 0xC8u, Factor{100, 0, capOf(g_batter, urban::ichinen::kCapMental)}, g_batter,
                                       urban::ichinen::kCapMental});
            applyStack(g_batterStack, r32(rg + 0xD4u), fields); // the batter's match copy: a new at-bat, new values
            // Pitcher: the pitch object [gp-0x7FF0] (sub_2B02D0), read by the next pitch's choice.
            const uint32_t pitch = r32(getRegU32(ctx, 28) - 0x7FF0u);
            if (!valid(pitch))
                return;
            fields.clear();
            for (int s = 0; s < kStatCount; ++s)
            {
                if (!kStats[s].pitcher || kStats[s].offset == kDominance)
                    continue;
                Factor factor = on && g_pitcher ? boostFor(g_pitcher, s, true) : Factor{};
                if (caps)
                    factor.cap = capOf(g_pitcher, s);
                if (kStats[s].offset == 0x14u)
                {
                    for (uint32_t k = 0; k < 10; ++k)
                        if (r32(pitch + 0x10u + 8u * k) != 0u || factor.identity()) // a breaking ball's level (type at +0x10)
                            fields.push_back(Field{pitch + 0x14u + 8u * k, factor, g_pitcher, s});
                }
                else
                    fields.push_back(Field{pitch + kStats[s].offset, factor, g_pitcher, s});
            }
            // 精神 (+0x6C, sub_2B0748 from the copy's +0x84 and skill kind 4) has a cap only.
            if (caps)
                fields.push_back(Field{pitch + 0x6Cu, Factor{100, 0, capOf(g_pitcher, urban::ichinen::kCapMental)}, g_pitcher,
                                       urban::ichinen::kCapMental});
            applyStack(g_pitcherStack, pitch ^ (static_cast<uint32_t>(g_pitcher) << 24), fields);
        }

        // sub_32C690(rg, zone type, zone): called by sub_32BB30 for a hit once its zone is rolled, before the detail and
        // the HR roll read the zone from $s7. A shallow outfield zone becomes the deep one of the same side.
        GuestFunction g_hitKind = nullptr;

        void hitKindHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t zone = getRegU32(ctx, 6);
            if (g_deepChance > 0 && (zone == 18u || zone == 21u || zone == 24u) && getRegU32(ctx, 23) == zone)
            {
                static uint32_t seed = 0x9E3779B9u;
                seed = seed * 1664525u + 1013904223u;
                if (static_cast<int>((seed >> 16) % 100u) < g_deepChance)
                {
                    const uint32_t deeper = zone + 1u + ((seed >> 8) & 1u); // the middle or the deep zone behind
                    setReg(ctx, 6, deeper);
                    setReg(ctx, 23, deeper);
                    static const bool debug = std::getenv("URBAN_SCRIPT_DEBUG") != nullptr;
                    if (debug)
                        std::fprintf(stderr, "[script] batter %u: hit zone %u -> %u\n", g_batter, zone, deeper);
                }
            }
            g_hitKind(rdram, ctx, runtime);
        }

        // URBAN_SCRIPT_STATS: the match phase handlers 1 (sub_2952B8) and 11 (sub_2950A0) of sub_295EA0's table 0x696430,
        // which run the skill checks of timings 1 and 2, counted per plate appearance.
        GuestFunction g_phase1 = nullptr, g_phase11 = nullptr;
        unsigned g_phase1Count = 0, g_phase11Count = 0;

        void phase1Hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_phase1Count;
            g_phase1(rdram, ctx, runtime);
        }

        void phase11Hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_phase11Count;
            g_phase11(rdram, ctx, runtime);
        }

        // Before the swing decision: a sign that makes the batter swing and hit (HR, hit), swing and miss (strikeout)
        // or take the pitch (walk). The sign is put back when the pitch's result is set (the two hooks below).
        // BSO (owner 11-08; the 5ch 改造コード thread 6回裏 #703/#725: アウト 00979314, ストライク 00979318, ボール 00979319 in
        // PCSX2 are this match object's +0xD4 (the copy of the outs at +0xD0), +0xD8 and +0xD9). Written before every pitch by
        // the swing hook (3D, 結果表示 and simulated games alike), so the pitch's result counts on from the held value:
        // balls 3 = any ball is a walk, strikes 2 = any strike is a strikeout, outs 2 = the next out ends the half-inning.
        // Outs held at 0 or 1 never let the third out come (the 5ch poster scored 90 in one inning), so that stops once the
        // batting club has bsoCap runs in this half-inning (the line score byte at +0x5880 + half); 0 = no limit, which a
        // simulated game would never finish. Only the own club's games: in a game of two CPU clubs nothing is written.
        void applyBso(uint32_t match)
        {
            if (!g_settings.enabled || !valid(match) || g_batter == 0u)
                return;
            bool any = !g_settings.bsoRules.empty();
            for (const auto &side : g_settings.bso)
                for (const int value : side)
                    any = any || value >= 0;
            if (!any)
                return;
            const int side = isOwn(g_batter) ? 0 : (g_pitcher != 0u && isOwn(g_pitcher)) ? 1 : -1;
            if (side < 0)
                return;
            // A held count with a condition whose condition holds (the own club's view) wins over the side's plain one.
            const int *want = g_settings.bso[side];
            if (!g_settings.bsoRules.empty())
            {
                g_situation = readSituation();
                for (const BsoRule &rule : g_settings.bsoRules)
                    if (rule.side == side && holds(rule.when, side == 1))
                    {
                        want = rule.count;
                        break;
                    }
            }
            const uint32_t before = r32(match + 0xD0u) << 16 | static_cast<uint32_t>(r8(match + 0xD8u)) << 8 | r8(match + 0xD9u);
            if (want[0] >= 0)
                g_rdram[(match + 0xD9u) & 0x01FFFFFFu] = static_cast<uint8_t>(std::min(want[0], 3));
            if (want[1] >= 0)
                g_rdram[(match + 0xD8u) & 0x01FFFFFFu] = static_cast<uint8_t>(std::min(want[1], 2));
            if (want[2] >= 0)
            {
                const uint32_t half = std::min<uint32_t>(r32(match + 0x14u) * 2u + (r32(match + 0x18u) != 0u ? 1u : 0u), 31u);
                const int runs = r8(match + 0x5880u + half);
                if (want[2] >= 2 || g_settings.bsoCap <= 0 || runs < g_settings.bsoCap)
                {
                    const uint32_t outs = static_cast<uint32_t>(std::min(want[2], 2));
                    w32(match + 0xD0u, outs);
                    w32(match + 0xD4u, outs);
                }
            }
            static const bool stats = std::getenv("URBAN_SCRIPT_STATS") != nullptr;
            const uint32_t after = r32(match + 0xD0u) << 16 | static_cast<uint32_t>(r8(match + 0xD8u)) << 8 | r8(match + 0xD9u);
            if (stats && after != before)
                std::fprintf(stderr, "[script] bso %s: O%u S%u B%u -> O%u S%u B%u\n", side == 0 ? "own" : "opp", before >> 16, (before >> 8) & 0xFFu,
                             before & 0xFFu, after >> 16, (after >> 8) & 0xFFu, after & 0xFFu);
        }

        void swingHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            restoreSign();
            g_action = Action::None;
            const uint32_t rg = getRegU32(ctx, 4);
            reloadIfChanged();
            if (valid(rg) && rg == r32(kRealgamePtr))
            {
                g_batter = batterId(rg);
                g_pitcher = pitcherId();
                const bool anyGroup = [] {
                    for (const Rule &rule : g_settings.rules)
                        if (rule.target < 0)
                            return true;
                    for (const Boost &boost : g_settings.boosts)
                        if (boost.target < 0)
                            return true;
                    return false;
                }();
                g_batterOwn = anyGroup && g_settings.enabled && g_batter && isOwn(g_batter);
                g_pitcherOwn = anyGroup && g_settings.enabled && g_pitcher && isOwn(g_pitcher);
                const uint32_t match = r32(kMatchPtr);
                if (valid(match))
                {
                    const uint32_t half = r32(match + 0x14u) * 2u + r32(match + 0x18u);
                    if (half != g_half)
                    {
                        g_half = half;
                        g_forcedInHalf = 0;
                    }
                }
                applyBso(match);               // before the situation: rule conditions see the held count
                g_situation = readSituation(); // the conditions of rules and stacks look at this pitch's situation
                applyBoosts(ctx, rg);
                // URBAN_SCRIPT_STATS=1: one line per plate appearance ("pa <batter>") and per ball in play ("play <batter>
                // <detail>", HR = 0x15 / 0x1A / 0x1F), to count a player's results over simulated games.
                static const bool stats = std::getenv("URBAN_SCRIPT_STATS") != nullptr;
                static uint32_t lastAtBat = 0;
                if (stats && r32(rg + 0xD4u) != lastAtBat)
                {
                    lastAtBat = r32(rg + 0xD4u);
                    std::fprintf(stderr, "[stat] pa %u vs %u inn %d%s outs %d bases %d%d%d score %d-%d%s ev %u/%u\n", g_batter, g_pitcher,
                                 g_situation.inning, g_situation.batting ? "b" : "t", g_situation.outs, g_situation.base[0] ? 1 : 0,
                                 g_situation.base[1] ? 1 : 0, g_situation.base[2] ? 1 : 0, g_situation.runs[0], g_situation.runs[1],
                                 g_situation.valid ? "" : " (no match state)", g_phase1Count, g_phase11Count);
                    g_phase1Count = g_phase11Count = 0;
                    // the batter's active skills (skill object [0x5E75F8]: side 0x6132, slot rg+0xA4 x 0x35A, flags +0xF0D, IDs +0xEFE)
                    const uint32_t skillObject = r32(0x5E75F8u), matchNow0 = r32(kMatchPtr);
                    if (valid(skillObject) && valid(matchNow0))
                    {
                        const uint32_t base = skillObject + r32(matchNow0 + 0x18u) * 0x6132u + r32(rg + 0xA4u) * 0x35Au;
                        std::string active;
                        for (uint32_t k = 0; k < 15u; ++k)
                            if (r8(base + 0xF0Du + k) == 1u)
                                active += (active.empty() ? "" : ",") + std::to_string(r8(base + 0xEFEu + k)) + (k >= 8u ? "i" : "");
                        if (!active.empty())
                            std::fprintf(stderr, "[stat] skills %u %s\n", g_batter, active.c_str());
                    }
                    static const bool dump = std::getenv("URBAN_SCRIPT_DUMP") != nullptr;
                    const uint32_t matchNow = r32(kMatchPtr);
                    if (dump && valid(matchNow))
                    {
                        std::string hex;
                        char cell[4];
                        for (uint32_t k = 0; k < 0x40u; ++k)
                        {
                            std::snprintf(cell, sizeof(cell), "%02x", r8(matchNow + 0x5870u + k));
                            hex += cell;
                            if ((k & 3u) == 3u)
                                hex += ' ';
                        }
                        std::fprintf(stderr, "[dump] +5870: %s\n", hex.c_str());
                    }
                }
                if (g_settings.enabled && !g_settings.rules.empty())
                    g_action = pick(g_batter, g_pitcher);
                if (g_settings.limit > 0 && g_action != Action::None && g_action != Action::Strikeout && g_forcedInHalf >= g_settings.limit)
                    g_action = Action::None; // the half-inning has had its forced HR / hit / walk results
            }
            if (g_action != Action::None)
            {
                g_rg = rg;
                g_saved[0] = r32(rg + kSign);
                g_saved[1] = r32(rg + kSignSwing);
                g_saved[2] = r32(rg + kSignContact);
                g_saved[3] = r32(rg + kSignOut);
                g_injected = true;
                const bool swing = g_action != Action::Walk;
                const bool contact = g_action == Action::HomeRun || g_action == Action::Hit;
                w32(rg + kSign, 4u);
                w32(rg + kSignSwing, swing ? 1u : 0u);
                w32(rg + kSignContact, contact ? 1u : 0u);
                w32(rg + kSignOut, 0u);
            }
            g_swing(rdram, ctx, runtime);
        }

        void noContactHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            restoreSign();
            const Action action = g_action;
            g_action = Action::None;
            if (action == Action::Strikeout || action == Action::Walk)
            {
                const uint32_t match = r32(kMatchPtr);
                const uint32_t strikes = r8(match + 0xD8u), balls = r8(match + 0xD9u);
                const uint32_t result = action == Action::Strikeout ? (strikes >= 2u ? 6u : 1u) : (balls >= 3u ? 7u : 2u);
                if (result == 7u)
                    ++g_forcedInHalf;
                log(action, "pitch", strikes * 16u + balls, result);
                setReturnU32(ctx, result);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            g_noContact(rdram, ctx, runtime);
        }

        void playSetupHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            restoreSign();
            const Action action = g_action;
            g_action = Action::None;
            const uint32_t rg = getRegU32(ctx, 4);
            if (action == Action::HomeRun || action == Action::Hit)
                ++g_forcedInHalf;
            if (action == Action::HomeRun || (action == Action::Hit && r32(rg + 0x1618u) != 4u))
            {
                // The center HR (zone 23, detail 0x1A) and the center single (zone 22, detail 0x16) of the game's own
                // tables, with the kind and side of a natural hit. $s7 is sub_32BB30's zone: it returns it after this
                // call (sub_32CF38 saves and restores $s7), and sub_329F78 keeps it at rg+0x74.
                const bool homeRun = action == Action::HomeRun;
                const uint32_t zone = homeRun ? 0x17u : 0x16u, detail = homeRun ? 0x1Au : 0x16u;
                const uint32_t before = r32(rg + 0x1618u) * 0x100u + r32(rg + 0x161Cu);
                setReg(ctx, 5, zone);
                setReg(ctx, 6, 1u);
                setReg(ctx, 7, 0u);
                setReg(ctx, 23, zone);
                w32(rg + 0x70u, 1u);
                w32(rg + 0x1618u, 4u);
                w32(rg + 0x161Cu, detail);
                w32(rg + 0x1620u, 0u);
                w32(rg + 0x16D4u, 0u);
                log(action, "in play", before, 0x400u + detail);
            }
            static const bool stats = std::getenv("URBAN_SCRIPT_STATS") != nullptr;
            if (stats)
                std::fprintf(stderr, "[stat] play %u %x %s\n", batterId(rg), r32(rg + 0x161Cu), r32(rg + 0x1618u) == 4u ? "hit" : "out");
            g_playSetup(rdram, ctx, runtime);
        }

        // A steal's result: sub_32DF88 calls this with the base the runner leaves (1 first, 2 second, 3 third).
        void stealHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // URBAN_SCRIPT_STATS=1: every steal's natural result and the runner bytes the judgement read (+0x28 / +0x29).
            static const bool stats = std::getenv("URBAN_SCRIPT_STATS") != nullptr;
            if (stats)
            {
                const uint32_t runners = r32(0x5E8E90u), base = getRegU32(ctx, 5);
                const uint32_t entry = valid(runners) && base <= 3u ? runners + 0x30u * base : 0u;
                const uint32_t rgNow = r32(kRealgamePtr);
                std::fprintf(stderr, "[stat] steal base %u natural %d runner speed %u running %u (%s)\n", base,
                             static_cast<int8_t>(r8(r32(0x5E75F8u) + 0xEB7u)), entry ? r8(entry + 0x28u) : 0u, entry ? r8(entry + 0x29u) : 0u,
                             valid(rgNow) && batterId(rgNow) && isOwn(batterId(rgNow)) ? "own" : "other");
            }
            if (g_settings.enabled && (g_settings.steal || !g_settings.steals.empty()))
            {
                const uint32_t rg = r32(kRealgamePtr);
                const uint16_t batter = valid(rg) ? batterId(rg) : 0;
                // The runners are the batting team's: side 0 when the own club bats, 1 when it fields (its games only).
                const int side = batter && isOwn(batter) ? 0 : (batter && pitcherId() && isOwn(pitcherId())) ? 1 : -1;
                int result = -1; // -1 as the game rolls it, else 1 safe / 0 out
                if (side >= 0 && !g_settings.steals.empty())
                {
                    g_situation = readSituation();
                    for (const bool conditional : {true, false})
                    {
                        for (const StealRule &rule : g_settings.steals)
                            if (rule.side == side && rule.when.any() == conditional && holds(rule.when, side == 1))
                            {
                                result = rule.safe ? 1 : 0;
                                break;
                            }
                        if (result >= 0)
                            break;
                    }
                }
                if (result < 0 && side == 0 && g_settings.steal)
                    result = 1;
                if (result >= 0)
                {
                    std::fprintf(stderr, "[script] steal from base %u (%s runner): %s (natural %d)\n", getRegU32(ctx, 5), side == 0 ? "own" : "opp",
                                 result ? "safe" : "out", static_cast<int8_t>(r8(r32(0x5E75F8u) + 0xEB7u)));
                    setReturnU32(ctx, static_cast<uint32_t>(result));
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
            }
            g_stealResult(rdram, ctx, runtime);
        }

#if defined(URBAN_SCRIPT_MENU)
        constexpr const char *kTeamNames[kTeams] = {"라이온즈", "호크스", "파이터즈", "마린즈", "버팔로즈", "골든이글스",
                                                    "드래곤즈", "스왈로즈", "자이언츠", "타이거즈", "카프", "요코하마"};
        constexpr const char *kBatterActions[] = {"홈런", "안타", "삼진", "볼넷"};
        constexpr const char *kPitcherActions[] = {"홈런 맞기", "안타 맞기", "삼진 잡기", "볼넷 주기"};
        constexpr const char *kTargets[] = {"내 구단 전원", "내 경기의 상대 전원", "모든 경기의 모든 선수"};

        std::mutex g_menuMutex;
        Settings g_menu; // the menu's copy (GS thread)
        int g_pickTeam = 0, g_pickKind = 0, g_groupTarget = 0, g_groupRole = 0, g_groupAction = 0;
        std::set<uint16_t> g_selected; // the checked players (global IDs), menu thread
        int g_boostTarget = 0, g_boostStat = 0;
        Condition g_when; // the condition the next rule or stack gets
        // The rule builder by innings: action 0 = none (the row is skipped), else the Action (1 HR, 2 hit, 3 K, 4 BB).
        struct InningRow
        {
            int from = 1, to = 3, action = 0;
        };
        std::vector<InningRow> g_innRows = {{1, 3, 4}, {4, 6, 2}, {7, 9, 1}}; // the owner's example: 볼넷, 안타, 홈런
        // The one-player panel: the player (global ID, -1 none yet), its own condition, role, result, rows and stat.
        int g_perPlayer = -1, g_perRole = 0, g_perAction = 0, g_perStat = 0;
        Condition g_perWhen;
        std::vector<InningRow> g_perRows = {{1, 3, 4}, {4, 6, 2}, {7, 9, 1}};
        int g_stealPickSide = 0, g_stealPickResult = 0; // the steal entry to add: side, 0 safe / 1 out
        int g_bsoPick[4] = {0, 0, 0, 0};                 // the held count to add: side, then balls / strikes / outs as combo items
        constexpr const char *kScoreModes[] = {"점수 상관없음", "이기고 있을 때", "지고 있을 때", "동점일 때", "이기거나 동점", "지거나 동점"};
        constexpr const char *kRunnerModes[] = {"주자 상관없음", "주자 없을 때", "주자 있을 때", "득점권 (2·3루)", "만루"};
        constexpr const char *kOutModes[] = {"아웃 상관없음", "0아웃", "1아웃", "2아웃"};

        std::string conditionLabel(const Condition &when)
        {
            if (!when.any())
                return "조건 없음 (항상)";
            std::string text;
            auto add = [&text](const std::string &part) { text += (text.empty() ? "" : " · ") + part; };
            if (when.innFrom > 0 || when.innTo > 0)
                add(when.innTo == 0 ? std::to_string(when.innFrom) + "회부터"
                    : when.innFrom == 0 ? std::to_string(when.innTo) + "회까지"
                    : when.innFrom == when.innTo ? std::to_string(when.innFrom) + "회"
                                                 : std::to_string(when.innFrom) + "~" + std::to_string(when.innTo) + "회");
            std::string score = when.score > 0 ? kScoreModes[when.score] : "";
            if (when.diffMin > 0 || when.diffMax > 0)
                score += std::string(score.empty() ? "점수 차 " : " ") +
                         (when.diffMax == 0 ? std::to_string(when.diffMin) + "점 이상"
                          : when.diffMin == 0 ? std::to_string(when.diffMax) + "점 이하"
                          : when.diffMin == when.diffMax ? std::to_string(when.diffMin) + "점 차"
                                                         : std::to_string(when.diffMin) + "~" + std::to_string(when.diffMax) + "점 차");
            if (!score.empty())
                add(score);
            if (when.runners > 0)
                add(kRunnerModes[when.runners]);
            if (when.outs > 0)
                add(kOutModes[when.outs]);
            return text;
        }

        struct Player
        {
            uint16_t id = 0;
            bool pitcher = false;
            std::string label;
        };

        std::string nameOf(uint32_t record)
        {
#if defined(_WIN32)
            char sjis[16];
            std::memcpy(sjis, g_rdram + (record & 0x01FFFFFFu) + 4u, sizeof(sjis));
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
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), text, sizeof(text) - 1, nullptr, nullptr);
            return std::string(text, bytes > 0 ? static_cast<size_t>(bytes) : 0u);
#else
            (void)record;
            return {};
#endif
        }

        int ownTeam()
        {
            for (int team = 0; team < kTeams; ++team)
            {
                const uint32_t object = r32(kTeamTable + 4u * static_cast<uint32_t>(team));
                const uint32_t roster = valid(object) ? r32(object + 0x120u) : 0u;
                if (valid(roster) && r32(roster + 4u) == kOwnRosterClass)
                    return team;
            }
            return -1;
        }

        std::vector<Player> playersOf(int team)
        {
            std::vector<Player> players;
            const uint32_t object = r32(kTeamTable + 4u * static_cast<uint32_t>(team));
            const uint32_t roster = valid(object) ? r32(object + 0x120u) : 0u;
            if (!valid(roster))
                return players;
            for (uint32_t k = 0; k < 40; ++k)
            {
                const uint32_t record = r32(roster + 8u + 4u * k);
                if (!valid(record))
                    continue;
                Player player;
                player.id = r16(record);
                const bool pitcher = r8(record + 0x26u) != 0u;
                player.pitcher = pitcher;
                player.label = "#" + std::to_string(r8(record + 0x17u)) + " " + nameOf(record) + (pitcher ? " (투)" : "");
                players.push_back(player);
            }
            return players;
        }

        // The name of a player by global ID (any club), for the rule list.
        std::string playerName(int id)
        {
            for (int team = 0; team < kTeams; ++team)
            {
                const uint32_t object = r32(kTeamTable + 4u * static_cast<uint32_t>(team));
                const uint32_t roster = valid(object) ? r32(object + 0x120u) : 0u;
                if (!valid(roster))
                    continue;
                for (uint32_t k = 0; k < 40; ++k)
                {
                    const uint32_t record = r32(roster + 8u + 4u * k);
                    if (valid(record) && r16(record) == id)
                        return nameOf(record) + " (" + kTeamNames[team] + ")";
                }
            }
            return "선수 ID " + std::to_string(id);
        }

        void draw(const Pcsx2GsUi *ui)
        {
            std::lock_guard<std::mutex> lock(g_menuMutex);
            // Another save or a new game: that save's settings (none for a new game).
            static uint32_t menuGeneration = ~0u;
            static std::string menuProfile;
            if (const uint32_t generation = urban::saveid::generation(); generation != menuGeneration)
            {
                menuGeneration = generation;
                menuProfile = urban::saveid::current();
                g_menuPath = urban::saveid::settingsFile(menuProfile, "match_script.ini");
                std::error_code error;
                g_menu = !g_menuPath.empty() && std::filesystem::exists(g_menuPath, error) ? load(g_menuPath) : Settings{};
            }
            Settings &settings = g_menu;
            bool changed = false;
            ui->text_disabled(("세이브: " + urban::saveid::describe(menuProfile) + " — 이 세이브의 경기 스크립트").c_str());
            int enabled = settings.enabled ? 1 : 0;
            if (ui->checkbox("경기 스크립트 켜기", &enabled))
            {
                settings.enabled = enabled != 0;
                changed = true;
            }
            ui->text_disabled("3D 경기와 결과 표시·간이 경기(시뮬레이션)에서 고른 타자·투수의 결과를 정합니다.");
            ui->text_disabled("스윙과 화면 연출, 점수와 기록이 그 결과를 따라갑니다.");

            // The player picker (several players, any clubs), shared by the rules and the stacking below.
            const int own = ownTeam();
            std::vector<std::string> teamLabels;
            for (int team = 0; team < kTeams; ++team)
                teamLabels.push_back(std::string(kTeamNames[team]) + (team == own ? " (내 구단)" : ""));
            std::vector<const char *> teamItems;
            for (const std::string &label : teamLabels)
                teamItems.push_back(label.c_str());
            const std::vector<Player> players = playersOf(g_pickTeam);
            const bool havePlayer = !g_selected.empty();
            const std::string picked = havePlayer ? "고른 선수 " + std::to_string(g_selected.size()) + "명" : "고른 선수 (위에서 체크)";
            const char *targets[] = {kTargets[0], kTargets[1], kTargets[2], picked.c_str()};
            // A choice of the target combos: a group (one entry), or every checked player (one entry each).
            auto targetsOf = [&](int choice) {
                std::vector<int> list;
                if (choice < 3)
                    list.push_back(-(choice + 1));
                else
                    for (const uint16_t id : g_selected)
                        list.push_back(id);
                return list;
            };
            auto nameOfTarget = [](int target) { return target >= 0 ? playerName(target) : std::string(kTargets[-target - 1]); };

            ui->separator_text("선수 고르기 (여러 명 체크, 구단을 바꿔 가며 고를 수 있음 · 대상 '고른 선수 N명'에 쓰임)");
            ui->set_next_width(170.0f);
            ui->combo("##pickteam", &g_pickTeam, teamItems.data(), kTeams);
            ui->same_line();
            const char *kinds[] = {"전체", "투수만", "야수만"};
            ui->set_next_width(90.0f);
            ui->combo("##pickkind", &g_pickKind, kinds, 3);
            ui->same_line();
            if (ui->small_button("보이는 선수 모두 체크"))
                for (const Player &player : players)
                    if (g_pickKind == 0 || (g_pickKind == 1) == player.pitcher)
                        g_selected.insert(player.id);
            ui->same_line();
            if (ui->small_button("체크 모두 풀기"))
                g_selected.clear();
            if (players.empty())
                ui->text_disabled("이 구단의 선수 목록을 아직 읽을 수 없습니다 (게임을 불러온 뒤에 나옵니다).");
            else if (ui->begin_child("##pickplayers", 0.0f, 190.0f))
            {
                int column = 0;
                for (const Player &player : players)
                {
                    if (g_pickKind != 0 && (g_pickKind == 1) != player.pitcher)
                        continue;
                    if (column++ % 3 != 0)
                        ui->same_line();
                    ui->push_id(player.id);
                    int on = g_selected.count(player.id) ? 1 : 0;
                    ui->set_next_width(0.0f);
                    if (ui->checkbox(player.label.c_str(), &on))
                    {
                        if (on)
                            g_selected.insert(player.id);
                        else
                            g_selected.erase(player.id);
                    }
                    ui->pop_id();
                }
                ui->end_child();
            }
            else
                ui->end_child();
            if (havePlayer)
            {
                std::string names;
                int shown = 0;
                for (const uint16_t id : g_selected)
                {
                    if (shown++ == 8)
                    {
                        names += " 외 " + std::to_string(g_selected.size() - 8) + "명";
                        break;
                    }
                    names += (names.empty() ? "" : ", ") + playerName(id);
                }
                ui->text_wrapped(("고른 선수 " + std::to_string(g_selected.size()) + "명: " + names).c_str());
            }

            // A condition's inputs (the shared one below and the one-player one), each under its own ID.
            auto conditionInputs = [ui](Condition &when, int id) {
                ui->push_id(7000 + id);
                ui->text("이닝");
                ui->same_line();
                ui->set_next_width(70.0f);
                if (ui->input_int("##innfrom", &when.innFrom, 1))
                    when.innFrom = std::clamp(when.innFrom, 0, 30);
                ui->tooltip("0 = 1회부터");
                ui->same_line();
                ui->text("~");
                ui->same_line();
                ui->set_next_width(70.0f);
                if (ui->input_int("##innto", &when.innTo, 1))
                    when.innTo = std::clamp(when.innTo, 0, 30);
                ui->tooltip("0 = 끝까지 (연장 포함)");
                ui->same_line();
                ui->set_next_width(130.0f);
                ui->combo("##whenscore", &when.score, kScoreModes, 6);
                ui->same_line();
                ui->text("점수 차");
                ui->same_line();
                ui->set_next_width(70.0f);
                if (ui->input_int("##diffmin", &when.diffMin, 1))
                    when.diffMin = std::clamp(when.diffMin, 0, 99);
                ui->same_line();
                ui->text("~");
                ui->same_line();
                ui->set_next_width(70.0f);
                if (ui->input_int("##diffmax", &when.diffMax, 1))
                    when.diffMax = std::clamp(when.diffMax, 0, 99);
                ui->tooltip("점수 차 범위 (0 = 제한 없음). 예: '지고 있을 때' + 1~3 = 1~3점 뒤질 때");
                ui->set_next_width(150.0f);
                ui->combo("##whenrunners", &when.runners, kRunnerModes, 5);
                ui->same_line();
                ui->set_next_width(120.0f);
                ui->combo("##whenouts", &when.outs, kOutModes, 4);
                ui->same_line();
                if (ui->small_button("조건 없음으로"))
                    when = Condition{};
                ui->text_disabled(("지금 조건: " + conditionLabel(when)).c_str());
                ui->pop_id();
            };
            // One result per target, role and condition: the same condition again replaces it, another condition adds one.
            auto addRule = [&settings](const Rule &rule) {
                for (Rule &existing : settings.rules)
                    if (existing.target == rule.target && existing.pitcher == rule.pitcher && existing.when == rule.when)
                    {
                        existing.action = rule.action;
                        return;
                    }
                settings.rules.push_back(rule);
            };
            auto addBoost = [&settings](const Boost &boost) {
                for (const Boost &existing : settings.boosts)
                    if (existing.target == boost.target && existing.stat == boost.stat && existing.when == boost.when)
                        return false;
                settings.boosts.push_back(boost);
                return true;
            };
            auto stackText = [](const Boost &boost) {
                char factor[96];
                const uint32_t offset = kStats[boost.stat].offset;
                if (offset == kDistance && !kStats[boost.stat].pitcher)
                    std::snprintf(factor, sizeof(factor), " x%d.%d %+d (깊은 외야로 갈 확률 %d%%)", boost.percent / 100, (boost.percent % 100) / 10,
                                  boost.add, deepChance(Factor{boost.percent, boost.add}));
                else if (offset == kDominance && kStats[boost.stat].pitcher)
                    std::snprintf(factor, sizeof(factor), " 상대 교타·선구안 ÷%d.%d %+d", boost.percent / 100, (boost.percent % 100) / 10, -boost.add);
                else
                    std::snprintf(factor, sizeof(factor), " x%d.%d %+d", boost.percent / 100, (boost.percent % 100) / 10, boost.add);
                return std::string(kStats[boost.stat].label) + factor;
            };

            // One player at a time (owner, 11-08: with several players checked, an add went to all of them by mistake):
            // what is added here goes to this one player only, with a condition of its own.
            ui->separator_text("선수 한 명씩 설정 (여기서 더하는 규칙·중첩은 이 선수에게만)");
            {
                std::vector<int> perIds;
                auto note = [&perIds](int id) {
                    if (id >= 0 && std::find(perIds.begin(), perIds.end(), id) == perIds.end())
                        perIds.push_back(id);
                };
                for (const uint16_t id : g_selected)
                    note(id);
                for (const Rule &rule : settings.rules)
                    note(rule.target);
                for (const Boost &boost : settings.boosts)
                    note(boost.target);
                if (perIds.empty())
                    ui->text_disabled("위에서 선수를 체크하면 여기서 한 명씩 따로 설정할 수 있습니다 (규칙·중첩이 있는 선수도 여기 나옵니다).");
                else
                {
                    if (std::find(perIds.begin(), perIds.end(), g_perPlayer) == perIds.end())
                        g_perPlayer = perIds.front();
                    std::vector<std::string> perLabels;
                    int perIndex = 0;
                    for (size_t i = 0; i < perIds.size(); ++i)
                    {
                        const int id = perIds[i];
                        const auto count = std::count_if(settings.rules.begin(), settings.rules.end(), [id](const Rule &r) { return r.target == id; }) +
                                           std::count_if(settings.boosts.begin(), settings.boosts.end(), [id](const Boost &b) { return b.target == id; });
                        perLabels.push_back(playerName(id) + (g_selected.count(static_cast<uint16_t>(id)) ? " · 체크됨" : "") + " · 규칙·중첩 " +
                                            std::to_string(count) + "개");
                        if (id == g_perPlayer)
                            perIndex = static_cast<int>(i);
                    }
                    std::vector<const char *> perItems;
                    for (const std::string &label : perLabels)
                        perItems.push_back(label.c_str());
                    ui->text("설정할 선수");
                    ui->same_line();
                    ui->set_next_width(460.0f);
                    if (ui->combo("##perplayer", &perIndex, perItems.data(), static_cast<int>(perItems.size())))
                        g_perPlayer = perIds[static_cast<size_t>(perIndex)];
                    const int id = g_perPlayer;
                    ui->text_disabled("이 선수의 조건 (아래 공통 '발동 조건'과 따로):");
                    conditionInputs(g_perWhen, 1);
                    int removeRule = -1, removeStack = -1;
                    for (size_t i = 0; i < settings.rules.size(); ++i)
                    {
                        const Rule &rule = settings.rules[i];
                        if (rule.target != id)
                            continue;
                        ui->push_id(6000 + static_cast<int>(i));
                        if (ui->small_button("빼기"))
                            removeRule = static_cast<int>(i);
                        ui->same_line();
                        ui->text((std::string(rule.pitcher ? "투수일 때 " : "타자일 때 ") + (rule.pitcher ? kPitcherActions : kBatterActions)[static_cast<int>(rule.action) - 1] +
                                  " · " + conditionLabel(rule.when))
                                     .c_str());
                        ui->pop_id();
                    }
                    for (size_t i = 0; i < settings.boosts.size(); ++i)
                    {
                        const Boost &boost = settings.boosts[i];
                        if (boost.target != id)
                            continue;
                        ui->push_id(6500 + static_cast<int>(i));
                        if (ui->small_button("빼기"))
                            removeStack = static_cast<int>(i);
                        ui->same_line();
                        ui->text(("중첩 " + stackText(boost) + " · " + conditionLabel(boost.when) + " (배율은 아래 '능력치 중첩'에서)").c_str());
                        ui->pop_id();
                    }
                    if (removeRule >= 0)
                    {
                        settings.rules.erase(settings.rules.begin() + removeRule);
                        changed = true;
                    }
                    if (removeStack >= 0)
                    {
                        settings.boosts.erase(settings.boosts.begin() + removeStack);
                        changed = true;
                    }
                    const char *perRoles[] = {"타자", "투수"};
                    ui->set_next_width(70.0f);
                    ui->combo("##perrole", &g_perRole, perRoles, 2);
                    ui->same_line();
                    ui->set_next_width(110.0f);
                    ui->combo("##peraction", &g_perAction, g_perRole ? kPitcherActions : kBatterActions, 4);
                    ui->same_line();
                    if (ui->button("이 선수에게만 더하기##perrule"))
                    {
                        addRule(Rule{id, g_perRole == 1, static_cast<Action>(g_perAction + 1), g_perWhen});
                        changed = true;
                    }
                    // By innings, for this player (its own rows).
                    const char *const *perNames = g_perRole ? kPitcherActions : kBatterActions;
                    const char *perActionItems[] = {"그대로", perNames[0], perNames[1], perNames[2], perNames[3]};
                    int removeRow = -1;
                    for (size_t r = 0; r < g_perRows.size(); ++r)
                    {
                        InningRow &row = g_perRows[r];
                        ui->push_id(6800 + static_cast<int>(r));
                        ui->set_next_width(70.0f);
                        if (ui->input_int("##prowfrom", &row.from, 1))
                            row.from = std::clamp(row.from, 1, 30);
                        ui->same_line();
                        ui->text("~");
                        ui->same_line();
                        ui->set_next_width(70.0f);
                        if (ui->input_int("##prowto", &row.to, 1))
                            row.to = std::clamp(row.to, 0, 30);
                        ui->same_line();
                        ui->text("회");
                        ui->same_line();
                        ui->set_next_width(110.0f);
                        ui->combo("##prowaction", &row.action, perActionItems, 5);
                        ui->same_line();
                        if (ui->small_button("줄 빼기"))
                            removeRow = static_cast<int>(r);
                        ui->pop_id();
                    }
                    if (removeRow >= 0)
                        g_perRows.erase(g_perRows.begin() + removeRow);
                    if (ui->small_button("줄 더하기##perrows"))
                    {
                        InningRow next;
                        if (!g_perRows.empty() && g_perRows.back().to > 0)
                        {
                            next.from = std::min(g_perRows.back().to + 1, 30);
                            next.to = std::min(next.from + 2, 30);
                        }
                        g_perRows.push_back(next);
                    }
                    ui->same_line();
                    if (ui->button("이 선수에게만 이닝별로 더하기##perrows"))
                    {
                        for (const InningRow &row : g_perRows)
                            if (row.action > 0)
                            {
                                Condition when = g_perWhen;
                                when.innFrom = row.from;
                                when.innTo = row.to;
                                addRule(Rule{id, g_perRole == 1, static_cast<Action>(row.action), when});
                            }
                        changed = true;
                    }
                    std::vector<const char *> perStats;
                    for (const Stat &stat : kStats)
                        perStats.push_back(stat.label);
                    ui->set_next_width(200.0f);
                    ui->combo("##perstat", &g_perStat, perStats.data(), kStatCount);
                    ui->same_line();
                    if (ui->button("이 선수에게만 중첩 더하기 (x2.0)##perstack") && addBoost(Boost{id, g_perStat, 200, 0, g_perWhen}))
                        changed = true;
                    ui->text_disabled("역할·결과·이닝 줄은 이 선수 것만입니다. 위 체크(고른 선수)와 상관없이 이 선수 한 명에게만 더합니다.");
                }
            }

            // The condition the next rule or stack gets.
            ui->separator_text("발동 조건 (아래에서 '더하기'하는 규칙·중첩에 붙음)");
            ui->text_disabled("점수·이닝은 그 선수 구단 기준입니다: 타자 규칙·타자 능력치는 공격하는 쪽, 투수 규칙·투수 능력치는 수비하는 쪽.");
            conditionInputs(g_when, 0);
            const bool bulk = g_selected.size() > 1u;
            const std::string bulkWarning = "대상이 '고른 선수 " + std::to_string(g_selected.size()) + "명'이면 " + std::to_string(g_selected.size()) +
                                            "명 모두에게 더합니다. 한 명만 바꾸려면 위 '선수 한 명씩 설정'을 쓰세요.";

            // Forced results
            ui->separator_text("결과 강제 (먼저 맞는 것: 고른 선수 타자 > 고른 선수 투수 > 전체 타자 > 전체 투수, 조건 있는 규칙이 먼저)");
            if (settings.rules.empty())
                ui->text_disabled("규칙이 없습니다.");
            int remove = -1;
            for (size_t i = 0; i < settings.rules.size(); ++i)
            {
                const Rule &rule = settings.rules[i];
                const char *what = (rule.pitcher ? kPitcherActions : kBatterActions)[static_cast<int>(rule.action) - 1];
                ui->push_id(static_cast<int>(i));
                if (ui->small_button("빼기"))
                    remove = static_cast<int>(i);
                ui->same_line();
                ui->text((nameOfTarget(rule.target) + " · " + (rule.pitcher ? "투수일 때 " : "타자일 때 ") + what + " · " +
                          conditionLabel(rule.when))
                             .c_str());
                ui->same_line();
                if (ui->small_button("위 조건 붙이기"))
                {
                    settings.rules[i].when = g_when;
                    changed = true;
                }
                ui->tooltip("위 '발동 조건'에 지금 정한 조건을 이 규칙에 붙입니다. '조건 없음으로'를 누른 뒤 붙이면 조건이 떨어집니다.");
                ui->pop_id();
            }
            if (remove >= 0)
            {
                settings.rules.erase(settings.rules.begin() + remove);
                changed = true;
            }
            const char *roles[] = {"타자", "투수"};
            ui->set_next_width(200.0f);
            ui->combo("##ruletarget", &g_groupTarget, targets, havePlayer ? 4 : 3);
            ui->same_line();
            ui->set_next_width(70.0f);
            ui->combo("##rulerole", &g_groupRole, roles, 2);
            ui->same_line();
            ui->set_next_width(110.0f);
            ui->combo("##ruleaction", &g_groupAction, g_groupRole ? kPitcherActions : kBatterActions, 4);
            ui->same_line();
            if (ui->button("이 조건으로 더하기##rule") && (g_groupTarget < 3 || havePlayer))
            {
                for (const int target : targetsOf(g_groupTarget))
                    addRule(Rule{target, g_groupRole == 1, static_cast<Action>(g_groupAction + 1), g_when});
                changed = true;
            }
            if (bulk && g_groupTarget == 3)
                ui->text_colored(0xFF5050FFu, bulkWarning.c_str());
            ui->text_disabled("같은 선수에게 조건이 다른 규칙을 여러 개 둘 수 있습니다: 위 '발동 조건'을 바꿔 가며 더하거나, 아래 이닝별 표로 한 번에 넣으세요.");
            // Several results by innings at once (owner, 11-08: "1~3은 볼넷, 4~6은 안타, 7~9는 홈런"), for the target and role above.
            if (ui->collapsing_header("이닝별로 한꺼번에 더하기 (대상·역할은 바로 위 칸)", 0))
            {
                const char *const *names = g_groupRole ? kPitcherActions : kBatterActions;
                const char *actionItems[] = {"그대로", names[0], names[1], names[2], names[3]};
                int removeRow = -1;
                for (size_t r = 0; r < g_innRows.size(); ++r)
                {
                    InningRow &row = g_innRows[r];
                    ui->push_id(500 + static_cast<int>(r));
                    ui->set_next_width(70.0f);
                    if (ui->input_int("##rowfrom", &row.from, 1))
                        row.from = std::clamp(row.from, 1, 30);
                    ui->same_line();
                    ui->text("~");
                    ui->same_line();
                    ui->set_next_width(70.0f);
                    if (ui->input_int("##rowto", &row.to, 1))
                        row.to = std::clamp(row.to, 0, 30);
                    ui->same_line();
                    ui->text("회");
                    ui->same_line();
                    ui->set_next_width(110.0f);
                    ui->combo("##rowaction", &row.action, actionItems, 5);
                    ui->same_line();
                    if (ui->small_button("줄 빼기"))
                        removeRow = static_cast<int>(r);
                    ui->pop_id();
                }
                if (removeRow >= 0)
                    g_innRows.erase(g_innRows.begin() + removeRow);
                if (ui->small_button("줄 더하기"))
                {
                    InningRow next;
                    if (!g_innRows.empty() && g_innRows.back().to > 0)
                    {
                        next.from = std::min(g_innRows.back().to + 1, 30);
                        next.to = std::min(next.from + 2, 30);
                    }
                    g_innRows.push_back(next);
                }
                ui->same_line();
                if (ui->button("고른 대상에 이닝별로 더하기##innrows") && (g_groupTarget < 3 || havePlayer))
                {
                    for (const int target : targetsOf(g_groupTarget))
                        for (const InningRow &row : g_innRows)
                            if (row.action > 0)
                            {
                                Condition when = g_when; // its score, runners and outs too
                                when.innFrom = row.from;
                                when.innTo = row.to;
                                addRule(Rule{target, g_groupRole == 1, static_cast<Action>(row.action), when});
                            }
                    changed = true;
                }
                if (bulk && g_groupTarget == 3)
                    ui->text_colored(0xFF5050FFu, bulkWarning.c_str());
                ui->text_disabled("'그대로' 줄은 건너뜁니다. 끝 이닝 0 = 끝까지(연장 포함). 위 '발동 조건'의 점수·주자·아웃 조건도 같이 붙습니다.");
            }
            ui->same_line();
            if (ui->small_button("규칙 모두 빼기") && !settings.rules.empty())
            {
                settings.rules.clear();
                changed = true;
            }
            ui->text(settings.limit > 0 ? ("한 이닝에 강제하는 홈런·안타·볼넷: " + std::to_string(settings.limit) + "번까지").c_str()
                                        : "한 이닝에 강제하는 홈런·안타·볼넷: 제한 없음");
            ui->same_line();
            if (ui->small_button("제한 없음##limit"))
            {
                settings.limit = 0;
                changed = true;
            }
            for (int step : {-5, -1, 1, 5})
            {
                ui->same_line();
                const std::string label = (step > 0 ? "+" : "") + std::to_string(step) + "##limit";
                if (ui->small_button(label.c_str()))
                {
                    settings.limit = std::clamp(settings.limit + step, 0, 99);
                    changed = true;
                }
            }
            ui->text_disabled("제한 없음일 때 팀 전체에 홈런·안타·볼넷을 걸면 아웃이 나오지 않아 이닝이 끝나지 않습니다(게임이 멈춤).");
            ui->text_disabled("횟수를 정하면 그 이닝에 그만큼만 강제하고 나머지는 원래대로 진행합니다. 삼진은 세지 않습니다.");

            ui->separator_text("도루");
            int steal = settings.steal ? 1 : 0;
            if (ui->checkbox("내 구단 도루 무조건 성공 (도루 작전을 내렸을 때)", &steal))
            {
                settings.steal = steal != 0;
                changed = true;
            }
            // Steals with conditions, several (the checkbox above is the plain one, used when none of these holds).
            {
                static const char *const kStealSides[] = {"내 구단 도루", "상대 도루 (내 경기)"};
                static const char *const kStealResults[] = {"무조건 성공", "무조건 실패"};
                int removeSteal = -1;
                for (size_t i = 0; i < settings.steals.size(); ++i)
                {
                    StealRule &rule = settings.steals[i];
                    ui->push_id(3000 + static_cast<int>(i));
                    if (ui->small_button("빼기"))
                        removeSteal = static_cast<int>(i);
                    ui->same_line();
                    ui->text((std::string(kStealSides[rule.side]) + " · " + kStealResults[rule.safe ? 0 : 1] + " · " + conditionLabel(rule.when)).c_str());
                    ui->same_line();
                    if (ui->small_button("위 조건 붙이기##stealwhen"))
                    {
                        rule.when = g_when;
                        changed = true;
                    }
                    ui->pop_id();
                }
                if (removeSteal >= 0)
                {
                    settings.steals.erase(settings.steals.begin() + removeSteal);
                    changed = true;
                }
                ui->set_next_width(170.0f);
                ui->combo("##stealside", &g_stealPickSide, kStealSides, 2);
                ui->same_line();
                ui->set_next_width(120.0f);
                ui->combo("##stealresult", &g_stealPickResult, kStealResults, 2);
                ui->same_line();
                if (ui->button("이 조건으로 더하기##steal"))
                {
                    const StealRule rule{g_stealPickSide, g_stealPickResult == 0, g_when};
                    bool replaced = false;
                    for (StealRule &existing : settings.steals)
                        if (existing.side == rule.side && existing.when == rule.when)
                        {
                            existing.safe = rule.safe;
                            replaced = true;
                        }
                    if (!replaced)
                        settings.steals.push_back(rule);
                    changed = true;
                }
                ui->text_disabled("위 '발동 조건'이 붙습니다(예: 7~9회만 성공, 상대 도루는 늘 실패). 조건 있는 줄이 먼저, 그다음 조건 없는 줄, 맨 끝이 위 체크입니다.");
            }

            // BSO count (5ch 改造コード 6回裏 #725)
            ui->separator_text("BSO 카운트 고정 (매 투구 직전에 씀, 내 구단 경기만)");
            {
                static const char *const kBallItems[] = {"그대로", "0", "1", "2", "3"};
                static const char *const kCountItems[] = {"그대로", "0", "1", "2"};
                static const char *const kSides[] = {"내 구단 공격 때", "상대 공격 때"};
                static const char *const kParts[] = {"볼", "스트라이크", "아웃"};
                for (int side = 0; side < 2; ++side)
                {
                    ui->push_id(200 + side);
                    ui->text(kSides[side]);
                    for (int part = 0; part < 3; ++part)
                    {
                        ui->same_line_at(120.0f + 150.0f * static_cast<float>(part));
                        ui->text(kParts[part]);
                        ui->same_line();
                        ui->push_id(part);
                        int pick = settings.bso[side][part] + 1;
                        ui->set_next_width(70.0f);
                        if (ui->combo("##bso", &pick, part == 0 ? kBallItems : kCountItems, part == 0 ? 5 : 4))
                        {
                            settings.bso[side][part] = pick - 1;
                            changed = true;
                        }
                        ui->pop_id();
                    }
                    ui->pop_id();
                }
                // Held counts with a condition, several; the first one that holds wins over the side's row above.
                int removeBso = -1;
                auto countText = [](const char *name, int value) { return std::string(name) + " " + (value < 0 ? std::string("그대로") : std::to_string(value)); };
                for (size_t i = 0; i < settings.bsoRules.size(); ++i)
                {
                    BsoRule &rule = settings.bsoRules[i];
                    ui->push_id(4000 + static_cast<int>(i));
                    if (ui->small_button("빼기"))
                        removeBso = static_cast<int>(i);
                    ui->same_line();
                    ui->text((std::string(kSides[rule.side]) + " · " + countText("볼", rule.count[0]) + " · " + countText("스트라이크", rule.count[1]) +
                              " · " + countText("아웃", rule.count[2]) + " · " + conditionLabel(rule.when))
                                 .c_str());
                    ui->same_line();
                    if (ui->small_button("위 조건 붙이기##bsowhen") && g_when.any())
                    {
                        rule.when = g_when;
                        changed = true;
                    }
                    ui->pop_id();
                }
                if (removeBso >= 0)
                {
                    settings.bsoRules.erase(settings.bsoRules.begin() + removeBso);
                    changed = true;
                }
                ui->text("조건 붙은 고정");
                ui->same_line();
                ui->set_next_width(130.0f);
                ui->combo("##bsopickside", &g_bsoPick[0], kSides, 2);
                for (int part = 0; part < 3; ++part)
                {
                    ui->same_line();
                    ui->text(kParts[part]);
                    ui->same_line();
                    ui->push_id(4500 + part);
                    ui->set_next_width(70.0f);
                    ui->combo("##bsopick", &g_bsoPick[part + 1], part == 0 ? kBallItems : kCountItems, part == 0 ? 5 : 4);
                    ui->pop_id();
                }
                ui->same_line();
                const bool canAddBso = g_when.any();
                if (ui->button("이 조건으로 더하기##bso") && canAddBso)
                {
                    BsoRule rule;
                    rule.side = g_bsoPick[0];
                    for (int part = 0; part < 3; ++part)
                        rule.count[part] = g_bsoPick[part + 1] - 1;
                    rule.when = g_when;
                    bool replaced = false;
                    for (BsoRule &existing : settings.bsoRules)
                        if (existing.side == rule.side && existing.when == rule.when)
                        {
                            std::copy(rule.count, rule.count + 3, existing.count);
                            replaced = true;
                        }
                    if (!replaced)
                        settings.bsoRules.push_back(rule);
                    changed = true;
                }
                ui->text_disabled(canAddBso ? "위 '발동 조건'이 붙습니다(예: 1~3회 상대 공격 때 아웃 2). 맞는 줄이 있으면 위 두 줄보다 먼저 씁니다."
                                            : "조건 붙은 고정은 위 '발동 조건'을 먼저 정해야 더해집니다(조건 없는 고정은 위 두 줄).");
                ui->text_disabled("볼 3 = 볼 하나면 볼넷 · 스트라이크 2 = 스트라이크 하나면 삼진 · 아웃 2 = 아웃 하나면 공수 교대.");
                ui->text_disabled("아웃 0·1 고정 = 세 번째 아웃이 안 나와 이닝이 계속됩니다 (5ch: 1회에 90점). 그래서 아래 득점에서 멈춥니다.");
                ui->text(settings.bsoCap > 0 ? ("아웃 0·1 고정을 멈추는 한 이닝 득점: " + std::to_string(settings.bsoCap) + "점").c_str()
                                             : "아웃 0·1 고정을 멈추는 한 이닝 득점: 제한 없음");
                ui->same_line();
                if (ui->small_button("제한 없음##bsocap"))
                {
                    settings.bsoCap = 0;
                    changed = true;
                }
                for (int step : {-10, -1, 1, 10})
                {
                    ui->same_line();
                    const std::string label = (step > 0 ? "+" : "") + std::to_string(step) + "##bsocap";
                    if (ui->small_button(label.c_str()))
                    {
                        settings.bsoCap = std::clamp(settings.bsoCap + step, 0, 99);
                        changed = true;
                    }
                }
                ui->text_disabled("제한 없음은 3D 경기에서만 쓰세요: 결과 표시·시뮬레이션 경기는 끝나지 않습니다. '경기 스크립트 켜기'가 켜져 있어야 씁니다.");
            }

            // Stacking
            ui->separator_text("능력치 중첩 (배율 x 더하는 값, 99를 넘어서)");
            ui->text_disabled("경기 계산에 쓰는 값 = 원래 값 x 배율 + 더하는 값. 경기용 복사본은 99에서 잘리지만 이 값은 그 뒤라 99를 넘습니다.");
            ui->text_disabled("3D 경기, 결과 표시, 다른 구단 경기(일정 진행) 모두 같습니다. 타자는 그 투구부터, 투수는 다음 투구부터 반영됩니다.");
            ui->text_disabled("교타를 아주 크게 올리면 안타는 늘지만 게임 공식상 홈런 확률은 줄어듭니다. 홈런은 장타 + 타구 비거리를 같이 올리세요.");
            int removeBoost = -1;
            for (size_t i = 0; i < settings.boosts.size(); ++i)
            {
                Boost &boost = settings.boosts[i];
                ui->push_id(1000 + static_cast<int>(i));
                if (ui->small_button("빼기"))
                    removeBoost = static_cast<int>(i);
                ui->same_line();
                char factor[96];
                const uint32_t offset = kStats[boost.stat].offset;
                if (offset == kDistance && !kStats[boost.stat].pitcher)
                    std::snprintf(factor, sizeof(factor), " x%d.%d %+d (깊은 외야로 갈 확률 %d%%)", boost.percent / 100, (boost.percent % 100) / 10,
                                  boost.add, deepChance(Factor{boost.percent, boost.add}));
                else if (offset == kDominance && kStats[boost.stat].pitcher)
                    std::snprintf(factor, sizeof(factor), " 상대 교타·선구안 ÷%d.%d %+d", boost.percent / 100, (boost.percent % 100) / 10, -boost.add);
                else
                    std::snprintf(factor, sizeof(factor), " x%d.%d %+d", boost.percent / 100, (boost.percent % 100) / 10, boost.add);
                ui->text((nameOfTarget(boost.target) + " · " + kStats[boost.stat].label + factor + " · " + conditionLabel(boost.when)).c_str());
                ui->same_line();
                if (ui->small_button("위 조건 붙이기##boostwhen"))
                {
                    boost.when = g_when;
                    changed = true;
                }
                ui->tooltip("위 '발동 조건'에 지금 정한 조건을 이 중첩에 붙입니다. '조건 없음으로'를 누른 뒤 붙이면 조건이 떨어집니다.");
                // The multiplier in steps of 0.1 and 1 (0.1 .. 100), the added value in steps of 1, 10 and 100.
                ui->text_disabled("    배율");
                for (const int step : {-100, -10, 10, 100})
                {
                    ui->same_line();
                    char label[32];
                    std::snprintf(label, sizeof(label), "%s%d.%d##m%d", step > 0 ? "+" : "-", std::abs(step) / 100, (std::abs(step) % 100) / 10, step);
                    if (ui->small_button(label))
                    {
                        boost.percent = std::clamp(boost.percent + step, 10, 10000);
                        changed = true;
                    }
                }
                ui->same_line();
                ui->text_disabled("  더하기");
                for (const int step : {-100, -10, -1, 1, 10, 100})
                {
                    ui->same_line();
                    char label[32];
                    std::snprintf(label, sizeof(label), "%+d##a%d", step, step);
                    if (ui->small_button(label))
                    {
                        boost.add = std::clamp(boost.add + step, -9999, 9999);
                        changed = true;
                    }
                }
                ui->pop_id();
            }
            if (removeBoost >= 0)
            {
                settings.boosts.erase(settings.boosts.begin() + removeBoost);
                changed = true;
            }
            std::vector<const char *> statItems;
            for (const Stat &stat : kStats)
                statItems.push_back(stat.label);
            ui->set_next_width(200.0f);
            ui->combo("##boosttarget", &g_boostTarget, targets, havePlayer ? 4 : 3);
            ui->same_line();
            ui->set_next_width(180.0f);
            ui->combo("##booststat", &g_boostStat, statItems.data(), kStatCount);
            ui->same_line();
            if (ui->button("더하기 (x2.0 +0, 위에서 조절)##boost") && (g_boostTarget < 3 || havePlayer))
            {
                for (const int target : targetsOf(g_boostTarget))
                    if (addBoost(Boost{target, g_boostStat, 200, 0, g_when}))
                        changed = true;
            }
            if (bulk && g_boostTarget == 3)
                ui->text_colored(0xFF5050FFu, bulkWarning.c_str());
            ui->same_line();
            if (ui->small_button("중첩 모두 빼기") && !settings.boosts.empty())
            {
                settings.boosts.clear();
                changed = true;
            }
            if (changed)
                save(settings);
        }
#endif
    }

    void install(PS2Runtime &runtime, uint8_t *rdram, const std::filesystem::path &userDir)
    {
        if (!rdram || !runtime.hasFunction(kSwing) || !runtime.hasFunction(kNoContact) || !runtime.hasFunction(kPlaySetup) ||
            !runtime.hasFunction(kStealResult))
            return;
        const char *mods = std::getenv("URBAN_MODS");
        if (mods && std::strcmp(mods, ";") == 0) // the launcher's safe start
            return;
        g_rdram = rdram;
        (void)userDir; // per save: cheat_saves/<save>/match_script.ini (reloadIfChanged, the menu)
        reloadIfChanged();
        FunctionHooks &hooks = FunctionHooks::instance();
        hooks.replace(kSwing, "match_script_swing", &swingHook, ReplacementStatus::Native, "match scripts: forced results");
        hooks.replace(kNoContact, "match_script_no_contact", &noContactHook, ReplacementStatus::Native, "match scripts");
        hooks.replace(kPlaySetup, "match_script_play_setup", &playSetupHook, ReplacementStatus::Native, "match scripts");
        hooks.replace(kStealResult, "match_script_steal", &stealHook, ReplacementStatus::Native, "match scripts: steals");
        if (runtime.hasFunction(0x32C690u))
        {
            hooks.replace(0x32C690u, "match_script_hit_zone", &hitKindHook, ReplacementStatus::Native, "match scripts: 타구 비거리");
            g_hitKind = hooks.original(0x32C690u);
        }
        g_swing = hooks.original(kSwing);
        g_noContact = hooks.original(kNoContact);
        g_playSetup = hooks.original(kPlaySetup);
        g_stealResult = hooks.original(kStealResult);
        if (std::getenv("URBAN_SCRIPT_STATS") && runtime.hasFunction(0x2952B8u) && runtime.hasFunction(0x2950A0u))
        {
            hooks.replace(0x2952B8u, "match_script_phase1", &phase1Hook, ReplacementStatus::Native, "stats: phase 1 count");
            hooks.replace(0x2950A0u, "match_script_phase11", &phase11Hook, ReplacementStatus::Native, "stats: phase 11 count");
            g_phase1 = hooks.original(0x2952B8u);
            g_phase11 = hooks.original(0x2950A0u);
        }
        if (!g_swing || !g_noContact || !g_playSetup || !g_stealResult || !g_hitKind)
        {
            std::fprintf(stderr, "[script] hooks not installed\n");
            return;
        }
#if defined(URBAN_SCRIPT_MENU)
        g_menu = g_settings;
        ps2x::menu::addPanel("경기 스크립트", [](const Pcsx2GsUi *ui) { draw(ui); });
#endif
    }
}
